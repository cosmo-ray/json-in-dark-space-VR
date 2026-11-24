/*
 * vr.c - "JSON in dark space", OpenGL ES 3.2 / OpenXR front-end.
 *
 * Same data, same layout, same CLI as gl-cube-2.c.  The renderer backend is
 * chosen at compile time:
 *
 *      make vr                 -> -DAPI=API_GLES      (currently the only one)
 *      make vr API=API_VK      -> -DAPI=API_VK        (not implemented yet)
 *
 * Modes:
 *      json-gl-xr < data.json                 OpenXR session (stereo, tracked)
 *      json-gl-xr --desktop < data.json       side-by-side stereo in a window
 *      json-gl-xr --list-xr-extensions        probe the OpenXR runtime
 *
 * Without an OpenXR runtime the binary falls back to --desktop and says so.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ *
 * backend selection (compile time, no vtable, no function pointers)
 * ------------------------------------------------------------------ */
#define API_GLES 1
#define API_VK   2

#ifndef API
#define API API_GLES
#endif

#if API != API_GLES
# error "API_VK is not implemented yet: add the Vulkan branch to the renderer"
#endif

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define GLFW_INCLUDE_ES3
#include <GLFW/glfw3.h>

#include "yyjson.h"
#include "csonpath_yyjson.h"
#include "json_layout.h"
#include "stb_easy_font.h"
#include "xr_result.h"

/*
 * The Khronos header only declares XrGraphicsBindingOpenGLESAndroidKHR when
 * XR_USE_PLATFORM_ANDROID is set, and that pulls in jni.h.  The struct itself
 * is pure EGL (the Vulkan counterpart below is XrGraphicsBindingEGLMNDX), so
 * we declare the ABI-compatible twin here.  Runtimes that expose
 * XR_KHR_opengl_es_enable on desktop Linux accept it as-is.
 */
typedef struct XrGraphicsBindingGLES {
    XrStructureType             type;
    const void *XR_MAY_ALIAS    next;
    EGLDisplay                  display;
    EGLConfig                   config;
    EGLContext                  context;
} XrGraphicsBindingGLES;

#define MAX_VERTICES  (128u * 1024u)   /* panel: expanded triangles */
#define DEG2RAD 0.017453292519943295f

/* ================================================================== *
 * scene
 * ================================================================== */

struct vec3 { float x, y, z; };

struct node {
    uint32_t idx;
    float x, y, z;
    float size;
};

struct scene {
    struct node *v;
    size_t n, cap;
    float extent;              /* max |coordinate| in model space */
    yyjson_val *items;         /* the JSON array */
    const char *path;          /* --main-path or "$" */
};

static void scene_push(struct scene *s, uint32_t idx, float x, float y, float z)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->v = realloc(s->v, s->cap * sizeof *s->v);
        if (!s->v) { perror("realloc"); exit(1); }
    }
    s->v[s->n++] = (struct node){ idx, x, y, z, JL_NODE_SIZE };

    float m = fmaxf(fabsf(x), fmaxf(fabsf(y), fabsf(z)));
    if (m > s->extent) s->extent = m;
}

static void scene_build_cb(uint32_t idx, float x, float y, float z, void *ud)
{
    scene_push(ud, idx, x, y, z);
}

/* ================================================================== *
 * json loading
 * ================================================================== */

static char *read_all(FILE *f, size_t *out_len)
{
    size_t cap = 1u << 16, len = 0;
    char *b = malloc(cap);
    if (!b) { perror("malloc"); exit(1); }

    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) { perror("realloc"); exit(1); }
        }
        size_t r = fread(b + len, 1, cap - len, f);
        if (r == 0) break;
        len += r;
    }
    *out_len = len;
    return b;
}

static yyjson_val *resolve_path(yyjson_val *root, const char *path)
{
    if (!path) return root;

    struct csonpath p;
    if (csonpath_init(&p, path) < 0) {
        fprintf(stderr, "csonpath_init failed\n");
        return NULL;
    }
    yyjson_val *r = csonpath_find_first(&p, root);
    if (!r && p.compile_error)
        fprintf(stderr, "csonpath: %s\n", p.compile_error);
    csonpath_destroy(&p);
    return r;
}

static const char *json_type_name(yyjson_val *v)
{
    if (!v) return "null";
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_OBJ:  return "object";
    case YYJSON_TYPE_ARR:  return "array";
    case YYJSON_TYPE_STR:  return "string";
    case YYJSON_TYPE_NUM:  return yyjson_is_int(v) ? "integer" : "number";
    case YYJSON_TYPE_BOOL: return "bool";
    default:               return "null";
    }
}

/* ================================================================== *
 * math (column major, GL convention: m[col*4+row])
 * ================================================================== */

static void mat4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul(float o[16], const float a[16], const float b[16])
{
    float t[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            t[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                           a[1 * 4 + r] * b[c * 4 + 1] +
                           a[2 * 4 + r] * b[c * 4 + 2] +
                           a[3 * 4 + r] * b[c * 4 + 3];
        }
    }
    memcpy(o, t, sizeof t);
}

static void mat4_persp_fov(float m[16], XrFovf fov, float n, float f)
{
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp),   d = tanf(fov.angleDown);
    float w = r - l, h = u - d;

    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (r + l) / w;
    m[9]  = (u + d) / h;
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
}

static void mat4_persp(float m[16], float fovy, float aspect, float n, float f)
{
    float s = tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 1.0f / (aspect * s);
    m[5]  = 1.0f / s;
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2.0f * f * n / (f - n);
}

/* view matrix from an OpenXR pose: R^T | -R^T p */
static void mat4_view_from_pose(float m[16], XrPosef p)
{
    float x = p.orientation.x, y = p.orientation.y;
    float z = p.orientation.z, w = p.orientation.w;

    float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
    float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
    float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);

    mat4_identity(m);
    m[0] = r00; m[1] = r01; m[2] = r02;
    m[4] = r10; m[5] = r11; m[6] = r12;
    m[8] = r20; m[9] = r21; m[10] = r22;

    float px = p.position.x, py = p.position.y, pz = p.position.z;
    m[12] = -(r00 * px + r10 * py + r20 * pz);
    m[13] = -(r01 * px + r11 * py + r21 * pz);
    m[14] = -(r02 * px + r12 * py + r22 * pz);
}

/* model = T * Ry(yaw) * S  (uniform scale) */
static void mat4_model(float m[16], float scale, float yaw, struct vec3 t)
{
    float c = cosf(yaw), s = sinf(yaw);
    memset(m, 0, 16 * sizeof(float));
    m[0]  =  c * scale;
    m[2]  = -s * scale;
    m[5]  =  scale;
    m[8]  =  s * scale;
    m[10] =  c * scale;
    m[12] = t.x; m[13] = t.y; m[14] = t.z;
    m[15] = 1.0f;
}

/* world -> model for picking (inverse of mat4_model) */
static struct vec3 to_model(struct vec3 p, struct vec3 t, float scale, float yaw)
{
    float c = cosf(yaw), s = sinf(yaw);
    p.x -= t.x; p.y -= t.y; p.z -= t.z;
    struct vec3 r;
    r.x = ( c * p.x - s * p.z) / scale;
    r.y =  p.y / scale;
    r.z = ( s * p.x + c * p.z) / scale;
    return r;
}

