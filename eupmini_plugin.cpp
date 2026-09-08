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
#include <algorithm>
#include <cstdio>
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

// The emulator has 16 channels; a EUP header assigns at most 6 FM and 8 PCM
// devices among them, and only the assigned ones are worth showing.
#define EUP_EMULATOR_CHANNELS 16
#define EUP_MAX_SCOPE_CHANNELS 14
// Per-channel scope history, as a ring. Power of two: the index wraps with a mask.
#define EUP_SCOPE_WINDOW 2048
// Window the VU peak is taken over.
#define EUP_VU_WINDOW 512

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

    // Visualization. scope_slot maps an emulator channel to its scope index, or
    // -1 for a channel the header assigned no device to.
    int scope_slot[EUP_EMULATOR_CHANNELS];
    char scope_names[EUP_MAX_SCOPE_CHANNELS][24];
    uint32_t scope_count;
    bool scope_enabled;
    std::vector<float> scope_ring;  // scope_count * EUP_SCOPE_WINDOW
    uint32_t scope_pos;
    uint32_t scope_tick_frames;  // frames the current tick reported, advanced by read_data
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_collect_pcm(void* user_data, int16_t const* samples, size_t frame_count) {
    auto* data = static_cast<EupminiReplayerData*>(user_data);
    data->pending_samples.insert(data->pending_samples.end(), samples, samples + frame_count * 2);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Give one emulator channel a scope slot. The header can name the same channel
// twice, or a channel outside the emulator's range, so both are rejected.

static void eupmini_map_scope_channel(EupminiReplayerData* data, int channel, const char* kind, int index) {
    if (channel < 0 || channel >= EUP_EMULATOR_CHANNELS) {
        return;
    }
    if (data->scope_slot[channel] >= 0 || data->scope_count >= EUP_MAX_SCOPE_CHANNELS) {
        return;
    }

    int slot = (int)data->scope_count++;
    data->scope_slot[channel] = slot;
    snprintf(data->scope_names[slot], sizeof(data->scope_names[slot]), "%s %d", kind, index);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Called by the patched emulator once per channel per tick, before the channels
// are summed. The samples are already scaled the way the mix scales them.
//
// This runs while nextTick() is generating, which is ahead of the audio the host
// has actually been handed by whatever is still queued in pending_samples -- at
// most one tick, so the scope leads the output very slightly.

static void eupmini_collect_scope(void* user_data, int channel, int16_t const* samples, size_t frame_count) {
    auto* data = static_cast<EupminiReplayerData*>(user_data);
    if (!data->scope_enabled || channel < 0 || channel >= EUP_EMULATOR_CHANNELS) {
        return;
    }

    int slot = data->scope_slot[channel];
    if (slot < 0) {
        return;
    }

    // Every channel of a tick writes the same span, so the position stays put
    // here and read_data advances it once the whole tick has been reported.
    data->scope_tick_frames = (uint32_t)frame_count;
    uint32_t pos = data->scope_pos;
    float* ring = &data->scope_ring[(size_t)slot * EUP_SCOPE_WINDOW];
    for (size_t frame = 0; frame < frame_count; frame++) {
        // Left of the interleaved stereo pair is enough for a scope trace.
        ring[(pos + frame) & (EUP_SCOPE_WINDOW - 1)] = (float)samples[frame * 2] * (1.0f / 32768.0f);
    }
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

    for (int i = 0; i < EUP_EMULATOR_CHANNELS; i++) {
        data->scope_slot[i] = -1;
    }
    data->scope_count = 0;
    data->scope_pos = 0;
    data->scope_tick_frames = 0;

    // Seed every FM program with the default instrument, exactly as eupmini's
    // own player does before it looks for a .fmb bank. Without it every
    // operator keeps attack rate 0, which the emulator correctly reads as an
    // envelope that never rises -- the whole song decodes to silence. That was
    // hidden until now by an integer overflow in the attack-time division,
    // which wrapped to a negative divisor and read outside the attack table;
    // see patches/attack-time-overflow.patch.
    {
        static const uint8_t default_fm_instrument[] = {
            ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',  // name
            17,  33,  10,  17,                       // detune / multiple
            25,  10,  57,  0,                        // output level
            154, 152, 218, 216,                      // key scale / attack rate
            15,  12,  7,   12,                       // amon / decay rate
            0,   5,   3,   5,                        // sustain rate
            38,  40,  70,  40,                       // sustain level / release rate
            20,                                      // feedback / algorithm
            0xc0,                                    // pan, LFO
            0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        };
        for (int n = 0; n < 128; n++) {
            data->device->setFmInstrumentParameter(n, default_fm_instrument);
        }
    }

    // Assign FM devices to channels (6 FM channels)
    for (int i = 0; i < 6; i++) {
        data->device->assignFmDeviceToChannel(buf[0x6D4 + i]);
        eupmini_map_scope_channel(data, buf[0x6D4 + i], "FM", i + 1);
    }

    // Assign PCM devices to channels (8 PCM channels)
    for (int i = 0; i < 8; i++) {
        data->device->assignPcmDeviceToChannel(buf[0x6DA + i]);
        eupmini_map_scope_channel(data, buf[0x6DA + i], "PCM", i + 1);
    }

    data->scope_ring.assign((size_t)data->scope_count * EUP_SCOPE_WINDOW, 0.0f);
    // The callback is installed only while the scope is on: with it set, the
    // emulator renders every channel into its own buffer instead of straight
    // into the mix, which is not worth paying for when nothing is drawing it.
    if (data->scope_enabled && data->scope_count != 0) {
        data->device->scopeCallback(eupmini_collect_scope, data);
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

    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(int16_t) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
    int remaining_frames = data->max_frames - data->elapsed_frames;
    if (max_frames > (uint32_t)remaining_frames) {
        max_frames = (uint32_t)remaining_frames;
    }

    // Generate audio by calling nextTick until the callback has enough samples.
    while (data->player->isPlaying() && data->pending_samples.size() / 2 < max_frames) {
        data->scope_tick_frames = 0;
        data->player->nextTick();
        if (data->scope_enabled && data->scope_tick_frames != 0) {
            data->scope_pos = (data->scope_pos + data->scope_tick_frames) & (EUP_SCOPE_WINDOW - 1);
        }
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
// Visualization.
//
// The scope is each emulator channel's own contribution, taken before the mix
// sums them, so it needs no analysis of the output. Only the channels the EUP
// header assigned an FM or PCM device to are offered.

static bool eupmini_plugin_get_structure(void* user_data, RVVizInfo* out) {
    auto* data = (EupminiReplayerData*)user_data;
    if (data == nullptr || out == nullptr || data->scope_count == 0) {
        return false;
    }

    out->caps = RVVizCaps_Scope | RVVizCaps_Vu;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = 0;
    out->scope_channel_count = data->scope_count;
    out->column_count = 0;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t eupmini_plugin_get_scope_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    auto* data = (EupminiReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    uint32_t count = data->scope_count < cap ? data->scope_count : cap;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "%s", data->scope_names[i]);
        out[i].scope_width = 1;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void eupmini_plugin_set_scope_enabled(void* user_data, bool on) {
    auto* data = (EupminiReplayerData*)user_data;
    if (data == nullptr) {
        return;
    }

    if (on && !data->scope_enabled) {
        std::fill(data->scope_ring.begin(), data->scope_ring.end(), 0.0f);
    }
    data->scope_enabled = on;

    if (data->device != nullptr) {
        data->device->scopeCallback(on && data->scope_count != 0 ? eupmini_collect_scope : nullptr, data);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t eupmini_plugin_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    auto* data = (EupminiReplayerData*)user_data;
    if (data == nullptr || out == nullptr || !data->scope_enabled) {
        return 0;
    }
    if (channel < 0 || (uint32_t)channel >= data->scope_count) {
        return 0;
    }

    uint32_t count = cap < EUP_SCOPE_WINDOW ? cap : EUP_SCOPE_WINDOW;
    const float* ring = &data->scope_ring[(size_t)channel * EUP_SCOPE_WINDOW];
    uint32_t start = (data->scope_pos + EUP_SCOPE_WINDOW - count) & (EUP_SCOPE_WINDOW - 1);
    for (uint32_t i = 0; i < count; i++) {
        out[i] = ring[(start + i) & (EUP_SCOPE_WINDOW - 1)];
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t eupmini_plugin_get_vu(void* user_data, float* out, uint32_t cap) {
    auto* data = (EupminiReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    // The host checks this against the declared channel count on every captured
    // frame, so the count is reported even with the capture switched off.
    uint32_t count = data->scope_count < cap ? data->scope_count : cap;
    for (uint32_t c = 0; c < count; c++) {
        if (!data->scope_enabled) {
            out[c] = 0.0f;
            continue;
        }
        const float* ring = &data->scope_ring[(size_t)c * EUP_SCOPE_WINDOW];
        uint32_t start = (data->scope_pos + EUP_SCOPE_WINDOW - EUP_VU_WINDOW) & (EUP_SCOPE_WINDOW - 1);
        float peak = 0.0f;
        for (uint32_t i = 0; i < EUP_VU_WINDOW; i++) {
            float v = ring[(start + i) & (EUP_SCOPE_WINDOW - 1)];
            if (v < 0.0f) {
                v = -v;
            }
            if (v > peak) {
                peak = v;
            }
        }
        out[c] = peak;
    }
    return count;
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

    // Visualization: per-channel scope and VU taken before the mix sums them.
    eupmini_plugin_get_structure,
    nullptr, // get_columns
    nullptr, // get_pattern_channels
    eupmini_plugin_get_scope_channels,
    nullptr, // get_position
    nullptr, // get_channel_rows
    nullptr, // get_cells
    eupmini_plugin_set_scope_enabled,
    eupmini_plugin_get_scope_samples,
    eupmini_plugin_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_eupmini_plugin;
}
