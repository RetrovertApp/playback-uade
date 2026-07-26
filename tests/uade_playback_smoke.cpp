#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

const RVLog* get_log(RVServicePrivData*, int);
const RVMetadata* get_metadata(RVServicePrivData*, int);

void log_message(RVLogPrivate*, uint32_t level, const char*, int, const char* format, ...)
{
    std::fprintf(stderr, "[plugin:%u] ", level);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

RVMetadataId metadata_create_url(RVMetadataPrivate*, const char*)
{
    return 1;
}

void metadata_set_tag(RVMetadataPrivate*, RVMetadataId, const char*, const char*) {}
void metadata_set_tag_f64(RVMetadataPrivate*, RVMetadataId, const char*, double) {}
void metadata_add_subsong(RVMetadataPrivate*, RVMetadataId, uint32_t, const char*, float) {}
void metadata_add_sample(RVMetadataPrivate*, RVMetadataId, const char*) {}
void metadata_add_instrument(RVMetadataPrivate*, RVMetadataId, const char*) {}

const RVLog log_api = { nullptr, log_message };
const RVMetadata metadata_api = {
    nullptr,
    metadata_create_url,
    metadata_set_tag,
    metadata_set_tag_f64,
    metadata_add_subsong,
    metadata_add_sample,
    metadata_add_instrument,
};

const RVLog* get_log(RVServicePrivData*, int version)
{
    return version == RV_LOG_API_VERSION ? &log_api : nullptr;
}

const RVMetadata* get_metadata(RVServicePrivData*, int version)
{
    return version == RV_METADATA_API_VERSION ? &metadata_api : nullptr;
}

bool write_test_module(const char* path)
{
    // One-pattern, one-sample ProTracker module. The short looping waveform
    // gives the decoder deterministic, non-silent input without a test asset.
    std::vector<uint8_t> module(1084 + 1024 + 64, 0);
    const char title[] = "Retrovert UADE smoke";
    std::memcpy(&module[0], title, sizeof(title) - 1);

    const size_t sample_header = 20;
    const char sample_name[] = "deterministic wave";
    std::memcpy(&module[sample_header], sample_name, sizeof(sample_name) - 1);
    module[sample_header + 22] = 0;
    module[sample_header + 23] = 32; // 32 words = 64 bytes
    module[sample_header + 25] = 64; // volume
    module[sample_header + 28] = 0;
    module[sample_header + 29] = 32; // loop over the complete sample

    module[950] = 1; // song length
    std::memcpy(&module[1080], "M.K.", 4);

    // Sample 1, period 428 (C-3), no effect.
    module[1084] = 0x01;
    module[1085] = 0xac;
    module[1086] = 0x10;
    module[1087] = 0x00;

    static const int8_t waveform[16] = {
        0, 24, 48, 72, 96, 120, 96, 72,
        48, 24, 0, -24, -48, -72, -96, -120,
    };
    for (size_t i = 0; i < 64; ++i)
        module[1084 + 1024 + i] = static_cast<uint8_t>(waveform[i % 16]);

    FILE* file = std::fopen(path, "wb");
    if (!file)
        return false;
    const bool ok = std::fwrite(module.data(), 1, module.size(), file) == module.size();
    return std::fclose(file) == 0 && ok;
}

struct DecodeResult {
    uint64_t hash;
    uint32_t frames;
    bool non_silent;
};

DecodeResult decode(RVPlaybackPlugin* plugin, const RVService* services, const char* module_path)
{
    DecodeResult result = { 14695981039346656037ULL, 0, false };
    void* instance = plugin->create(services);
    if (!instance)
        return { 0, 0, false };

    if (plugin->open(instance, module_path, 0, services) != 0) {
        plugin->destroy(instance);
        return { 0, 0, false };
    }

    const uint32_t target_frames = 32768;
    std::vector<int16_t> pcm(4096 * 2);
    unsigned int empty_reads = 0;
    while (result.frames < target_frames) {
        RVReadData request = {};
        request.channels_output = pcm.data();
        request.channels_output_max_bytes_size = static_cast<uint32_t>(pcm.size() * sizeof(pcm[0]));
        const RVReadInfo read = plugin->read_data(instance, request);
        if (read.status == RVReadStatus_Error)
            break;
        if (read.frame_count == 0) {
            if (read.status == RVReadStatus_Finished || ++empty_reads == 8)
                break;
            continue;
        }

        empty_reads = 0;
        uint32_t frames = read.frame_count;
        if (frames > target_frames - result.frames)
            frames = target_frames - result.frames;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
        const size_t byte_count = static_cast<size_t>(frames) * 2 * sizeof(int16_t);
        for (size_t i = 0; i < byte_count; ++i) {
            result.hash ^= bytes[i];
            result.hash *= 1099511628211ULL;
            result.non_silent = result.non_silent || bytes[i] != 0;
        }
        result.frames += frames;
    }

    plugin->close(instance);
    plugin->destroy(instance);
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <uade plugin>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
    HMODULE library = LoadLibraryA(argv[1]);
    if (!library) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 1;
    }
    RVPlaybackPluginGetFunc get_plugin
        = reinterpret_cast<RVPlaybackPluginGetFunc>(GetProcAddress(library, "rv_playback_plugin"));
#else
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    RVPlaybackPluginGetFunc get_plugin
        = reinterpret_cast<RVPlaybackPluginGetFunc>(dlsym(library, "rv_playback_plugin"));
#endif
    if (!get_plugin) {
        std::fprintf(stderr, "rv_playback_plugin export not found\n");
        return 1;
    }

    RVService services = {};
    services.get_log = get_log;
    services.get_metadata = get_metadata;

    RVPlaybackPlugin* plugin = get_plugin();
    if (!plugin || plugin->api_version != RV_PLAYBACK_PLUGIN_API_VERSION) {
        std::fprintf(stderr, "invalid playback plugin API\n");
        return 1;
    }
    plugin->static_init(&services);

    const char module_path[] = "uade_playback_smoke.mod";
    if (!write_test_module(module_path)) {
        std::fprintf(stderr, "unable to write generated test module\n");
        return 1;
    }

    const DecodeResult first = decode(plugin, &services, module_path);
    const DecodeResult second = decode(plugin, &services, module_path);
    std::remove(module_path);

    std::printf("PCM_FNV1A64=%016llx FRAMES=%u NON_SILENT=%s\n",
                static_cast<unsigned long long>(first.hash), first.frames,
                first.non_silent ? "yes" : "no");
    if (first.frames != 32768 || !first.non_silent || first.hash != second.hash
        || first.frames != second.frames) {
        std::fprintf(stderr,
                     "playback verification failed; second hash=%016llx frames=%u non_silent=%s\n",
                     static_cast<unsigned long long>(second.hash), second.frames,
                     second.non_silent ? "yes" : "no");
        return 1;
    }

#ifdef _WIN32
    FreeLibrary(library);
#else
    dlclose(library);
#endif
    return 0;
}