static struct vec3 v3(float x, float y, float z) { return (struct vec3){ x, y, z }; }
static struct vec3 v3_add(struct vec3 a, struct vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static struct vec3 v3_sub(struct vec3 a, struct vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static struct vec3 v3_mul(struct vec3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }

static struct vec3 v3_norm(struct vec3 a)
{
    float l = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    if (l < 1e-8f) return v3(0, 0, -1);
    return v3(a.x / l, a.y / l, a.z / l);
}

static struct vec3 v3_cross(struct vec3 a, struct vec3 b)
{
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

/* camera position in world space, from a world->eye view matrix:
 * eye = -R^T * t */
static struct vec3 view_cam_pos(const float m[16])
{
    return v3(-(m[0] * m[12] + m[1] * m[13] + m[2]  * m[14]),
              -(m[4] * m[12] + m[5] * m[13] + m[6]  * m[14]),
              -(m[8] * m[12] + m[9] * m[13] + m[10] * m[14]));
}

/* the camera looks along -Z in eye space, so forward = -R^T * (0,0,1) */
static struct vec3 view_cam_fwd(const float m[16])
{
    return v3_norm(v3(-m[2], -m[6], -m[10]));
}

/* rotate an eye-space direction into world space (R^T * d) */
static struct vec3 view_dir_to_world(const float m[16], struct vec3 d)
{
    return v3(m[0] * d.x + m[1] * d.y + m[2]  * d.z,
              m[4] * d.x + m[5] * d.y + m[6]  * d.z,
              m[8] * d.x + m[9] * d.y + m[10] * d.z);
}


/* rotate -Z by an OpenXR quaternion */
static struct vec3 quat_forward(XrQuaternionf q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    /* R * (0,0,-1) */
    return v3(-2 * (x * z + y * w),
              -2 * (y * z - x * w),
              -(1 - 2 * (x * x + y * y)));
}

/* ================================================================== *
 * picking
 * ================================================================== */

struct hit {
    int found;
    size_t node;
    float t;
};

static struct hit scene_pick(const struct scene *s, struct vec3 ro, struct vec3 rd,
                             struct vec3 t, float scale, float yaw)
{
    struct hit h = { 0, 0, 0.0f };
    struct vec3 mo = to_model(ro, t, scale, yaw);
    struct vec3 md = to_model(rd, v3(0, 0, 0), scale, yaw);
    md = v3_norm(md);

    const float mo3[3] = { mo.x, mo.y, mo.z };
    const float md3[3] = { md.x, md.y, md.z };

    /* one extra iteration for the origin cube at (0,0,0) */
    for (size_t i = 0; i <= s->n; ++i) {
        float x = 0, y = 0, z = 0, size = JL_ORIGIN_SIZE;
        if (i < s->n) {
            const struct node *n = &s->v[i];
            x = n->x; y = n->y; z = n->z; size = n->size;
        }
        float hs = size * 0.5f;
        const float bmin[3] = { x - hs, y - hs, z - hs };
        const float bmax[3] = { x + hs, y + hs, z + hs };
        float tn;
        if (jl_ray_box(mo3, md3, bmin, bmax, &tn) && (!h.found || tn < h.t)) {
            h.found = i < s->n ? 1 : 2;
            h.node = i;
            h.t = tn;
        }
    }
    return h;
}

/* ================================================================== *
 * GL objects
 * ================================================================== */

struct gfx {
    GLuint prog_scene, prog_ui;
    GLuint vao_cube, vbo_cube, ibo_cube;
    GLuint vbo_inst;
    GLuint vao_line, vbo_line;
    GLuint vao_dyn, vbo_dyn;
    GLuint vao_panel, vbo_panel;

    GLint u_scene_vp, u_ui_mvp, u_ui_col;

    size_t n_inst;
};

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader: %s\n", log);
        exit(1);
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint v = compile(GL_VERTEX_SHADER, vs);
    GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link: %s\n", log);
        exit(1);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static const char *VS_SCENE =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec3 aNrm;\n"
    "layout(location=2) in vec3 aCol;\n"
    "layout(location=3) in vec4 aInst;\n"
    "uniform mat4 uVP;\n"
    "out vec3 vCol; out vec3 vNrm;\n"
    "void main(){\n"
    "  vec3 p = aPos * aInst.w + aInst.xyz;\n"
    "  gl_Position = uVP * vec4(p, 1.0);\n"
    "  vCol = aCol; vNrm = aNrm;\n"
    "}\n";

static const char *FS_SCENE =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec3 vCol; in vec3 vNrm;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  vec3 n = normalize(vNrm);\n"
    "  float d = max(dot(n, normalize(vec3(0.35, 0.55, 0.75))), 0.0);\n"
    "  o = vec4(vCol * (0.35 + 0.65 * d), 1.0);\n"
    "}\n";

/* lines and the text panel share one program: the panel carries per-vertex
 * colour in attribute 1, lines leave that attribute at its generic value
 * (1,1,1,1) and get their colour from the uCol uniform */
static const char *VS_UI =
    "#version 300 es\n"
    "layout(location=0) in vec3 aPos;\n"
    "layout(location=1) in vec4 aCol;\n"
    "uniform mat4 uMVP;\n"
    "uniform vec4 uCol;\n"
    "out vec4 vCol;\n"
    "void main(){ vCol = aCol * uCol; gl_Position = uMVP * vec4(aPos, 1.0); }\n";

static const char *FS_UI =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec4 vCol;\n"
    "out vec4 o;\n"
    "void main(){ o = vCol; }\n";

static void gfx_init(struct gfx *g, const struct scene *s)
{
    g->prog_scene = program(VS_SCENE, FS_SCENE);
    g->prog_ui    = program(VS_UI, FS_UI);

    g->u_scene_vp = glGetUniformLocation(g->prog_scene, "uVP");
    g->u_ui_mvp   = glGetUniformLocation(g->prog_ui, "uMVP");
    g->u_ui_col   = glGetUniformLocation(g->prog_ui, "uCol");

    /* lines do not bind attribute 1; keep its generic value opaque white */
    glVertexAttrib4f(1, 1.0f, 1.0f, 1.0f, 1.0f);

    float verts[JL_CUBE_VERTICES * 9];
    unsigned short idx[JL_CUBE_INDICES];
    jl_cube_mesh(verts, idx);

    glGenVertexArrays(1, &g->vao_cube);
    glBindVertexArray(g->vao_cube);

    glGenBuffers(1, &g->vbo_cube);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_cube);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);

    glGenBuffers(1, &g->ibo_cube);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibo_cube);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float), (void *)(6 * sizeof(float)));

    /* per-instance (x, y, z, size) */
    glGenBuffers(1, &g->vbo_inst);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_inst);
    glBufferData(GL_ARRAY_BUFFER, (s->n + 1) * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glVertexAttribDivisor(3, 1);

    /* graph lines */
    glGenVertexArrays(1, &g->vao_line);
    glBindVertexArray(g->vao_line);
    glGenBuffers(1, &g->vbo_line);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_line);
    glBufferData(GL_ARRAY_BUFFER, s->n * 2 * 3 * sizeof(float), NULL, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);

    /* dynamic lines: controller rays and selection box */
    glGenVertexArrays(1, &g->vao_dyn);
    glBindVertexArray(g->vao_dyn);
    glGenBuffers(1, &g->vbo_dyn);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_dyn);
    glBufferData(GL_ARRAY_BUFFER, 64 * 3 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);

    /* panel: 16-byte vertices, position + packed rgba (stb_easy_font layout) */
    glGenVertexArrays(1, &g->vao_panel);
    glBindVertexArray(g->vao_panel);
    glGenBuffers(1, &g->vbo_panel);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_panel);
    glBufferData(GL_ARRAY_BUFFER, MAX_VERTICES * 16, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 16, (void *)12);

    glBindVertexArray(0);

    /* fill static instance and line buffers */
    float *inst = malloc((s->n + 1) * 4 * sizeof(float));
    float *lines = malloc(s->n * 6 * sizeof(float));
    if (!inst || !lines) { perror("malloc"); exit(1); }

    inst[0] = 0; inst[1] = 0; inst[2] = 0; inst[3] = 1.0f;
    for (size_t i = 0; i < s->n; ++i) {
        inst[(i + 1) * 4 + 0] = s->v[i].x;
        inst[(i + 1) * 4 + 1] = s->v[i].y;
        inst[(i + 1) * 4 + 2] = s->v[i].z;
        inst[(i + 1) * 4 + 3] = s->v[i].size;

        lines[i * 6 + 0] = 0; lines[i * 6 + 1] = 0; lines[i * 6 + 2] = 0;
        lines[i * 6 + 3] = s->v[i].x;
        lines[i * 6 + 4] = s->v[i].y;
        lines[i * 6 + 5] = s->v[i].z;
    }

    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_inst);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (s->n + 1) * 4 * sizeof(float), inst);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_line);
    glBufferSubData(GL_ARRAY_BUFFER, 0, s->n * 6 * sizeof(float), lines);

    g->n_inst = s->n + 1;
    free(inst);
    free(lines);

    glEnable(GL_DEPTH_TEST);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
}

