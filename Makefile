
json-gl: gl-cube-2.c
	$(CC) gl-cube-2.c -O3 -g -Icsonpath/   -framework GLUT -framework OpenGL  $(shell pkg-config --cflags json-c) -o json-gl $(shell pkg-config --libs json-c)
