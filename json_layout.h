/*
 * json_layout.h - the parts both viewers must agree on.
 *
 *      gl-cube-2.c   GLUT / fixed-function desktop viewer
 *      vr.c          OpenGL ES 3.2 / OpenXR viewer
 *
 * Plain C99, header-only: the two programs stay single-translation-unit
 * builds and this file pulls in no GL or JSON header.  The walker only needs
 * a node count; each caller keeps its own JSON library.
 */
#ifndef JSON_LAYOUT_H
#define JSON_LAYOUT_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define JL_MAX_NODES      200000u
#define JL_NODE_SIZE      0.4f   /* size of a JSON node cube */
#define JL_ORIGIN_SIZE    1.0f   /* size of the root cube at (0,0,0) */

#define JL_CUBE_VERTICES  24
#define JL_CUBE_INDICES   36
#define JL_BOX_EDGE_FLOATS (24 * 3)

/* not every consumer needs every helper; keep -Wall quiet */
#if defined(__GNUC__) || defined(__clang__)
# define JL_UNUSED __attribute__((unused))
#else
# define JL_UNUSED
#endif

/* ------------------------------------------------------------------ *
 * index -> position
 * ------------------------------------------------------------------ */

typedef void (*jl_node_cb)(uint32_t index, float x, float y, float z, void *ud);

/*
 * Node index bits interleave on the three axes: bits 0-1 select x, 2-3 y,
 * 4-5 z, and every further bit doubles the result.  Index 0 is the centre
 * cube, so the walk never yields (0,0,0); when a shifted position collapses
 * back onto the centre the requested count grows instead.
 */
static JL_UNUSED void jl_walk(size_t count, jl_node_cb cb, void *ud)
{
    for (size_t i = 1; i <= count; ++i) {
        if (i > JL_MAX_NODES) break;

        unsigned x_mask = (unsigned)(i & 3);
        unsigned y_mask = (unsigned)(i & 0xc);
        unsigned z_mask = (unsigned)(i & 0x30);
        size_t   mult   = i >> 6;

        float x = x_mask == 1 ? -1.0f : x_mask == 2 ? 1.0f : 0.0f;
        float y = y_mask == 4 ? -1.0f : y_mask == 8 ? 1.0f : 0.0f;
        float z = z_mask == 0x10 ? -1.0f : z_mask == 0x20 ? 1.0f : 0.0f;

        while (mult) {
            x *= 2.0f; y *= 2.0f; z *= 2.0f;
            mult >>= 1;
        }
        if (x == 0.0f && y == 0.0f && z == 0.0f) {
            ++count;
            continue;
        }
        cb((uint32_t)i, x, y, z, ud);
    }
}

/* ------------------------------------------------------------------ *
 * cube
 * ------------------------------------------------------------------ */

/* unit cube corners: front (+Z) first, then back */
static const float jl_corners[8][3] = {
    { -0.5f, -0.5f,  0.5f }, {  0.5f, -0.5f,  0.5f },
    {  0.5f,  0.5f,  0.5f }, { -0.5f,  0.5f,  0.5f },
    { -0.5f, -0.5f, -0.5f }, {  0.5f, -0.5f, -0.5f },
    {  0.5f,  0.5f, -0.5f }, { -0.5f,  0.5f, -0.5f },
};

/* quads, same winding as the legacy renderer: front, back, left, right, top, bottom */
static const unsigned char jl_faces[6][4] = {
    { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 3, 7, 4 },
    { 1, 5, 6, 2 }, { 3, 2, 6, 7 }, { 0, 1, 5, 4 },
};

static const float jl_normals[6][3] = {
    {  0.0f,  0.0f,  1.0f }, {  0.0f,  0.0f, -1.0f },
    { -1.0f,  0.0f,  0.0f }, {  1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f }, {  0.0f, -1.0f,  0.0f },
};

static const float jl_colors[6][3] = {
    { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 1.0f },
};

/* the 8 corners of an axis-aligned box, in jl_corners order */
static JL_UNUSED void jl_box_corners(float out[8][3], float cx, float cy, float cz, float size)
{
    for (int i = 0; i < 8; ++i) {
        out[i][0] = cx + jl_corners[i][0] * size;
        out[i][1] = cy + jl_corners[i][1] * size;
        out[i][2] = cz + jl_corners[i][2] * size;
    }
}

/* expanded mesh: 24 x (position, normal, colour) followed by 36 indices */
static JL_UNUSED void jl_cube_mesh(float *v, unsigned short *idx)
{
    for (int f = 0; f < 6; ++f) {
        for (int k = 0; k < 4; ++k) {
            int o = (f * 4 + k) * 9;
            const float *p = jl_corners[jl_faces[f][k]];
            v[o + 0] = p[0]; v[o + 1] = p[1]; v[o + 2] = p[2];
            v[o + 3] = jl_normals[f][0];
            v[o + 4] = jl_normals[f][1];
            v[o + 5] = jl_normals[f][2];
            v[o + 6] = jl_colors[f][0];
            v[o + 7] = jl_colors[f][1];
            v[o + 8] = jl_colors[f][2];
        }
        unsigned short b = (unsigned short)(f * 4);
        idx[f * 6 + 0] = b + 0; idx[f * 6 + 1] = b + 1; idx[f * 6 + 2] = b + 2;
        idx[f * 6 + 3] = b + 0; idx[f * 6 + 4] = b + 2; idx[f * 6 + 5] = b + 3;
    }
}

/* ------------------------------------------------------------------ *
 * picking
 * ------------------------------------------------------------------ */

/* slab test; returns 1 on hit and the entry distance along the ray in *tnear */
static JL_UNUSED int jl_ray_box(const float o[3], const float d[3],
                      const float bmin[3], const float bmax[3], float *tnear)
{
    float tmin = -INFINITY, tmax = INFINITY;
    for (int i = 0; i < 3; ++i) {
        if (fabsf(d[i]) < 1e-9f) {
            if (o[i] < bmin[i] || o[i] > bmax[i]) return 0;
        } else {
            float ood = 1.0f / d[i];
            float t1 = (bmin[i] - o[i]) * ood;
            float t2 = (bmax[i] - o[i]) * ood;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax || tmax < 0) return 0;
        }
    }
    *tnear = tmin >= 0 ? tmin : tmax;
    return 1;
}

/* 12 edges of an axis-aligned box, as 24 line-list vertices */
static JL_UNUSED void jl_box_edges(float *out, float cx, float cy, float cz, float size)
{
    static const unsigned char e[12][2] = {
        {0,1},{1,2},{2,3},{3,0}, {4,5},{5,6},{6,7},{7,4}, {0,4},{1,5},{2,6},{3,7}
    };
    float p[8][3];
    jl_box_corners(p, cx, cy, cz, size);

    int k = 0;
    for (int i = 0; i < 12; ++i) {
        out[k++] = p[e[i][0]][0]; out[k++] = p[e[i][0]][1]; out[k++] = p[e[i][0]][2];
        out[k++] = p[e[i][1]][0]; out[k++] = p[e[i][1]][1]; out[k++] = p[e[i][1]][2];
    }
}

#endif /* JSON_LAYOUT_H */