static void draw_scene(const struct gfx *g, const struct scene *s, const float vp[16])
{
    glUseProgram(g->prog_scene);
    glUniformMatrix4fv(g->u_scene_vp, 1, GL_FALSE, vp);
    glBindVertexArray(g->vao_cube);
    glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, NULL, (GLsizei)g->n_inst);

    glUseProgram(g->prog_ui);
    glUniformMatrix4fv(g->u_ui_mvp, 1, GL_FALSE, vp);
    glUniform4f(g->u_ui_col, 0.75f, 0.75f, 0.75f, 1.0f);
    glBindVertexArray(g->vao_line);
    glDrawArrays(GL_LINES, 0, (GLsizei)(s->n * 2));

    glBindVertexArray(0);
}

/* draw a small set of world/model space line segments */
static void draw_dyn_lines(const struct gfx *g, const float *pts, int n, const float vp[16],
                           float r, float gg, float b, float a)
{
    glUseProgram(g->prog_ui);
    glUniformMatrix4fv(g->u_ui_mvp, 1, GL_FALSE, vp);
    glUniform4f(g->u_ui_col, r, gg, b, a);
    glBindVertexArray(g->vao_dyn);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_dyn);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(n * 3 * sizeof(float)), pts);
    glDrawArrays(GL_LINES, 0, n);
    glBindVertexArray(0);
}

/* ================================================================== *
 * text panel
 * ================================================================== */

struct panel {
    float vbuf[64 * 1024];       /* 16 bytes per vertex, stb layout */
    int nverts;
    float w, h;                  /* in stb units, scaled */
    char lines[8][256];
    int nlines;
    int dirty;
};

static void panel_clear(struct panel *p)
{
    p->nverts = 0;
    p->w = p->h = 0;
}

static void panel_line(struct panel *p, const char *fmt, ...)
{
    if (p->nlines >= (int)(sizeof p->lines / sizeof p->lines[0])) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->lines[p->nlines++], sizeof p->lines[0], fmt, ap);
    va_end(ap);
}

/* a float whose four bytes are the packed rgba, as stb_easy_font expects */
static float pack_rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    unsigned char c[4] = { r, g, b, a };
    float f;
    memcpy(&f, c, sizeof f);
    return f;
}

static void panel_emit_tri(struct panel *p, const float *a, const float *b, const float *c)
{
    if (p->nverts + 3 > (int)(sizeof p->vbuf / 16)) return;
    memcpy(&p->vbuf[p->nverts * 4], a, 16); p->nverts++;
    memcpy(&p->vbuf[p->nverts * 4], b, 16); p->nverts++;
    memcpy(&p->vbuf[p->nverts * 4], c, 16); p->nverts++;
}

static void panel_emit_quad_vec(struct panel *p, const float *v)
{
    /* stb writes 4 vertices per quad, 4 floats each */
    panel_emit_tri(p, v + 0, v + 4, v + 8);
    panel_emit_tri(p, v + 0, v + 8, v + 12);
}

#define PANEL_PAD 9.0f

static void panel_add_line(struct panel *p, const char *text, float scale, float *y)
{
    static float quadbuf[4096];
    /* *y is in stb units; both glyph axes are scaled identically */
    int n = stb_easy_font_print(0, *y, (char *)text, NULL, quadbuf, (int)sizeof quadbuf);
    for (int i = 0; i < n; ++i) {
        float v[16];
        memcpy(v, &quadbuf[i * 16], sizeof v);
        for (int k = 0; k < 4; ++k) {
            v[k * 4 + 0] = v[k * 4 + 0] * scale + PANEL_PAD;
            v[k * 4 + 1] = v[k * 4 + 1] * scale;
        }
        panel_emit_quad_vec(p, v);
    }
    *y += stb_easy_font_height((char *)text) + 3.0f;   /* stb units */
}

static void panel_build(struct panel *p)
{
    panel_clear(p);

    float scale = 2.0f;
    float y = PANEL_PAD / scale;                 /* stb units */
    float maxw = 0;
    float total = PANEL_PAD * 2.0f;

    for (int i = 0; i < p->nlines; ++i) {
        float w = stb_easy_font_width(p->lines[i]) * scale;
        if (w > maxw) maxw = w;
        total += (stb_easy_font_height(p->lines[i]) + 3.0f) * scale;
    }
    p->w = maxw + PANEL_PAD * 2.0f;
    p->h = total;

    /* background first, in the same 16-byte vertex format as the glyphs */
    {
        float c = pack_rgba(10, 12, 20, 236);
        float v0[4] = { 0,      0, 0, c };
        float v1[4] = { p->w,   0, 0, c };
        float v2[4] = { p->w, p->h, 0, c };
        float v3[4] = { 0,    p->h, 0, c };
        panel_emit_tri(p, v0, v1, v2);
        panel_emit_tri(p, v0, v2, v3);
    }

    for (int i = 0; i < p->nlines; ++i)
        panel_add_line(p, p->lines[i], scale, &y);

    p->dirty = 0;
}

static void panel_upload(const struct panel *p, struct gfx *g)
{
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo_panel);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)(p->nverts * 16), p->vbuf);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/* ================================================================== *
 * app state
 * ================================================================== */

struct app {
    struct scene scene;
    struct gfx gfx;
    struct panel panel;

    int selected;                 /* -1: none, -2: origin */
    int hovered;
    int desktop;
    float yaw, scale;
    struct vec3 offset;

    struct vec3 ray_o[2], ray_d[2];
    int ray_ok[2];
};

