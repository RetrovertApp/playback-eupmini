// SDL shim for eupmini -- stubs for SDL functions used in the ring buffer code.
// eupmini calls SDL_Delay(1) in a spin-wait loop when the ring buffer is full.
// With this stub, SDL_Delay is a no-op so nextTick() never blocks.
#ifndef SDL_SHIM_H
#define SDL_SHIM_H
#include <stdint.h>
static inline void SDL_Delay(uint32_t ms) {
    (void)ms;
}
static inline int SDL_Init(uint32_t flags) {
    (void)flags;
    return 0;
}
#endif
