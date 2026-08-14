#include "eupplayer_townsEmulator.hpp"

#include <cstddef>
#include <cstdint>

struct pcm_struct pcm;

struct CallbackState {
    size_t calls;
    size_t frames;
};

static void collect_pcm(void* user_data, int16_t const* samples, size_t frame_count) {
    auto* state = static_cast<CallbackState*>(user_data);
    if (samples != nullptr) {
        state->calls++;
        state->frames += frame_count;
    }
}

int main() {
    EUP_TownsEmulator device;
    CallbackState state = { 0, 0 };
    struct timeval step = { 0, 10000 };

    device.outputSampleUnsigned(false);
    device.outputSampleLSBFirst(true);
    device.outputSampleSize(2);
    device.outputSampleChannels(2);
    device.rate(44100);
    device.timeStep(step);
    device.outputCallback(collect_pcm, &state);
    device.nextTick();

    return state.calls == 1 && state.frames > 0 ? 0 : 1;
}