static void panel_for_selection(struct app *a)
{
    struct panel *p = &a->panel;
    p->nlines = 0;
    p->dirty = 1;

    if (a->selected == -1) {
        panel_line(p, "JSON in dark space");
        panel_line(p, "%zu nodes   %s", a->scene.n,
                   a->scene.path ? a->scene.path : "$");
        panel_line(p, "%s", a->desktop ? "click: select   drag: rotate"
                                       : "trigger: select   menu: reset");
        return;
    }

    if (a->selected == -2) {
        panel_line(p, "origin");
        panel_line(p, "root cube (size %.1f)", (double)JL_ORIGIN_SIZE);
        return;
    }

    struct node *n = &a->scene.v[a->selected];
    uint32_t e = n->idx > 0 ? n->idx - 1 : 0;
    yyjson_val *val = yyjson_arr_get(a->scene.items, e);

    panel_line(p, "#%u", n->idx);
    panel_line(p, "%s[%u]", a->scene.path, e);
    panel_line(p, "type: %s", json_type_name(val));

    char *js = val ? yyjson_val_write(val, 0, NULL) : NULL;
    if (js) {
        char flat[200];
        size_t k = 0;
        for (size_t i = 0; js[i] && k < sizeof flat - 1; ++i) {
            char c = js[i];
            flat[k++] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        }
        flat[k] = 0;

        /* wrap roughly every 26 chars */
        const size_t per = 26;
        for (size_t i = 0; i < k && p->nlines < 8; i += per) {
            size_t l = k - i;
            if (l > per) l = per;
            panel_line(p, "%.*s", (int)l, flat + i);
        }
        free(js);
    }
}

static void app_init_gfx(struct app *a)
{
    gfx_init(&a->gfx, &a->scene);
    panel_for_selection(a);
    panel_build(&a->panel);
    panel_upload(&a->panel, &a->gfx);
}

static void panel_draw(struct app *a, const float view[16], const float proj[16],
                       struct vec3 cam_pos, struct vec3 cam_fwd, struct vec3 target)
{
    struct panel *p = &a->panel;
    if (p->dirty) {
        panel_build(p);
        panel_upload(p, &a->gfx);
        p->dirty = 0;
    }
    if (p->nverts == 0) return;

    struct vec3 center;
    if (a->selected == -1) {
        /* hint panel, right in front of the eye */
        center = v3_add(cam_pos, v3_mul(cam_fwd, 0.85f));
    } else {
        /* anchored in the direction of the selected node, but always at a
         * comfortable reading distance from the eye */
        struct vec3 dir = v3_norm(v3_sub(target, cam_pos));
        center = v3_add(cam_pos, v3_mul(dir, 0.85f));
    }

    struct vec3 fwd = v3_norm(v3_sub(cam_pos, center));
    struct vec3 right = v3_norm(v3_cross(v3(0, 1, 0), fwd));
    struct vec3 down = v3_cross(right, fwd);

    const float wpu = 0.0011f;                /* world units per stb unit */
    struct vec3 origin = v3_sub(center,
                                v3_add(v3_mul(right, p->w * 0.5f * wpu),
                                       v3_mul(down, p->h * 0.5f * wpu)));

    float model[16];
    memset(model, 0, sizeof model);
    model[0]  = right.x * wpu; model[1]  = right.y * wpu; model[2]  = right.z * wpu;
    model[4]  = down.x  * wpu; model[5]  = down.y  * wpu; model[6]  = down.z  * wpu;
    model[8]  = fwd.x;         model[9]  = fwd.y;         model[10] = fwd.z;
    model[12] = origin.x;      model[13] = origin.y;      model[14] = origin.z;
    model[15] = 1.0f;

    float mvp[16];
    mat4_mul(mvp, proj, view);
    mat4_mul(mvp, mvp, model);

    glUseProgram(a->gfx.prog_ui);
    glUniformMatrix4fv(a->gfx.u_ui_mvp, 1, GL_FALSE, mvp);
    glUniform4f(a->gfx.u_ui_col, 1.0f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(a->gfx.vao_panel);
    glDrawArrays(GL_TRIANGLES, 0, p->nverts);
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
}

/* outline one node (or the origin) in model space */
static void draw_node_box(struct app *a, int which, float grow, const float vp[16],
                          float r, float g, float b)
{
    float pts[JL_BOX_EDGE_FLOATS];
    struct vec3 c = v3(0, 0, 0);
    float sz = JL_ORIGIN_SIZE;

    if (which >= 0) {
        const struct node *n = &a->scene.v[which];
        c = v3(n->x, n->y, n->z);
        sz = n->size;
    }
    jl_box_edges(pts, c.x, c.y, c.z, sz * grow);
    draw_dyn_lines(&a->gfx, pts, 24, vp, r, g, b, 1.0f);
}

/* draw the graph + panel for one eye */
static void render_eye(struct app *a, const float view[16], const float proj[16],
                       struct vec3 cam_pos, struct vec3 cam_fwd)
{
    float model[16], vp[16];
    mat4_model(model, a->scale, a->yaw, a->offset);
    mat4_mul(vp, proj, view);
    mat4_mul(vp, vp, model);

    draw_scene(&a->gfx, &a->scene, vp);

    /* controller rays, world space */
    float world_vp[16];
    mat4_mul(world_vp, proj, view);
    for (int i = 0; i < 2; ++i) {
        if (!a->ray_ok[i]) continue;
        struct vec3 o = a->ray_o[i], d = a->ray_d[i];
        float pts[6] = { o.x, o.y, o.z,
                         o.x + d.x * 2.5f, o.y + d.y * 2.5f, o.z + d.z * 2.5f };
        draw_dyn_lines(&a->gfx, pts, 2, world_vp, 0.2f, 0.8f, 1.0f, 0.8f);
    }

    if (a->selected != -1)
        draw_node_box(a, a->selected, 1.06f, vp, 1.0f, 1.0f, 0.2f);
    if (a->hovered != -1 && a->hovered != a->selected)
        draw_node_box(a, a->hovered, 1.10f, vp, 0.2f, 0.9f, 1.0f);

    /* panel is in world space */
    struct vec3 target = cam_pos;
    if (a->selected == -2) {
        target = a->offset;
    } else if (a->selected >= 0) {
        float m[16];
        mat4_model(m, a->scale, a->yaw, a->offset);
        struct node *n = &a->scene.v[a->selected];
        float x = n->x, y = n->y, z = n->z;
        target = v3(m[0] * x + m[4] * y + m[8] * z + m[12],
                    m[1] * x + m[5] * y + m[9] * z + m[13],
                    m[2] * x + m[6] * y + m[10] * z + m[14]);
    }
    panel_draw(a, view, proj, cam_pos, cam_fwd, target);
}

/* ================================================================== *
 * desktop mode: side-by-side stereo in a window
 * ================================================================== */

static const char *g_snapshot;     /* --snapshot FILE.ppm */
static int g_snapshot_after = 3;

static void write_ppm(const char *path, int w, int h)
{
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; --y)
        for (int x = 0; x < w; ++x)
            fwrite(&px[((size_t)y * w + x) * 4], 1, 3, f);
    fclose(f);
    free(px);
    printf("snapshot: %s (%dx%d)\n", path, w, h);
}

