/*
 * json-gl-xr-doctor: print what this machine's OpenXR setup looks like.
 *
 * It deliberately links neither the OpenXR loader nor GLES: everything is
 * dlopen()/dlsym(), so the tool still runs (and says something useful) when
 * the loader is missing or the runtime cannot be reached. That is exactly the
 * case it exists for.
 *
 * Build: make doctor        Run: ./json-gl-xr-doctor
 */
#define _GNU_SOURCE             /* dladdr() and Dl_info */
#include <dlfcn.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <openxr/openxr.h>
#include "xr_result.h"

/* lives in openxr_platform.h, which would drag in the EGL/GL headers */
#define XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME "XR_KHR_opengl_es_enable"

#define MAXPATH 4096

static int is_file(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* target of a symlink, so we can show libopenxr_loader.so.1 -> ...1.0.20 */
static void print_link(const char *path)
{
    char buf[MAXPATH];
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        printf("            -> %s\n", buf);
    }
}

static void print_var(const char *name)
{
    const char *v = getenv(name);
    printf("  %-20s %s\n", name, (v && *v) ? v : "(unset)");
}

static void print_uname(void)
{
    struct utsname u;
    if (uname(&u) == 0)
        printf("  %-20s %s %s %s\n", "kernel", u.sysname, u.release, u.machine);
}

/* first value of a "key": "value" pair in a runtime manifest */
static const char *json_string(const char *text, const char *key, char *out, size_t n)
{
    char pat[128];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p = strchr(p, '"');
    if (!p) return NULL;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) {
        if (*p == '\\' && p[1]) ++p;
        out[i++] = *p++;
    }
    out[i] = 0;
    return out;
}

static void check_manifest(const char *path)
{
    if (!path || !*path) return;
    if (!is_file(path)) {
        printf("  MISSING  %s\n", path);
        return;
    }
    static char buf[64 * 1024];
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;

    char lib[MAXPATH];
    printf("  FOUND    %s\n", path);
    if (json_string(buf, "library_path", lib, sizeof lib))
        printf("            library_path = %s%s\n", lib,
               is_file(lib) ? "" : "   (no such file)");
    else
        printf("            no \"library_path\" in the file\n");
}

static void section_manifests(void)
{
    const char *xrj = getenv("XR_RUNTIME_JSON");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char paths[8][MAXPATH];
    int n = 0;

    printf("\n[runtime manifests the loader tries, in order]\n");
    if (xrj && *xrj) check_manifest(xrj);

    if (xdg && *xdg) {
        snprintf(paths[n++], MAXPATH, "%s/openxr/1/active_runtime.json", xdg);
    }
    if (home) snprintf(paths[n++], MAXPATH, "%s/.config/openxr/1/active_runtime.json", home);
    snprintf(paths[n++], MAXPATH, "/etc/xdg/openxr/1/active_runtime.json");
    snprintf(paths[n++], MAXPATH, "/usr/local/share/openxr/1/active_runtime.json");
    snprintf(paths[n++], MAXPATH, "/usr/share/openxr/1/active_runtime.json");
    if (home) {
        snprintf(paths[n++], MAXPATH,
                 "%s/.steam/steam/steamapps/common/SteamVR/steamxr_linux64.json", home);
        snprintf(paths[n++], MAXPATH,
                 "%s/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux64.json", home);
    }
    for (int i = 0; i < n; ++i) check_manifest(paths[i]);
}

static void check_lib(const char *soname, const char *symname)
{
    void *h = dlopen(soname, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("  %-20s MISSING (%s)\n", soname, dlerror());
        return;
    }
    void *sym = dlsym(h, symname);
    Dl_info info;
    if (sym && dladdr(sym, &info) && info.dli_fname)
        printf("  %-20s %s\n", soname, info.dli_fname);
    else
        printf("  %-20s loaded\n", soname);
    dlclose(h);
}

static void section_gl(void)
{
    printf("\n[EGL / GLES]\n");
    check_lib("libEGL.so.1", "eglGetDisplay");
    check_lib("libGLESv2.so.2", "glGetString");
    printf("  %-20s %s\n", "/dev/dri", is_dir("/dev/dri") ? "present" : "MISSING");

    const char *dir = "/usr/share/glvnd/egl_vendor.d";
    printf("  %-20s ", "egl vendors");
    DIR *d = opendir(dir);
    if (!d) {
        printf("%s: MISSING\n", dir);
        return;
    }
    struct dirent *e;
    int first = 1;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        printf("%s%s", first ? "" : ", ", e->d_name);
        first = 0;
    }
    closedir(d);
    printf("%s\n", first ? "(none)" : "");
}

