# JSON in dark space

Walk a JSON array as a 3D graph: one cube per element, laid out by
bit-interleaving the index on three axes. A ray from your hand picks a node and
a floating panel shows its csonpath, type and value.

Two renderers share the same data, command line and layout code
(`json_layout.h`); each front-end only owns its rendering path:

| target  | source        | renderer                            | platform              |
| ------- | ------------- | ----------------------------------- | --------------------- |
| `json-gl`  | `gl-cube-2.c` | OpenGL 1.4 + GLUT (legacy)        | macOS                 |
| `json-gl-xr` | `vr.c`      | OpenGL ES 3.2 + OpenXR            | Linux aarch64 (Steam Frame) |

## Legacy desktop renderer

```sh
make json-gl
./json-gl --in-file example.json
```

![image](./Screenshot%202025-10-06%20at%2015.51.43.png)

## OpenXR renderer (Steam Frame / Linux aarch64)

`vr.c` is a single file: JSON loading, the GLES 3.2 renderer and the OpenXR
session, with the shared layout in `json_layout.h`. The graph is drawn instanced,
the panel with `stb_easy_font`.

The backend is picked at compile time (`-DAPI=API_GLES`, the only one for now);
there is no runtime dispatch. Vulkan would be a second branch in the same file.

### Sources

The dependencies are git submodules, fetched over HTTPS and pinned to a commit:

| path                  | project                                          |
| --------------------- | ------------------------------------------------ |
| `third_party/yyjson`  | [ibireme/yyjson](https://github.com/ibireme/yyjson) — JSON |
| `third_party/stb`     | [nothings/stb](https://github.com/nothings/stb) — `stb_easy_font` |
| `third_party/csonpath`| [outscale/csonpath](https://github.com/outscale/csonpath) — path expressions |

```sh
git clone --recursive <url>
# in an existing checkout:
make deps
```

GL/EGL/OpenXR/GLFW come from the distribution, not from the repository.

### Build (dev machine)

Nothing is built on the headset: it runs the AppImage as-is.

macOS builds the aarch64 target in a colima/Docker container:

```sh
make appimage          # -> dist/json-gl-xr-aarch64.AppImage
```

On an aarch64 Linux host (and in CI) build natively instead, with
`libglfw3-dev libegl-dev libgles-dev libopenxr-dev`:

```sh
make vr
make appimage-native   # same packaging script, no docker
```

`make docker-image` and `make vr-linux` remain for the container-only case.

### Run

```sh
./json-gl-xr --in-file example.json          # OpenXR session
./json-gl-xr --in-file example.json --main-path '$.items'
./json-gl-xr --desktop --in-file example.json # side-by-side stereo in a window
./json-gl-xr --list-xr-extensions             # what the runtime offers
```

Same input rules as `json-gl`: `--in-file FILE` or JSON on stdin, `--main-path`
is a csonpath expression selecting the root array (default `$`).

Controls: trigger selects the hovered cube, menu resets the view, left stick
rotates and scales the graph. `--desktop` uses the mouse instead.

### Headset

The AppImage is the deliverable; there is no toolchain on the Steam Frame.

```sh
make appimage          # macOS / docker
make appimage-native   # aarch64 Linux, no docker
# -> dist/json-gl-xr-aarch64.AppImage
```

Copy it to the devkit and run it. If the runtime does not expose
`XR_KHR_opengl_es_enable` the binary prints a warning and falls back to
`--desktop`; use `--list-xr-extensions` to see the available bindings. SteamVR
on Linux is Vulkan-first, so GLES support is the known risk here — that is what
the compile-time backend switch exists for.

Every push is built by `.github/workflows/nightly.yml` on an arm64 runner and
the AppImage is run under Xvfb. Pushes to `main` replace the rolling `nightly`
pre-release of this repository with the fresh binary and its `sha256`, so
`/releases/tag/nightly` always matches the last commit.

## Files

| path                 | what                                            |
| -------------------- | ----------------------------------------------- |
| `gl-cube-2.c`        | legacy GLUT front-end                           |
| `vr.c`               | OpenXR / GLES front-end                         |
| `json_layout.h`      | layout, cube mesh and picking shared by both    |
| `third_party/`       | submodules: yyjson, stb, csonpath               |
| `.gitmodules`        | HTTPS urls; each dependency is pinned by commit |
| `packaging/`         | arm64 build image and AppImage script           |
| `.github/workflows/` | nightly build + rolling `nightly` release       |
| `example.json`       | sample data (36 elements)                       |

Build products (`json-gl`, `json-gl-xr`, `build/`, `dist/`) are ignored.

## Layout

Element `i` becomes a cube at coordinates derived from the bits of the index
(`jl_walk()` in `json_layout.h`, shared by both renderers). The origin cube
marks `$`. Selection is an axis-aligned box raycast against those cubes, done in
model space so the ray stays correct while the graph rotates and scales.