static void run_desktop(struct app *a, int width, int height)
{
    if (!glfwInit()) {
        const char *err = NULL;
        glfwGetError(&err);
        fprintf(stderr, "glfwInit failed: %s\n", err ? err : "unknown error");
        exit(1);
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_SAMPLES, g_snapshot ? 0 : 4);

    GLFWwindow *win = glfwCreateWindow(width, height, "json-gl-xr  [SBS]", NULL, NULL);
    if (!win) {
        const char *err = NULL;
        glfwGetError(&err);
        fprintf(stderr, "cannot create a GLES 3.2 window: %s\n",
                err ? err : "unknown GLFW error");
        glfwTerminate();
        exit(1);
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    printf("GLES %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    app_init_gfx(a);

    float yaw = 0.35f, pitch = 0.15f, dist = 5.0f;
    int drag = 0;
    double lx = 0, ly = 0;

    glfwSetWindowUserPointer(win, a);

    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(win, 1);

        double mx, my;
        glfwGetCursorPos(win, &mx, &my);
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            if (drag) {
                yaw   += (float)(mx - lx) * 0.01f;
                pitch += (float)(my - ly) * 0.01f;
            }
            drag = 1;
            lx = mx; ly = my;
        } else {
            drag = 0;
        }
        if (glfwGetKey(win, GLFW_KEY_EQUAL) == GLFW_PRESS) dist -= 0.03f;
        if (glfwGetKey(win, GLFW_KEY_MINUS) == GLFW_PRESS) dist += 0.03f;
        if (dist < 0.8f) dist = 0.8f;
        if (dist > 40.0f) dist = 40.0f;

        int w, h;
        glfwGetFramebufferSize(win, &w, &h);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* view matrix: rotate the world, then pull it back (like the GLUT app) */
        float ry[16], rx[16], tr[16], view[16];
        memset(ry, 0, sizeof ry);
        ry[0] = cosf(yaw);  ry[2] = -sinf(yaw);
        ry[8] = sinf(yaw);  ry[10] = cosf(yaw);  ry[5] = 1; ry[15] = 1;
        memset(rx, 0, sizeof rx);
        rx[5] = cosf(pitch); rx[6] = -sinf(pitch);
        rx[9] = sinf(pitch); rx[10] = cosf(pitch); rx[0] = 1; rx[15] = 1;
        mat4_identity(tr);
        tr[14] = -dist;
        mat4_mul(view, rx, ry);
        mat4_mul(view, view, tr);

        float proj[16];
        mat4_persp(proj, 62.0f * DEG2RAD, (float)(w / 2) / (float)(h ? h : 1), 0.05f, 200.0f);

        /* camera in world space, derived from the view matrix */
        struct vec3 eye = view_cam_pos(view);
        struct vec3 cam_fwd = view_cam_fwd(view);
        struct vec3 right = v3_norm(v3_cross(cam_fwd, v3(0, 1, 0)));

        struct vec3 eye_l = eye;
        for (int e = 0; e < 2; ++e) {
            float off = (e == 0 ? -0.032f : 0.032f);
            struct vec3 eye_e = v3_add(eye, v3_mul(right, off));
            if (e == 0) eye_l = eye_e;
            glViewport(e * (w / 2), 0, w / 2, h);
            render_eye(a, view, proj, eye_e, cam_fwd);
        }
        glViewport(0, 0, w, h);

        if (g_snapshot && --g_snapshot_after == 0) {
            glfwGetFramebufferSize(win, &w, &h);
            write_ppm(g_snapshot, w, h);
            glfwSetWindowShouldClose(win, 1);
            continue;
        }
        glfwSwapBuffers(win);

        /* hover under the mouse (left eye), click selects */
        static double last_pick = 0;
        static double pmx = -1, pmy = -1;
        static int lmb_prev = 0;
        int lmb = glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        int clicked = lmb && !lmb_prev;
        lmb_prev = lmb;

        double now = glfwGetTime();
        int moved = (mx != pmx || my != pmy);
        if (moved || clicked) { pmx = mx; pmy = my; }

        if (!drag && (clicked || (moved && now - last_pick > 0.08))) {
            last_pick = now;
            int bw = w / 2;
            float ndcx = (float)(mx / (double)(bw ? bw : 1)) * 2.0f - 1.0f;
            float ndcy = 1.0f - (float)(my / (double)(h ? h : 1)) * 2.0f;
            float t = tanf(62.0f * 0.5f * DEG2RAD);
            struct vec3 dir = v3(ndcx * t * ((float)bw / (float)(h ? h : 1)),
                                 ndcy * t, -1.0f);
            dir = view_dir_to_world(view, dir);

            struct hit hit = scene_pick(&a->scene, eye_l, dir, a->offset, a->scale, a->yaw);
            int want = -1;
            if (hit.found == 1) want = (int)hit.node;
            else if (hit.found == 2) want = -2;

            a->hovered = want;
            if (clicked && want != -1 && want != a->selected) {
                a->selected = want;
                panel_for_selection(a);
            }
        }
    }

    glfwDestroyWindow(win);
    glfwTerminate();
}

/* ================================================================== *
 * OpenXR
 * ================================================================== */

#define XR_CHECK(x) do {                                                   \
        XrResult _r = (x);                                                 \
        if (XR_FAILED(_r)) {                                               \
            fprintf(stderr, "%s:%d  %s -> %s\n", __FILE__, __LINE__, #x,    \
                    xr_result_name(_r));                                   \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

struct xr_state {
    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSpace local;
    int running, exit;

    XrViewConfigurationView cfg[2];
    uint32_t nviews;

    XrSwapchain swap[2];
    GLuint fbo, depth;
    uint32_t sw, sh;

    XrActionSet actset;
    XrAction aim, select, menu, stick;
    XrSpace aim_space[2];
    XrPath hand[2];

    EGLDisplay egl_dpy;
    EGLConfig  egl_cfg;
    EGLContext egl_ctx;

    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_gles_req;
};

static struct xr_state g_xr;

static XrExtensionProperties *xr_extensions(uint32_t *n)
{
    *n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, n, NULL)) || *n == 0)
        return NULL;

    XrExtensionProperties *p = calloc(*n, sizeof *p);
    if (!p) return NULL;
    for (uint32_t i = 0; i < *n; ++i) p[i].type = XR_TYPE_EXTENSION_PROPERTIES;

    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, *n, n, p))) {
        free(p);
        *n = 0;
        return NULL;
    }
    return p;
}

static int xr_has_extension(const char *name)
{
    uint32_t n = 0;
    XrExtensionProperties *p = xr_extensions(&n);

    int found = 0;
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(p[i].extensionName, name)) found = 1;
    free(p);
    return found;
}

