///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// eupmini Playback Plugin
//
// Implements RVPlaybackPlugin interface for FM TOWNS Euphony (.eup) music format.
// Uses the eupmini library for FM TOWNS sound emulation (6 FM + 8 PCM channels).
// The library uses global state (pcm struct), so only one file at a time.
//
// Audio output uses the library's output2File mode which writes S16 stereo samples
// to a FILE* stream, bypassing the ring buffer path that requires a concurrent
// SDL audio consumer.
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

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    // Temporary file for output2File mode (cross-platform, replaces POSIX open_memstream)
    FILE* mem_stream;
    long mem_read_pos;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* eupmini_plugin_supported_extensions(void) {
    return "eup";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* eupmini_plugin_create(const RVService* service_api) {
    auto* data = (EupminiReplayerData*)calloc(1, sizeof(EupminiReplayerData));
    if (data == nullptr) {
        return nullptr;
    }

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int eupmini_plugin_destroy(void* user_data) {
    auto* data = (EupminiReplayerData*)user_data;

    if (data->player) {
        data->player->stopPlaying();
        delete data->player;
    }
    delete data->device;
    if (data->mem_stream) {
        fclose(data->mem_stream);
    }
    free(data->file_data);
    free(data);
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
    if (data->mem_stream) {
        fclose(data->mem_stream);
        data->mem_stream = nullptr;
    }
    data->mem_read_pos = 0;
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

    // Use output2File mode: writes S16 samples to a FILE* stream,
    // bypassing the ring buffer path that deadlocks without SDL audio thread.
    // Uses tmpfile() for cross-platform compatibility (open_memstream is POSIX-only).
    data->mem_stream = tmpfile();
    if (data->mem_stream == nullptr) {
        rv_error("eupmini: Failed to create memory stream");
        delete data->player;
        data->player = nullptr;
        delete data->device;
        data->device = nullptr;
        free(data->file_data);
        data->file_data = nullptr;
        return -1;
    }
    data->device->output2File(true);
    data->device->outputStream(data->mem_stream);

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

    // Initialize the pcm struct (count is used by output2File mode)
    memset(&pcm, 0, sizeof(pcm));

    // Start playback (skip 2048-byte header + 6-byte prefix)
    data->player->startPlaying(buf + EUP_HEADER_SIZE + 6);

    data->file_open = 1;
    data->elapsed_frames = 0;
    data->max_frames = (int)(((int64_t)DEFAULT_LENGTH_MS * EUP_SAMPLE_RATE) / 1000);
    data->mem_read_pos = 0;

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
    if (data->mem_stream) {
        fclose(data->mem_stream);
        data->mem_stream = nullptr;
    }
    data->mem_read_pos = 0;
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

    if (!data->player->isPlaying() || data->elapsed_frames >= data->max_frames) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);

    // Generate audio by calling nextTick until we have enough samples
    while (data->player->isPlaying()) {
        fflush(data->mem_stream);
        long write_pos = ftell(data->mem_stream);
        size_t available_bytes = (size_t)(write_pos - data->mem_read_pos);
        size_t available_frames = available_bytes / (sizeof(int16_t) * 2);
        if (available_frames >= max_frames) {
            break;
        }
        data->player->nextTick();
    }

    fflush(data->mem_stream);

    // Read S16 samples from temp file
    long write_pos = ftell(data->mem_stream);
    size_t available_bytes = (size_t)(write_pos - data->mem_read_pos);
    size_t available_frames = available_bytes / (sizeof(int16_t) * 2);
    if (available_frames > max_frames) {
        available_frames = max_frames;
    }

    // Seek to read position, read S16 data directly to output, then seek back to end for writes
    fseek(data->mem_stream, data->mem_read_pos, SEEK_SET);
    size_t read_count = fread(dest.channels_output, sizeof(int16_t) * 2, available_frames, data->mem_stream);
    fseek(data->mem_stream, 0, SEEK_END);

    data->mem_read_pos += (long)(read_count * sizeof(int16_t) * 2);
    data->elapsed_frames += (int)read_count;

    RVReadStatus status = RVReadStatus_Ok;
    if (!data->player->isPlaying() || data->elapsed_frames >= data->max_frames) {
        status = RVReadStatus_Finished;
    }

    return (RVReadInfo) { format, (uint32_t)read_count, status};
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
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    nullptr, // get_scope_data
    nullptr, // static_destroy
    nullptr, // get_scope_channel_names
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_eupmini_plugin;
}
