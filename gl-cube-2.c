#define GL_SILENCE_DEPRECATION
#include <stdlib.h>
#include <string.h> // for strcmp
#include <stdio.h>
#include <math.h>
#include <OpenGL/gl.h>
#include <GLUT/glut.h>
#include "csonpath_json-c.h"
#include "json_layout.h"

#define WIN_X 800
#define WIN_Y 600

static float angle_x = 0.0f;
static float angle_y = 0.0f;
static float zoom = -3;

static struct json_object *main_json;
static struct json_object *cur_json;

// 8 cube corners relative to (cx,cy,cz) will be computed
GLfloat vertices[8][3];

// selection: store selected cube center; use an impossible initial value
int selected_exists = 0;
float selected_cx =0.0f, selected_cy = 0.0f, selected_cz = 0.0f;


static int frameCount = 0;
static float fps = 0.0f;
static int previousTime = 0;

void drawLine(float cx0, float cy0, float cz0,
	      float cx1, float cy1, float cz1)
{
  glBegin(GL_LINES);
  glColor3f(1,1,1);
  glVertex3f(cx0, cy0, cz0); glVertex3f(cx1, cy1, cz1);
  glEnd();
}

void drawCubeAt(float cx, float cy, float cz, float size)
{
    jl_box_corners(vertices, cx, cy, cz, size);

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vertices);

    for (int i = 0; i < 6; i++) {
	glNormal3fv(jl_normals[i]);
	glColor3fv(jl_colors[i]);
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, jl_faces[i]);
    }

    glDisableClientState(GL_VERTEX_ARRAY);
}


// helper to draw a wireframe box around selected cube
void drawWireAABB(float cx, float cy, float cz, float size)
{
    // 8 corners (same orientation as vertices)
    GLfloat v[8][3];
    jl_box_corners(v, cx, cy, cz, size);
    glPushAttrib(GL_ENABLE_BIT | GL_POLYGON_BIT | GL_CURRENT_BIT);    glColor3f(1.0f, 1.0f, 1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBegin(GL_QUADS);
      // draw slightly inset quads so lines are visible
      for (int i=0;i<6;i++) {
        glVertex3fv(v[jl_faces[i][0]]);
        glVertex3fv(v[jl_faces[i][1]]);
        glVertex3fv(v[jl_faces[i][2]]);
        glVertex3fv(v[jl_faces[i][3]]);
      }
    glEnd();
    // restore polygon mode etc.
    glPopAttrib();
}

void drawCube(float cx, float cy, float cz, float size)
{
  // wrapper used earlier: draw cube normally
  drawCubeAt(cx, cy, cz, size);

  // if selected and this cube matches selection, draw a wireframe highlight
  if (selected_exists && fabs(cx - selected_cx) < 1e-6f &&
      fabs(cy - selected_cy) < 1e-6f && fabs(cz - selected_cz) < 1e-6f) {
    drawWireAABB(cx, cy, cz, size * 1.02f); // slightly bigger wireframe
  }
}

void renderBitmapString(float x, float y, void *font, const char *string)
{
    glRasterPos2f(x, y);
    for (const char *c = string; *c; c++) {
        glutBitmapCharacter(font, *c);
    }
}

struct walk_thunk {
    void (*f)(float, float, float, void *);
    void *data;
};

static void walk_thunk_cb(uint32_t idx, float x, float y, float z, void *ud)
{
    struct walk_thunk *t = ud;
    (void)idx;
    t->f(x, y, z, t->data);
}

void do_json(json_object *cur, void (*f)(float, float, float, void *), void *data)
{
    struct walk_thunk t = { f, data };
    jl_walk(json_object_array_length(cur), walk_thunk_cb, &t);
}

void display_json_(float x, float y, float z, void *data)
{
      drawLine(0,0,0, x, y, z);
      drawCube(x, y, z, 0.4f);    
}

void display_json(json_object *cur)
{
    do_json(cur, display_json_, NULL);
}

void display()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    glTranslatef(0.0f, 0.0f, zoom);
    glRotatef(angle_y, 1.0f, 0.0f, 0.0f);
    glRotatef(angle_x, 0.0f, 1.0f, 0.0f);

    drawCube(0.0f, 0.0f, 0.0f, 1.0f);

    json_object *cur = cur_json;
    display_json(cur);
    // --- draw overlay ---
    glDisable(GL_LIGHTING);
    char fpsText[64];
    sprintf(fpsText, "FPS: %.2f", fps);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluOrtho2D(0, WIN_X, 0, WIN_Y); // match window size

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor3f(1.0f, 1.0f, 1.0f); // white text
    renderBitmapString(10, 580, GLUT_BITMAP_HELVETICA_18, fpsText);

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_LIGHTING);

    glutSwapBuffers();

    frameCount++;
    int currentTime = glutGet(GLUT_ELAPSED_TIME);
    int timeInterval = currentTime - previousTime;

    if (timeInterval > 1000) { // 1 second passed
      fps = frameCount / (timeInterval / 1000.0f);
      previousTime = currentTime;
      frameCount = 0;
    }
}