/* 1 when a runtime is reachable and can do GLES; otherwise says why not */
static int xr_probe(void)
{
    uint32_t n = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    if (XR_FAILED(r)) {
        fprintf(stderr,
                "cannot reach an OpenXR runtime: %s\n"
                "start SteamVR (or the runtime of your headset) first, or set\n"
                "XR_RUNTIME_JSON to its manifest; json-gl-xr-doctor prints what\n"
                "was searched.\n", xr_result_name(r));
        return 0;
    }
    if (!xr_has_extension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        fprintf(stderr,
                "this OpenXR runtime does not expose %s\n"
                "(run --list-xr-extensions to see what it does expose)\n",
                XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        return 0;
    }
    return 1;
}

static void xr_print_extensions(void)
{
    uint32_t n = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    if (XR_FAILED(r)) {
        printf("no reachable OpenXR runtime: %s\n", xr_result_name(r));
        printf("try json-gl-xr-doctor: it prints the manifests that were searched\n");
        return;
    }
    XrExtensionProperties *p = xr_extensions(&n);
    if (!p) {
        printf("no OpenXR runtime found\n");
        return;
    }
    printf("%u instance extensions\n", n);
    for (uint32_t i = 0; i < n; ++i) printf("  %-48s v%u\n", p[i].extensionName, p[i].extensionVersion);
    printf("\nXR_KHR_opengl_es_enable : %s\n",
           xr_has_extension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) ? "yes" : "NO");
    printf("XR_KHR_opengl_enable    : %s\n",
           xr_has_extension("XR_KHR_opengl_enable") ? "yes" : "NO");
    printf("XR_KHR_vulkan_enable2   : %s\n",
           xr_has_extension("XR_KHR_vulkan_enable2") ? "yes" : "NO");
    free(p);
}

static void egl_init(struct xr_state *x)
{
    x->egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (x->egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "no EGL display\n"); exit(1); }
    if (!eglInitialize(x->egl_dpy, NULL, NULL)) { fprintf(stderr, "eglInitialize failed\n"); exit(1); }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(x->egl_dpy, cfg_attr, &x->egl_cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "no EGL config for GLES3\n");
        exit(1);
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    x->egl_ctx = eglCreateContext(x->egl_dpy, x->egl_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (x->egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); exit(1); }

    const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface pb = eglCreatePbufferSurface(x->egl_dpy, x->egl_cfg, pb_attr);
    if (pb == EGL_NO_SURFACE) { fprintf(stderr, "eglCreatePbufferSurface failed\n"); exit(1); }

    if (!eglMakeCurrent(x->egl_dpy, pb, pb, x->egl_ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n");
        exit(1);
    }
}

static void xr_instance(struct xr_state *x)
{
    if (!xr_has_extension(XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        fprintf(stderr,
                "this OpenXR runtime does not expose XR_KHR_opengl_es_enable.\n"
                "run with --list-xr-extensions to see what it does expose.\n");
        exit(2);
    }

    const char *exts[] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };

    XrApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    snprintf(ai.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "json-gl-xr");
    ai.applicationVersion = 1;
    snprintf(ai.engineName, XR_MAX_ENGINE_NAME_SIZE, "none");
    ai.engineVersion = 1;
    ai.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstanceCreateInfo ci = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    ci.next = NULL;
    ci.createFlags = 0;
    ci.applicationInfo = ai;
    ci.enabledApiLayerCount = 0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;

    XR_CHECK(xrCreateInstance(&ci, &x->instance));

    XrSystemGetInfo si = { .type = XR_TYPE_SYSTEM_GET_INFO };
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(x->instance, &si, &x->system));

    XR_CHECK(xrGetInstanceProcAddr(x->instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                   (PFN_xrVoidFunction *)&x->get_gles_req));
}

static void xr_swapchains(struct xr_state *x)
{
    uint32_t n = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(x->instance, x->system,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               0, &n, NULL));
    if (n > 2) n = 2;
    x->nviews = n;
    for (uint32_t i = 0; i < n; ++i) x->cfg[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XR_CHECK(xrEnumerateViewConfigurationViews(x->instance, x->system,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                               n, &n, x->cfg));

    uint32_t nfmt = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(x->session, 0, &nfmt, NULL));
    int64_t *fmts = calloc(nfmt ? nfmt : 1, sizeof *fmts);
    XR_CHECK(xrEnumerateSwapchainFormats(x->session, nfmt, &nfmt, fmts));

    /* linear RGBA8 first: we do not enable GL_FRAMEBUFFER_SRGB, so an sRGB
     * attachment would make the compositor darken everything */
    int64_t chosen = fmts[0];
    for (uint32_t i = 0; i < nfmt; ++i) {
        if (fmts[i] == (int64_t)GL_RGBA8) { chosen = fmts[i]; break; }
        if (fmts[i] == (int64_t)GL_SRGB8_ALPHA8) chosen = fmts[i];
    }
    free(fmts);

    x->sw = x->cfg[0].recommendedImageRectWidth;
    x->sh = x->cfg[0].recommendedImageRectHeight;

    for (uint32_t e = 0; e < x->nviews; ++e) {
        XrSwapchainCreateInfo sc = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO };
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = chosen;
        sc.sampleCount = 1;
        sc.width = x->cfg[e].recommendedImageRectWidth;
        sc.height = x->cfg[e].recommendedImageRectHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XR_CHECK(xrCreateSwapchain(x->session, &sc, &x->swap[e]));
    }

    /* one FBO with an attachable depth buffer (color is swapped each frame) */
    glGenFramebuffers(1, &x->fbo);
    glGenRenderbuffers(1, &x->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, x->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)x->sw, (GLsizei)x->sh);
    glBindFramebuffer(GL_FRAMEBUFFER, x->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, x->depth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void xr_actions(struct xr_state *x)
{
    XrActionSetCreateInfo as = { .type = XR_TYPE_ACTION_SET_CREATE_INFO };
    snprintf(as.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "main");
    snprintf(as.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "main");
    as.priority = 0;
    XR_CHECK(xrCreateActionSet(x->instance, &as, &x->actset));

    xrStringToPath(x->instance, "/user/hand/left", &x->hand[0]);
    xrStringToPath(x->instance, "/user/hand/right", &x->hand[1]);

    struct {
        XrAction *act;
        const char *name, *local;
        XrActionType type;
    } defs[] = {
        { &x->aim,    "aim",    "Aim",    XR_ACTION_TYPE_POSE_INPUT },
        { &x->select, "select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &x->menu,   "menu",   "Menu",   XR_ACTION_TYPE_BOOLEAN_INPUT },
        { &x->stick,  "move",   "Move",   XR_ACTION_TYPE_VECTOR2F_INPUT },
    };

    for (size_t i = 0; i < sizeof defs / sizeof defs[0]; ++i) {
        XrActionCreateInfo ac = { .type = XR_TYPE_ACTION_CREATE_INFO };
        snprintf(ac.actionName, XR_MAX_ACTION_NAME_SIZE, "%s", defs[i].name);
        snprintf(ac.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "%s", defs[i].local);
        ac.actionType = defs[i].type;
        ac.countSubactionPaths = 2;
        ac.subactionPaths = x->hand;
        XR_CHECK(xrCreateAction(x->actset, &ac, defs[i].act));
    }

    XrPath p_aim[2], p_sel[2], p_menu, p_stick;
    xrStringToPath(x->instance, "/user/hand/left/input/aim/pose", &p_aim[0]);
    xrStringToPath(x->instance, "/user/hand/right/input/aim/pose", &p_aim[1]);
    xrStringToPath(x->instance, "/user/hand/left/input/select/click", &p_sel[0]);
    xrStringToPath(x->instance, "/user/hand/right/input/select/click", &p_sel[1]);
    xrStringToPath(x->instance, "/user/hand/left/input/menu/click", &p_menu);
    xrStringToPath(x->instance, "/user/hand/left/input/thumbstick", &p_stick);

    XrActionSuggestedBinding bindings[] = {
        { x->aim, p_aim[0] }, { x->aim, p_aim[1] },
        { x->select, p_sel[0] }, { x->select, p_sel[1] },
        { x->menu, p_menu },
        { x->stick, p_stick },
    };
    static const char *profiles[] = {
        "/interaction_profiles/oculus/touch_controller",
        "/interaction_profiles/khr/simple_controller",
    };

    for (int i = 0; i < 2; ++i) {
        XrPath profile;
        xrStringToPath(x->instance, profiles[i], &profile);

        XrInteractionProfileSuggestedBinding sp = { .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sp.interactionProfile = profile;
        /* the thumbstick is the last binding and simple_controller lacks it */
        sp.countSuggestedBindings = sizeof bindings / sizeof bindings[0] - (i == 0 ? 0 : 1);
        sp.suggestedBindings = bindings;
        xrSuggestInteractionProfileBindings(x->instance, &sp);
    }

    XrSessionActionSetsAttachInfo at = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    at.countActionSets = 1;
    at.actionSets = &x->actset;
    XR_CHECK(xrAttachSessionActionSets(x->session, &at));

    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo sc = { .type = XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sc.action = x->aim;
        sc.subactionPath = x->hand[i];
        sc.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace(x->session, &sc, &x->aim_space[i]));
    }
}

static void xr_session(struct xr_state *x)
{
    XrGraphicsRequirementsOpenGLESKHR req = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XR_CHECK(x->get_gles_req(x->instance, x->system, &req));

    XrGraphicsBindingGLES bind = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
    };
    bind.display = x->egl_dpy;
    bind.config  = x->egl_cfg;
    bind.context = x->egl_ctx;

    XrSessionCreateInfo si = { .type = XR_TYPE_SESSION_CREATE_INFO };
    si.next = &bind;
    si.systemId = x->system;
    XR_CHECK(xrCreateSession(x->instance, &si, &x->session));

    XrReferenceSpaceCreateInfo rs = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(x->session, &rs, &x->local));

    xr_swapchains(x);
    xr_actions(x);
}

