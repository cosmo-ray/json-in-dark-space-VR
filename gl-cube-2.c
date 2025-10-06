#define GL_SILENCE_DEPRECATION
#include <stdlib.h>
#include <string.h> // for memcpy
#include <stdio.h>
#include <math.h>
#include <OpenGL/gl.h>
#include <GLUT/glut.h>
#include "csonpath_json-c.h"

#define WIN_X 800
#define WIN_Y 600

static float angle_x = 0.0f;
static float angle_y = 0.0f;
static float zoom = -3;

static struct json_object *main_json;
static struct json_object *cur_json;

// 8 cube corners relative to (cx,cy,cz) will be computed
GLfloat vertices[8][3];
GLfloat colors[8][3] = {
    {1,0,0}, {0,1,0}, {0,0,1}, {1,1,0},
    {1,0,1}, {0,1,1}, {1,1,1}, {0.5,0.5,0.5}
};

GLfloat normals[24][3] = {
    // +Z front
    {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1},
    // -Z back
    {0,0,-1}, {0,0,-1}, {0,0,-1}, {0,0,-1},
    // +X right
    {1,0,0}, {1,0,0}, {1,0,0}, {1,0,0},
    // -X left
    {-1,0,0}, {-1,0,0}, {-1,0,0}, {-1,0,0},
    // +Y top
    {0,1,0}, {0,1,0}, {0,1,0}, {0,1,0},
    // -Y bottom
    {0,-1,0}, {0,-1,0}, {0,-1,0}, {0,-1,0}
};

// indices for 6 faces (each has 4 vertices = quad)
GLubyte indices[6][4] = {
    {0,1,2,3}, // front
    {4,5,6,7}, // back
    {0,3,7,4}, // left
    {1,5,6,2}, // right
    {3,2,6,7}, // top
    {0,1,5,4}  // bottom
};

// selection: store selected cube center; use an impossible initial value
int selected_exists = 0;
float selected_cx =0.0f, selected_cy = 0.0f, selected_cz = 0.0f;


static int frameCount = 0;
static float fps = 0.0f;
static int previousTime = 0;

void buildCube_(void *dst, size_t s, float cx, float cy, float cz, float size)
{
    float hs = size * 0.5f;

    // cube corners
    memcpy(dst, (GLfloat[8][3]){
        {cx-hs, cy-hs, cz+hs}, // 0 front-lower-left
        {cx+hs, cy-hs, cz+hs}, // 1 front-lower-right
        {cx+hs, cy+hs, cz+hs}, // 2 front-upper-right
        {cx-hs, cy+hs, cz+hs}, // 3 front-upper-left
        {cx-hs, cy-hs, cz-hs}, // 4 back-lower-left
        {cx+hs, cy-hs, cz-hs}, // 5 back-lower-right
        {cx+hs, cy+hs, cz-hs}, // 6 back-upper-right
        {cx-hs, cy+hs, cz-hs}  // 7 back-upper-left
      }, s);
}

void buildCube(float cx, float cy, float cz, float size)
{
    buildCube_(vertices, sizeof(vertices), cx, cy, cz, size);
}

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
    buildCube(cx, cy, cz, size);

    glEnableClientState(GL_VERTEX_ARRAY);
    //glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);

    glVertexPointer(3, GL_FLOAT, 0, vertices);
    glNormalPointer(GL_FLOAT, 0, normals);
    //glColorPointer(3, GL_FLOAT, 0, colors);

    for (int i = 0; i < 6; i++) {
	glColor3fv(colors[i]);
	glDrawElements(GL_QUADS, 4, GL_UNSIGNED_BYTE, indices[i]);
    }

    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_NORMAL_ARRAY);
}