int main(void)
{
    printf("json-gl-xr-doctor\n");
    printf("  %-20s %d.%d.%d\n", "openxr.h version",
           XR_VERSION_MAJOR(XR_CURRENT_API_VERSION),
           XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
           XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
    print_uname();
    print_var("XDG_SESSION_TYPE");
    print_var("DISPLAY");
    print_var("WAYLAND_DISPLAY");
    print_var("XR_RUNTIME_JSON");
    print_var("LD_LIBRARY_PATH");

    printf("\n[OpenXR loader]\n");
    void *loader = dlopen("libopenxr_loader.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        printf("  MISSING (%s)\n", dlerror());
        printf("\nresult: no OpenXR loader on this machine (libopenxr_loader.so.1)\n");
        return 0;
    }
    void *sym = dlsym(loader, "xrGetInstanceProcAddr");
    Dl_info info;
    if (sym && dladdr(sym, &info) && info.dli_fname) {
        printf("  %-20s %s\n", "path", info.dli_fname);
        print_link(info.dli_fname);
    } else {
        printf("  %-20s loaded, but xrGetInstanceProcAddr is missing\n", "path");
        printf("\nresult: libopenxr_loader.so.1 does not look like an OpenXR loader\n");
        return 0;
    }

    PFN_xrEnumerateInstanceExtensionProperties enum_ext =
        (PFN_xrEnumerateInstanceExtensionProperties)dlsym(loader, "xrEnumerateInstanceExtensionProperties");
    PFN_xrCreateInstance create_instance =
        (PFN_xrCreateInstance)dlsym(loader, "xrCreateInstance");
    PFN_xrDestroyInstance destroy_instance =
        (PFN_xrDestroyInstance)dlsym(loader, "xrDestroyInstance");
    PFN_xrGetSystem get_system =
        (PFN_xrGetSystem)dlsym(loader, "xrGetSystem");

    if (!enum_ext || !create_instance) {
        printf("  %-20s incomplete (no xrCreateInstance?)\n", "symbols");
        printf("\nresult: libopenxr_loader.so.1 is unusable\n");
        return 0;
    }

    section_manifests();
    section_gl();

    printf("\n[instance extensions]\n");
    uint32_t n = 0;
    XrResult r = enum_ext(NULL, 0, &n, NULL);
    if (XR_FAILED(r)) {
        printf("  xrEnumerateInstanceExtensionProperties -> %s\n", xr_result_name(r));
        printf("\nresult: no reachable OpenXR runtime (%s)\n", xr_result_name(r));
        printf("hint: start SteamVR (or the headset runtime) first, or set XR_RUNTIME_JSON\n");
        printf("      to the runtime manifest listed above\n");
        return 0;
    }

    XrExtensionProperties *props = calloc(n ? n : 1, sizeof *props);
    if (!props) return 1;
    for (uint32_t i = 0; i < n; ++i) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    r = enum_ext(NULL, n, &n, props);
    if (XR_FAILED(r)) {
        printf("  xrEnumerateInstanceExtensionProperties(2) -> %s\n", xr_result_name(r));
        free(props);
        printf("\nresult: no reachable OpenXR runtime (%s)\n", xr_result_name(r));
        return 0;
    }

    int gles = 0, gl = 0, vk = 0;
    printf("  %u extensions\n", n);
    for (uint32_t i = 0; i < n; ++i) {
        if (!strcmp(props[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) gles = 1;
        if (!strcmp(props[i].extensionName, "XR_KHR_opengl_enable")) gl = 1;
        if (!strcmp(props[i].extensionName, "XR_KHR_vulkan_enable2")) vk = 1;
    }
    printf("  %-28s %s\n", XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, gles ? "yes" : "NO");
    printf("  %-28s %s\n", "XR_KHR_opengl_enable", gl ? "yes" : "NO");
    printf("  %-28s %s\n", "XR_KHR_vulkan_enable2", vk ? "yes" : "NO");

    if (!gles) {
        free(props);
        printf("\nresult: runtime reachable, but it does not expose %s\n",
               XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME);
        return 0;
    }

    /* the real question: can we create an instance and find the headset? */
    printf("\n[instance]\n");
    const char *exts[] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    XrApplicationInfo ai;
    memset(&ai, 0, sizeof ai);
    snprintf(ai.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "json-gl-xr-doctor");
    snprintf(ai.engineName, XR_MAX_ENGINE_NAME_SIZE, "none");
    ai.apiVersion = XR_MAKE_VERSION(1, 0, 0);

    XrInstanceCreateInfo ci = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    ci.applicationInfo = ai;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;

    XrInstance inst = XR_NULL_HANDLE;
    r = create_instance(&ci, &inst);
    printf("  xrCreateInstance -> %s\n", xr_result_name(r));
    if (XR_FAILED(r)) {
        free(props);
        printf("\nresult: runtime reachable, but xrCreateInstance failed (%s)\n",
               xr_result_name(r));
        return 0;
    }

    if (get_system) {
        XrSystemGetInfo si = { .type = XR_TYPE_SYSTEM_GET_INFO };
        si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        XrSystemId sys = XR_NULL_SYSTEM_ID;
        r = get_system(inst, &si, &sys);
        printf("  xrGetSystem     -> %s%s\n", xr_result_name(r),
               XR_SUCCEEDED(r) ? "  (headset found)" : "  (no headset / not running)");
        if (destroy_instance) destroy_instance(inst);
        free(props);
        if (XR_FAILED(r)) {
            printf("\nresult: runtime reachable, no head-mounted display\n");
            return 0;
        }
    } else if (destroy_instance) {
        destroy_instance(inst);
    }

    free(props);
    printf("\nresult: OpenXR is usable here (loader + runtime + GLES + headset)\n");
    return 0;
}