void reshape(int w, int h)
{
    if (h == 0) h = 1;
    float aspect = (float)w / (float)h;
    glViewport(0, 0, w, h);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(60.0, aspect, 0.1, 100.0);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

struct col_data {
    float hitX, hitY, hitZ, hitSize;
    float bestT;
    int hit;
    float origin[3];
    float dir[3];

};

void do_col(float cx, float cy, float cz, void *data) {
    struct col_data *c = data;
    float size = JL_NODE_SIZE;           // match display size
    float hs = 0.5f * size;
    float boxMin[3] = {cx - hs, cy - hs, cz - hs};
    float boxMax[3] = {cx + hs, cy + hs, cz + hs};
    float tNear;
    if (jl_ray_box(c->origin, c->dir, boxMin, boxMax, &tNear)) {
	if (tNear >= 0.0f && tNear < c->bestT) {
	    c->bestT = tNear;
	    c->hit = 1;
	    c->hitX = cx; c->hitY = cy; c->hitZ = cz; c->hitSize = size;
	}
    }
}

// Mouse picking callback
void mouseCallback(int button, int state, int x, int y)
{
    if (button != GLUT_LEFT_BUTTON || state != GLUT_DOWN) return;

    // get current matrices / viewport
    GLdouble modelview[16], projection[16];
    GLint viewport[4];
    glGetDoublev(GL_MODELVIEW_MATRIX, modelview);
    glGetDoublev(GL_PROJECTION_MATRIX, projection);
    glGetIntegerv(GL_VIEWPORT, viewport);

    // convert window y to OpenGL coordinate (bottom-left origin)
    double winX = (double)x;
    double winY = (double)(viewport[3] - y - 1);

    // unproject near and far points (winZ = 0.0 -> near plane, 1.0 -> far plane)
    GLdouble nearX, nearY, nearZ;
    GLdouble farX, farY, farZ;
    if (!gluUnProject(winX, winY, 0.0, modelview, projection, viewport, &nearX, &nearY, &nearZ)) {
        fprintf(stderr, "gluUnProject near failed\n");
        return;
    }
    if (!gluUnProject(winX, winY, 1.0, modelview, projection, viewport, &farX, &farY, &farZ)) {
        fprintf(stderr, "gluUnProject far failed\n");
        return;
    }

    // iterate the same set of cubes you draw: center positions x,y,z in {-1,0,1}
    struct col_data cdata = {
	.bestT = INFINITY,
	.origin={ (float)nearX, (float)nearY, (float)nearZ },
	.dir={ (float)(farX - nearX), (float)(farY - nearY), (float)(farZ - nearZ) }
    };
    // optionally normalize dir for more stable t values
    float len = sqrtf(cdata.dir[0]*cdata.dir[0] + cdata.dir[1]*cdata.dir[1] + cdata.dir[2]*cdata.dir[2]);
    if (len > 1e-12f) { cdata.dir[0] /= len; cdata.dir[1] /= len; cdata.dir[2] /= len; }

    json_object *cur = cur_json;

    do_json(cur, do_col, &cdata);

    if (cdata.hit) {
        selected_exists = 1;
        selected_cx = cdata.hitX; selected_cy = cdata.hitY; selected_cz = cdata.hitZ;
        printf("Picked cube at (%.0f, %.0f, %.0f), size=%.2f\n", selected_cx, selected_cy, selected_cz, cdata.hitSize);
    } else {
        selected_exists = 0;
        printf("No cube picked\n");
    }

    glutPostRedisplay();
}

void key(unsigned char k, int u0_, int u1_)
{
    switch (k) {
    case 27:
    case 'q':
	json_object_put(main_json);
	exit(0);
	return;
    case 'b':
      zoom -= 0.25;
      break;
    case 'f':
      zoom += 0.25;
      break;
    case 'w':
      angle_y += 0.8f;
      if (angle_y > 360.0f) angle_y -= 360.0f;
      break;
    case 's':
      angle_y -= 0.8f;
      if (angle_y < 0.0) angle_y += 360.0f;
      break;
    case 'd':
      angle_x -= 0.8f;
      if (angle_x > 360.0f) angle_x -= 360.0f;
      break;
    case 'a':
      angle_x += 0.8f;
      if (angle_x < 0.0) angle_x += 360.0f;
      break;
    }
    glutPostRedisplay();
}

void idle()
{
    glutPostRedisplay();  // request redraw
}

int main(int argc, char** argv)
{
    // load json from stdin, or from --in-file
    char *main_path = NULL;
    char *in_file = NULL;

    for (int i = 1; i < argc; ++i) {
	if (!strcmp(argv[i], "--main-path") && i + 1 < argc) {
	    main_path = argv[++i];
	} else if (!strcmp(argv[i], "--in-file") && i + 1 < argc) {
	    in_file = argv[++i];
	}
    }

    int in_fd = 0;
    if (in_file) {
	FILE *f = fopen(in_file, "rb");
	if (!f) {
	    perror(in_file);
	    return 1;
	}
	in_fd = fileno(f);
    }
    main_json = json_object_from_fd(in_fd);
    if (!main_json) {
	fprintf(stderr, "fail to create json from %s\n",
		in_file ? in_file : "stdin");
	return 1;
    }

    if (main_path) {
	struct csonpath p;

	if (csonpath_init(&p, main_path) < 0) {
	    fprintf(stderr, "compile csonpath fail %s\n", p.compile_error);
	    return 1;
	}
	cur_json = csonpath_find_first(&p, main_json);
	if (!cur_json) {
	    fprintf(stderr, "fail to retrive json with '%s'\n", main_path);
	    return 1;
	}
    } else {
	cur_json = main_json;
    }

    if (!json_object_is_type(cur_json, json_type_array)) {
	fprintf(stderr, "main json must be array\n");
	return 1;
    }

    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB | GLUT_DEPTH);
    glutInitWindowSize(WIN_X, WIN_Y);
    glutCreateWindow("OpenGL 1.4 Cube with glDrawElements");

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    // simple light
    GLfloat lightPos[4] = {2.0f, 2.0f, 2.0f, 1.0f};
    GLfloat white[4]    = {1.0f, 1.0f, 1.0f, 1.0f};
    GLfloat amb[4]      = {0.2f, 0.2f, 0.2f, 1.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, lightPos);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  white);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  amb);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    glutDisplayFunc(display);
    glutReshapeFunc(reshape);
    glutIdleFunc(idle);
    glutKeyboardFunc(key);
    glutMouseFunc(mouseCallback);

    glutMainLoop();
    return 0;
}