// helper to draw a wireframe box around selected cube
void drawWireAABB(float cx, float cy, float cz, float size)
{
    float hs = size * 0.5f;
    // 8 corners (same orientation as vertices)
    GLfloat v[8][3];
    buildCube_(v, sizeof(vertices), cx, cy, cz, size);
    glPushAttrib(GL_ENABLE_BIT | GL_POLYGON_BIT | GL_CURRENT_BIT);    glColor3f(1.0f, 1.0f, 1.0f);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glBegin(GL_QUADS);
      // draw slightly inset quads so lines are visible
      for (int i=0;i<6;i++) {
        glVertex3fv(v[indices[i][0]]);
        glVertex3fv(v[indices[i][1]]);
        glVertex3fv(v[indices[i][2]]);
        glVertex3fv(v[indices[i][3]]);
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

void do_json(json_object *cur, void (*f)(float, float, float, void *), void *data)
{
    int last_node = json_object_array_length(cur);
    for (unsigned int count_nodes = 1; count_nodes <= last_node; ++count_nodes) {
      int x_mask = count_nodes & 3;
      int y_mask = count_nodes & 0xc;
      int z_mask = count_nodes & 0x30;
      unsigned int mult_mask = count_nodes >> 6;
      float x =  x_mask == 1 ? -1 : x_mask == 2 ? 1 : 0;
      float y = y_mask == 4 ? -1 : y_mask == 8 ? 1 : 0;
      float z = z_mask == 0x10 ? -1 : z_mask == 0x20 ? 1 : 0;
      while (mult_mask) {
	x *= 2;
	y *= 2;
	z *= 2;
	mult_mask = mult_mask >> 1;
      }
      if (x == 0 && y == 0 && z == 0) {
	++last_node;
	continue;
      }
      f(x,y,z,data);
    }
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

// Ray - AABB intersection: returns 1 if hit and sets *tNear to entry distance along ray (>=0).
// Ray: origin + t * dir  (dir must be non-zero, not necessarily normalized).
static int rayBoxIntersect(const double origin[3], const double dir[3],
                    const double boxMin[3], const double boxMax[3],
                    double *tNearOut)
{
    // slab method
    double tmin = -INFINITY;
    double tmax =  INFINITY;

    for (int i = 0; i < 3; ++i) {
        if (fabs(dir[i]) < 1e-9) {
            // ray parallel to slab: no hit if origin not within slab
            if (origin[i] < boxMin[i] || origin[i] > boxMax[i]) return 0;
        } else {
            double ood = 1.0 / dir[i];
            double t1 = (boxMin[i] - origin[i]) * ood;
            double t2 = (boxMax[i] - origin[i]) * ood;
            if (t1 > t2) { double tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return 0;
            if (tmax < 0) return 0; // box is behind ray
        }
    }
    if (tNearOut) *tNearOut = tmin >= 0.0 ? tmin : tmax;
    return 1;
}

struct col_data {
    float hitX, hitY, hitZ, hitSize;
    double bestT;
    int hit;
    double origin[3];
    double dir[3];

};

void do_col(float cx, float cy, float cz, void *data) {
    // skip the origin cube center? your code draws center too; keep it
    struct col_data *c = data;
    float size = ( (cx==0 && cy==0 && cz==0) ? 1.0f : 0.4f ); // match display size
    double hs = 0.5 * size;
    double boxMin[3] = {cx - hs, cy - hs, cz - hs};
    double boxMax[3] = {cx + hs, cy + hs, cz + hs};
    double tNear;
    if (rayBoxIntersect(c->origin, c->dir, boxMin, boxMax, &tNear)) {
	if (tNear >= 0.0 && tNear < c->bestT) {
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
	.origin={nearX, nearY, nearZ},
	.dir={ farX - nearX, farY - nearY, farZ - nearZ }
    };
    // optionally normalize dir for more stable t values
    double len = sqrt(cdata.dir[0]*cdata.dir[0] + cdata.dir[1]*cdata.dir[1] + cdata.dir[2]*cdata.dir[2]);
    if (len > 1e-12) { cdata.dir[0] /= len; cdata.dir[1] /= len; cdata.dir[2] /= len; }

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
    // load json from stdin
    char *main_path = NULL;
    char *in_file = NULL;

    for (int i = 1; i < argc; ++i) {
	if (!strcmp(argv[i], "--main-path")) {
	    main_path = argv[++i];
	} else if (!strcmp(argv[i], "--in-file")) {
	    in_file = argv[++i];
	}
    }
    main_json = json_object_from_fd(0);
    if (!main_json) {
	fprintf(stderr, "fail to create json from stdin\n");
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
