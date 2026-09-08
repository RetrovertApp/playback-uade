#include <retrovert/io.h>
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

// The plugin reads through RVIo, so the harness has to supply one; stdio is
// enough for a local file.
static bool io_exists(RVIoPrivate*, const char* url) {
    std::FILE* file = std::fopen(url, "rb");
    if (!file)
        return false;
    std::fclose(file);
    return true;
}

static RVIoReadUrlResult io_read_url_to_memory(RVIoPrivate*, const char* url) {
    RVIoReadUrlResult result = { nullptr, 0 };
    std::FILE* file = std::fopen(url, "rb");
    if (!file)
        return result;
    if (std::fseek(file, 0, SEEK_END) == 0) {
        long size = std::ftell(file);
        if (size >= 0 && std::fseek(file, 0, SEEK_SET) == 0) {
            auto* data = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size)));
            if (data && std::fread(data, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size)) {
                result.data = data;
                result.data_size = static_cast<uint64_t>(size);
            } else {
                std::free(data);
            }
        }
    }
    std::fclose(file);
    return result;
}

static void io_free_url_to_memory(RVIoPrivate*, void* memory) {
    std::free(memory);
}

static const RVIo* get_io(RVServicePrivData*, int) {
    static const RVIo io = { nullptr, io_exists, io_read_url_to_memory, io_free_url_to_memory };
    return &io;
}

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
    RVService service = { nullptr, get_io, get_log, get_metadata, nullptr };
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
