
# ---------------------------------------------------------------------------
# dependencies: git submodules (HTTPS, pinned)
#
#   third_party/yyjson    JSON              github.com/ibireme/yyjson
#   third_party/stb       stb_easy_font     github.com/nothings/stb
#   third_party/csonpath  path expressions  github.com/outscale/csonpath
#
#   make deps                    fetch them after a plain clone
# ---------------------------------------------------------------------------
SUBMODULES := third_party/yyjson/src/yyjson.h \
              third_party/stb/stb_easy_font.h \
              third_party/csonpath/csonpath.h

deps:
	git submodule update --init --recursive

$(SUBMODULES):
	@echo "submodules missing: run 'make deps'" >&2
	@exit 1

# ---------------------------------------------------------------------------
# classic desktop renderer (OpenGL 1.4 + GLUT, macOS) - untouched
# ---------------------------------------------------------------------------
json-gl: $(SUBMODULES) gl-cube-2.c json_layout.h
	$(CC) gl-cube-2.c -O3 -g -Ithird_party/csonpath/ -framework GLUT -framework OpenGL  $(shell pkg-config --cflags json-c) -o json-gl $(shell pkg-config --libs json-c)

# ---------------------------------------------------------------------------
# OpenXR / OpenGL ES 3.2 renderer
#
#   make vr                      native build (needs glfw3, EGL, GLES3, OpenXR)
#   make vr API=API_VK           same file, Vulkan backend (not implemented)
#   make vr-linux                aarch64 Linux build inside docker (arm64 native)
#   make appimage                dist/json-gl-xr-aarch64.AppImage
# ---------------------------------------------------------------------------
API ?= API_GLES
XR_BIN := json-gl-xr

VR_SRC  := vr.c third_party/yyjson/src/yyjson.c
VR_DEPS := $(SUBMODULES) vr.c json_layout.h \
           third_party/yyjson/src/yyjson.c third_party/csonpath/csonpath_yyjson.h
VR_CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
             -Wno-unused-function -Wno-missing-field-initializers \
             -Ithird_party/csonpath -Ithird_party/yyjson/src -Ithird_party/stb \
             -DAPI=$(API)
VR_LIBS := $(shell pkg-config --cflags --libs glfw3 2>/dev/null) -lEGL -lGLESv2 -lopenxr_loader -lm

vr: $(VR_DEPS)
	$(CC) $(VR_CFLAGS) $(VR_SRC) -o $(XR_BIN) $(VR_LIBS)

DOCKER_IMG := json-gl-xr-build

docker-image:
	docker build --platform linux/arm64 -t $(DOCKER_IMG) packaging

vr-linux: $(VR_DEPS) docker-image
	docker run --rm -v "$(CURDIR)":/src -w /src $(DOCKER_IMG) \
		make vr API=$(API)

appimage: vr-linux
	docker run --rm -v "$(CURDIR)":/src -w /src $(DOCKER_IMG) sh packaging/appimage.sh

deploy: appimage
	@echo "copy dist/json-gl-xr-aarch64.AppImage to the Steam Frame (devkit or scp)"

clean:
	rm -rf json-gl json-gl-xr json-gl.dSYM build/AppDir dist

.PHONY: deps json-gl vr vr-linux docker-image appimage deploy clean