static int xr_poll(struct xr_state *x)
{
    XrEventDataBuffer ev = { .type = XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(x->instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged *)&ev;
            if (s->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { .type = XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(x->session, &bi))) x->running = 1;
            } else if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(x->session);
                x->running = 0;
            } else if (s->state == XR_SESSION_STATE_EXITING ||
                       s->state == XR_SESSION_STATE_LOSS_PENDING) {
                x->running = 0;
                x->exit = 1;
            }
        }
        ev = (XrEventDataBuffer){ .type = XR_TYPE_EVENT_DATA_BUFFER };
    }
    return x->exit ? 0 : 1;
}

static int xr_pressed(struct xr_state *x, XrAction act, XrPath hand)
{
    XrActionStateBoolean s = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateGetInfo gi = { .type = XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = act;
    gi.subactionPath = hand;
    xrGetActionStateBoolean(x->session, &gi, &s);
    return s.changedSinceLastSync && s.currentState;
}

static int xr_pose_active(struct xr_state *x, XrAction act, XrPath hand)
{
    XrActionStatePose s = { .type = XR_TYPE_ACTION_STATE_POSE };
    XrActionStateGetInfo gi = { .type = XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = act;
    gi.subactionPath = hand;
    xrGetActionStatePose(x->session, &gi, &s);
    return s.isActive;
}

static void xr_input(struct app *a, struct xr_state *x, XrTime t, int *reset_view)
{
    XrActiveActionSet active = { .actionSet = x->actset, .subactionPath = XR_NULL_PATH };
    XrActionsSyncInfo sync = { .type = XR_TYPE_ACTIONS_SYNC_INFO };
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(x->session, &sync);

    a->hovered = -1;
    a->ray_ok[0] = a->ray_ok[1] = 0;

    for (int i = 0; i < 2; ++i) {
        if (xr_pose_active(x, x->aim, x->hand[i])) {
            XrSpaceLocation loc = { .type = XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(x->aim_space[i], x->local, t, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                struct vec3 o = v3(loc.pose.position.x, loc.pose.position.y, loc.pose.position.z);
                struct vec3 d = v3_norm(quat_forward(loc.pose.orientation));
                a->ray_o[i] = o;
                a->ray_d[i] = d;
                a->ray_ok[i] = 1;

                struct hit hit = scene_pick(&a->scene, o, d, a->offset, a->scale, a->yaw);
                if (hit.found == 1 && a->hovered < 0) a->hovered = (int)hit.node;
                if (hit.found == 2 && a->hovered < 0) a->hovered = -2;
            }
        }

        if (xr_pressed(x, x->select, x->hand[i]) &&
            (a->hovered >= 0 || a->hovered == -2)) {
            a->selected = a->hovered;
            panel_for_selection(a);
        }

        if (xr_pressed(x, x->menu, x->hand[i]))
            *reset_view = 1;
    }

    XrActionStateVector2f vs = { .type = XR_TYPE_ACTION_STATE_VECTOR2F };
    XrActionStateGetInfo gi4 = { .type = XR_TYPE_ACTION_STATE_GET_INFO };
    gi4.action = x->stick;
    gi4.subactionPath = x->hand[0];
    xrGetActionStateVector2f(x->session, &gi4, &vs);
    if (vs.isActive) {
        a->yaw   -= vs.currentState.x * 0.03f;
        a->scale *= (1.0f + vs.currentState.y * 0.02f);
        if (a->scale < 0.05f) a->scale = 0.05f;
        if (a->scale > 4.0f) a->scale = 4.0f;
    }
}

static void run_vr(struct app *a)
{
    struct xr_state *x = &g_xr;
    memset(x, 0, sizeof *x);

    egl_init(x);
    xr_instance(x);
    xr_session(x);

    printf("GLES %s | %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    printf("views %u, swapchain %ux%u\n", x->nviews, x->sw, x->sh);

    app_init_gfx(a);

    XrSwapchainImageOpenGLESKHR **images = calloc(x->nviews, sizeof *images);
    uint32_t *nimg = calloc(x->nviews, sizeof *nimg);
    for (uint32_t e = 0; e < x->nviews; ++e) {
        uint32_t n = 0;
        XR_CHECK(xrEnumerateSwapchainImages(x->swap[e], 0, &n, NULL));
        XrSwapchainImageOpenGLESKHR *arr = calloc(n, sizeof *arr);
        for (uint32_t i = 0; i < n; ++i) arr[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        XR_CHECK(xrEnumerateSwapchainImages(x->swap[e], n, &n, (XrSwapchainImageBaseHeader *)arr));
        images[e] = arr;
        nimg[e] = n;
    }

    while (xr_poll(x)) {
        if (!x->running) { usleep(10000); continue; }

        XrFrameState fs = { .type = XR_TYPE_FRAME_STATE };
        XrFrameWaitInfo fw = { .type = XR_TYPE_FRAME_WAIT_INFO };
        if (XR_FAILED(xrWaitFrame(x->session, &fw, &fs))) break;

        XrFrameBeginInfo fb = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        if (XR_FAILED(xrBeginFrame(x->session, &fb))) break;

        XrViewLocateInfo vl = { .type = XR_TYPE_VIEW_LOCATE_INFO };
        vl.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vl.displayTime = fs.predictedDisplayTime;
        vl.space = x->local;
        XrViewState vs = { .type = XR_TYPE_VIEW_STATE };
        uint32_t nv = 2;
        XrView views[2];
        for (int i = 0; i < 2; ++i) views[i].type = XR_TYPE_VIEW;
        xrLocateViews(x->session, &vl, &vs, 2, &nv, views);

        int reset = 0;
        if (fs.shouldRender && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            xr_input(a, x, fs.predictedDisplayTime, &reset);
            if (reset) {
                a->yaw = 0;
                a->scale = 0.5f / fmaxf(1.0f, a->scene.extent);
            }
        }

        XrCompositionLayerProjectionView pv[2];
        XrSwapchainSubImage si[2];
        memset(pv, 0, sizeof pv);
        memset(si, 0, sizeof si);

        for (uint32_t e = 0; e < nv && e < x->nviews; ++e) {
            uint32_t img = 0;
            XrSwapchainImageAcquireInfo ac = { .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            xrAcquireSwapchainImage(x->swap[e], &ac, &img);

            XrSwapchainImageWaitInfo wi = { .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(x->swap[e], &wi);

            glBindFramebuffer(GL_FRAMEBUFFER, x->fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   images[e][img].image, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                fprintf(stderr, "incomplete XR framebuffer\n");
                exit(1);
            }
            glViewport(0, 0, (GLsizei)x->cfg[e].recommendedImageRectWidth,
                       (GLsizei)x->cfg[e].recommendedImageRectHeight);
            glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            float view[16], proj[16];
            mat4_view_from_pose(view, views[e].pose);
            mat4_persp_fov(proj, views[e].fov, 0.05f, 200.0f);
            struct vec3 eye = v3(views[e].pose.position.x, views[e].pose.position.y,
                                 views[e].pose.position.z);
            struct vec3 cam_fwd = v3_norm(quat_forward(views[e].pose.orientation));
            render_eye(a, view, proj, eye, cam_fwd);

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glFlush();

            XrSwapchainImageReleaseInfo ri = { .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(x->swap[e], &ri);

            si[e].swapchain = x->swap[e];
            si[e].imageRect.offset.x = 0;
            si[e].imageRect.offset.y = 0;
            si[e].imageRect.extent.width = (int32_t)x->cfg[e].recommendedImageRectWidth;
            si[e].imageRect.extent.height = (int32_t)x->cfg[e].recommendedImageRectHeight;
            si[e].imageArrayIndex = 0;

            pv[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[e].pose = views[e].pose;
            pv[e].fov = views[e].fov;
            pv[e].subImage = si[e];
        }

        XrCompositionLayerProjection layer = { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        layer.space = x->local;
        layer.viewCount = nv < x->nviews ? nv : x->nviews;
        layer.views = pv;

        const XrCompositionLayerBaseHeader *layers[1] = {
            (const XrCompositionLayerBaseHeader *)&layer
        };

        XrFrameEndInfo fe = { .type = XR_TYPE_FRAME_END_INFO };
        fe.displayTime = fs.predictedDisplayTime;
        fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fe.layerCount = fs.shouldRender ? 1 : 0;
        fe.layers = fs.shouldRender ? layers : NULL;
        xrEndFrame(x->session, &fe);
    }

    for (uint32_t e = 0; e < x->nviews; ++e) {
        xrDestroySwapchain(x->swap[e]);
        free(images[e]);
    }
    free(images);
    free(nimg);
    for (int i = 0; i < 2; ++i) xrDestroySpace(x->aim_space[i]);
    if (x->local) xrDestroySpace(x->local);
    xrDestroyActionSet(x->actset);
    if (x->session) xrDestroySession(x->session);
    if (x->instance) xrDestroyInstance(x->instance);
}

/* ================================================================== *
 * main
 * ================================================================== */

static void usage(const char *argv0)
{
    printf("usage: %s [options] < data.json\n\n", argv0);
    printf("  --in-file FILE          read the json from FILE instead of stdin\n");
    printf("  --main-path PATH        csonpath expression, e.g. '$.items'\n");
    printf("  --desktop               side-by-side stereo in a window, no OpenXR\n");
    printf("  --list-xr-extensions    print the runtime capabilities and exit\n");
    printf("  --width N --height N    desktop window size (default 1600x600)\n");
    printf("  --snapshot FILE.ppm     render a few frames to FILE and exit\n");
    printf("  --select N              preselect node N (-2 = origin)\n");
    printf("  --help\n");
}

int main(int argc, char **argv)
{
    const char *in_file = NULL, *main_path = NULL;
    int desktop = 0, list_only = 0, width = 1600, height = 600, preselect = -1;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--in-file") && i + 1 < argc) in_file = argv[++i];
        else if (!strcmp(argv[i], "--main-path") && i + 1 < argc) main_path = argv[++i];
        else if (!strcmp(argv[i], "--desktop")) desktop = 1;
        else if (!strcmp(argv[i], "--list-xr-extensions")) list_only = 1;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) g_snapshot = argv[++i];
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) preselect = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (list_only) { xr_print_extensions(); return 0; }

    FILE *f = in_file ? fopen(in_file, "rb") : stdin;
    if (!f) { perror(in_file); return 1; }

    size_t len = 0;
    char *buf = read_all(f, &len);
    if (f != stdin) fclose(f);

    yyjson_read_err rerr;
    yyjson_doc *doc = yyjson_read_opts(buf, len, 0, NULL, &rerr);
    if (!doc) {
        fprintf(stderr, "invalid json: %s at byte %zu\n", rerr.msg, rerr.pos);
        return 1;
    }

    struct app a;
    memset(&a, 0, sizeof a);
    a.selected = -1;
    a.hovered = -1;
    a.yaw = 0.0f;

    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *cur = resolve_path(root, main_path);
    if (!cur) {
        fprintf(stderr, "fail to retrieve json with '%s'\n", main_path);
        return 1;
    }
    if (!yyjson_is_arr(cur)) {
        fprintf(stderr, "main json must be an array\n");
        return 1;
    }

    a.scene.items = cur;
    a.scene.path = main_path ? main_path : "$";

    jl_walk(yyjson_arr_size(cur), scene_build_cb, &a.scene);

    /* the origin cube (size 1) sets a floor for the extent */
    if (a.scene.extent < 1.0f) a.scene.extent = 1.0f;
    a.scale = 1.0f / a.scene.extent;
    a.offset = v3(0, 0, 0);

    printf("%zu nodes, extent %.1f, path %s\n", a.scene.n, a.scene.extent, a.scene.path);

    if (preselect >= -2) {
        if (preselect < (int)a.scene.n) a.selected = preselect;
        else fprintf(stderr, "--select %d out of range (0..%zu)\n",
                     preselect, a.scene.n ? a.scene.n - 1 : 0);
    }

    if (!desktop && !xr_probe()) {
        fprintf(stderr, "falling back to --desktop\n");
        desktop = 1;
    }
    a.desktop = desktop;

    if (desktop) {
        run_desktop(&a, width, height);
    } else {
        /* VR: 1.5 m in front of the user, 0.5 m radius */
        a.offset = v3(0, 1.5f, -1.0f);
        a.scale = 0.5f / a.scene.extent;
        run_vr(&a);
    }

    yyjson_doc_free(doc);
    free(a.scene.v);
    free(buf);
    return 0;
}
