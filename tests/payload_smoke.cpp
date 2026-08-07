#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>

#include <cstdint>
#include <cstdio>
#include <vector>

static void log_message(RVLogPrivate*, uint32_t, const char*, int, const char*, ...) {}

static const RVLog* get_log(RVServicePrivData*, int) {
    static const RVLog log = { nullptr, log_message };
    return &log;
}

static const RVMetadata* get_metadata(RVServicePrivData*, int) {
    static const RVMetadata metadata = {};
    return &metadata;
}

static int fail(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 3)
        return fail("usage: uade_payload_smoke <plugin> <module>");

    char plugin_path[PATH_MAX];
    if (!realpath(argv[1], plugin_path))
        return fail("cannot resolve plugin path");
    unsetenv("HOME");

    void* library = dlopen(plugin_path, RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return fail(dlerror());
    auto get_plugin = reinterpret_cast<RVPlaybackPlugin* (*)()>(dlsym(library, "rv_playback_plugin"));
    if (!get_plugin)
        return fail("plugin entry point is missing");

    RVPlaybackPlugin* plugin = get_plugin();
    RVService service = { nullptr, nullptr, get_log, get_metadata, nullptr };
    plugin->static_init(&service);
    void* instance = plugin->create(&service);
    if (!instance)
        return fail("plugin instance creation failed");
    if (plugin->open(instance, argv[2], 0, &service) != 0)
        return fail("module open failed");

    std::vector<int16_t> samples(4096);
    bool decoded = false;
    for (int attempt = 0; attempt < 512; ++attempt) {
        RVReadData output = { samples.data(), static_cast<uint32_t>(samples.size() * sizeof(int16_t)), {} };
        RVReadInfo info = plugin->read_data(instance, output);
        if (info.status == RVReadStatus_Error)
            return fail("decode failed");
        if (info.frame_count > 0) {
            decoded = true;
            break;
        }
        if (info.status == RVReadStatus_Finished)
            break;
    }

    plugin->close(instance);
    plugin->destroy(instance);
    dlclose(library);
    return decoded ? 0 : fail("decode produced no audio");
}
