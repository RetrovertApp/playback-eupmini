///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// eupmini Playback Plugin
//
// Implements RVPlaybackPlugin interface for FM TOWNS Euphony (.eup) music format.
// Uses the eupmini library for FM TOWNS sound emulation (6 FM + 8 PCM channels).
// The library uses global state (pcm struct), so only one file at a time.
//
// Audio output uses an in-memory callback, bypassing eupmini's FILE* and SDL
// ring-buffer paths.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" {
#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
}

#include "eupplayer.hpp"
#include "eupplayer_townsEmulator.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

// Define the global pcm struct required by eupmini
struct pcm_struct pcm;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define EUP_SAMPLE_RATE 44100
#define EUP_HEADER_SIZE 2048
// Default song length: 5 minutes (Euphony files don't embed duration)
#define DEFAULT_LENGTH_MS (5 * 60 * 1000)

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
extern "C" { RV_PLUGIN_USE_LOG_API(); }

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct EupminiReplayerData {
    EUPPlayer* player;
    EUP_TownsEmulator* device;
    uint8_t* file_data;
    size_t file_size;
    int file_open;
    int elapsed_frames;
    int max_frames;
    std::vector<int16_t> pending_samples;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_collect_pcm(void* user_data, int16_t const* samples, size_t frame_count) {
    auto* data = static_cast<EupminiReplayerData*>(user_data);
    data->pending_samples.insert(data->pending_samples.end(), samples, samples + frame_count * 2);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* eupmini_plugin_supported_extensions(void) {
    return "eup";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* eupmini_plugin_create(const RVService* service_api) {
    (void)service_api;
    return new (std::nothrow) EupminiReplayerData{};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int eupmini_plugin_destroy(void* user_data) {
    auto* data = (EupminiReplayerData*)user_data;

    if (data->player) {
        data->player->stopPlaying();
        delete data->player;
    }
    delete data->device;
    free(data->file_data);
    delete data;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int eupmini_plugin_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    auto* data = (EupminiReplayerData*)user_data;

    // Clean up previous
    if (data->player) {
        data->player->stopPlaying();
        delete data->player;
        data->player = nullptr;
    }
    delete data->device;
    data->device = nullptr;
    free(data->file_data);
    data->file_data = nullptr;
    data->pending_samples.clear();
    data->file_open = 0;

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("eupmini: Failed to load %s to memory", url);
        return -1;
    }

    // EUP files need at least the 2048-byte header
    if (read_res.data_size < EUP_HEADER_SIZE + 6) {
        rv_error("eupmini: File too small for EUP format: %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    // Keep a copy of the file data
    data->file_data = (uint8_t*)malloc((size_t)read_res.data_size);
    if (data->file_data == nullptr) {
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }
    memcpy(data->file_data, read_res.data, (size_t)read_res.data_size);
    data->file_size = (size_t)read_res.data_size;
    rv_io_free_url_to_memory(read_res.data);

    uint8_t* buf = data->file_data;

    // Create emulator and player
    data->device = new EUP_TownsEmulator;
    data->player = new EUPPlayer;

    // Configure output format: 16-bit signed stereo, little-endian
    data->device->outputSampleUnsigned(false);
    data->device->outputSampleLSBFirst(true);
    data->device->outputSampleSize(2);
    data->device->outputSampleChannels(2);
    data->device->rate(EUP_SAMPLE_RATE);

    data->device->outputCallback(eupmini_collect_pcm, data);

    data->player->outputDevice(data->device);

    // Parse EUP header: track -> MIDI channel mapping (32 tracks)
    for (int trk = 0; trk < 32; trk++) {
        data->player->mapTrack_toChannel(trk, buf[0x394 + trk]);
    }

    // Assign FM devices to channels (6 FM channels)
    for (int i = 0; i < 6; i++) {
        data->device->assignFmDeviceToChannel(buf[0x6D4 + i]);
    }

    // Assign PCM devices to channels (8 PCM channels)
    for (int i = 0; i < 8; i++) {
        data->device->assignPcmDeviceToChannel(buf[0x6DA + i]);
    }

    // Note: FM/PCM instrument banks (.fmb/.pmb) are not loaded here since
    // we'd need to resolve the filenames from the header and load them via IO API.
    // Without instrument banks, the emulator uses default sounds.
    // TODO: Load instrument banks from the same directory as the .eup file

    // Set initial tempo
    int tempo = buf[0x805] + 30;
    data->player->tempo(tempo);

    // Initialize eupmini's required global PCM state.
    memset(&pcm, 0, sizeof(pcm));

    // Start playback (skip 2048-byte header + 6-byte prefix)
    data->player->startPlaying(buf + EUP_HEADER_SIZE + 6);

    data->file_open = 1;
    data->elapsed_frames = 0;
    data->max_frames = (int)(((int64_t)DEFAULT_LENGTH_MS * EUP_SAMPLE_RATE) / 1000);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_plugin_close(void* user_data) {
    auto* data = (EupminiReplayerData*)user_data;

    if (data->player) {
        data->player->stopPlaying();
        delete data->player;
        data->player = nullptr;
    }
    delete data->device;
    data->device = nullptr;
    free(data->file_data);
    data->file_data = nullptr;
    data->pending_samples.clear();
    data->file_open = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult eupmini_plugin_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                                   uint64_t total_size) {
    (void)probe_data;
    (void)total_size;

    // EUP files don't have a strong magic number. Use extension check.
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr && strcasecmp(dot, ".eup") == 0) {
            // Additional check: file should be at least header size
            if (data_size >= EUP_HEADER_SIZE) {
                return RVProbeResult_Supported;
            }
            return RVProbeResult_Unsure;
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo eupmini_plugin_read_data(void* user_data, RVReadData dest) {
    auto* data = (EupminiReplayerData*)user_data;
    RVAudioFormat format = { RVAudioStreamFormat_S16, 2, EUP_SAMPLE_RATE };

    if (!data->file_open || data->player == nullptr) {
        return (RVReadInfo) { format, 0, RVReadStatus_Error};
    }

    if (data->elapsed_frames >= data->max_frames ||
        (!data->player->isPlaying() && data->pending_samples.empty())) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    int remaining_frames = data->max_frames - data->elapsed_frames;
    if (max_frames > (uint32_t)remaining_frames) {
        max_frames = (uint32_t)remaining_frames;
    }

    // Generate audio by calling nextTick until the callback has enough samples.
    while (data->player->isPlaying() && data->pending_samples.size() / 2 < max_frames) {
        data->player->nextTick();
    }

    size_t available_frames = data->pending_samples.size() / 2;
    if (available_frames > max_frames) {
        available_frames = max_frames;
    }

    size_t sample_count = available_frames * 2;
    if (sample_count > 0) {
        memcpy(dest.channels_output, data->pending_samples.data(), sample_count * sizeof(int16_t));
        data->pending_samples.erase(data->pending_samples.begin(),
                                    data->pending_samples.begin() + sample_count);
    }
    data->elapsed_frames += (int)available_frames;

    RVReadStatus status = RVReadStatus_Ok;
    if (data->elapsed_frames >= data->max_frames ||
        (!data->player->isPlaying() && data->pending_samples.empty())) {
        status = RVReadStatus_Finished;
    }

    return (RVReadInfo) { format, (uint32_t)available_frames, status};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t eupmini_plugin_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int eupmini_plugin_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr || read_res.data_size < EUP_HEADER_SIZE) {
        if (read_res.data != nullptr) {
            rv_io_free_url_to_memory(read_res.data);
        }
        return -1;
    }

    RVMetadataId index = rv_metadata_create_url(url);

    // Extract title from header (32 bytes at offset 0)
    char title[33];
    memcpy(title, read_res.data, 32);
    title[32] = '\0';
    // Trim trailing spaces
    for (int i = 31; i >= 0 && (title[i] == ' ' || title[i] == '\0'); i--) {
        title[i] = '\0';
    }
    if (title[0] != '\0') {
        rv_metadata_set_tag(index, RV_METADATA_TITLE_TAG, title);
    }

    rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, "Euphony");
    rv_metadata_set_tag(index, RV_METADATA_AUTHORINGTOOL_TAG, "FM TOWNS");
    rv_metadata_set_tag_f64(index, RV_METADATA_LENGTH_TAG, DEFAULT_LENGTH_MS / 1000.0);

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_plugin_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_plugin_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_eupmini_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "eupmini",
    "0.0.1",
    "eupmini (Tomoaki Hayasaka)",
    eupmini_plugin_probe_can_play,
    eupmini_plugin_supported_extensions,
    eupmini_plugin_create,
    eupmini_plugin_destroy,
    eupmini_plugin_event,
    eupmini_plugin_open,
    eupmini_plugin_close,
    eupmini_plugin_read_data,
    eupmini_plugin_seek,
    eupmini_plugin_metadata,
    eupmini_plugin_static_init,
    nullptr, // settings_updated
    nullptr, // static_destroy

    // Visualization: none (caps = 0; pure decoder, no pattern grid or scope).
    nullptr, // get_structure
    nullptr, // get_columns
    nullptr, // get_pattern_channels
    nullptr, // get_scope_channels
    nullptr, // get_position
    nullptr, // get_channel_rows
    nullptr, // get_cells
    nullptr, // set_scope_enabled
    nullptr, // get_scope_samples
    nullptr, // get_vu
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_eupmini_plugin;
}
