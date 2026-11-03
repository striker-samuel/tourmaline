// SPDX-License-Identifier: MIT
#ifndef TOUR_RECOMP_ABI_H
#define TOUR_RECOMP_ABI_H
// Stable C boundary between Tourmaline's native host and independently
// built/relicensed static recompilation runtimes. No Nintendo or GPL code.
// Pointers are borrowed. All calls occur on the host's game thread in v1.
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TOUR_RECOMP_ABI_VERSION 1u

#define TOUR_RECOMP_CAP_AUDIO      (1u << 0)
#define TOUR_RECOMP_CAP_RUMBLE     (1u << 1)
#define TOUR_RECOMP_CAP_RGBA_FRAME (1u << 2)

typedef struct TourRecompControllerV1 {
    uint16_t buttons; // N64 button bits
    int8_t stick_x;   // canonical range -80..80
    int8_t stick_y;
} TourRecompControllerV1;

typedef struct TourRecompFrameV1 {
    const uint8_t* pixels; // borrowed until frame_rgba returns
    uint32_t width;
    uint32_t height;
    uint32_t pitch; // bytes/row, >= width * 4
} TourRecompFrameV1;

typedef struct TourRecompAudioV1 {
    const float* interleaved; // borrowed for the duration of the callback
    uint32_t frames;
    uint32_t channels;
    uint32_t sample_rate;
} TourRecompAudioV1;

typedef struct TourRecompHostV1 {
    uint32_t struct_bytes; // sizeof(TourRecompHostV1)
    void* user;
    void (*log)(void* user, uint32_t level, const char* message);
    void (*audio)(void* user, const TourRecompAudioV1* chunk);
    void (*rumble)(void* user, float low, float high, uint32_t duration_ms);
} TourRecompHostV1;

typedef struct TourRecompApiV1 {
    uint32_t abi_version; // TOUR_RECOMP_ABI_VERSION
    uint32_t struct_bytes; // sizeof(TourRecompApiV1)
    uint32_t capabilities;
    uint32_t reserved;
    void* user;
    // Game memory, ROM and EEPROM storage stay owned by the caller.
    // The adapter may only hold the borrowed pointers until shutdown.
    int (*boot)(void* user,
                const uint8_t* rom, size_t rom_bytes,
                uint8_t* rdram, size_t rdram_bytes,
                uint8_t* eeprom, size_t eeprom_bytes,
                const TourRecompHostV1* host);
    void (*step)(void* user, TourRecompControllerV1 controller);
    // Optional only if capability RGBA_FRAME is advertised.
    // This path is diagnostic. RT64's Vulkan image import is separate work.
    int (*frame_rgba)(void* user, TourRecompFrameV1* out);
    void (*shutdown)(void* user);
} TourRecompApiV1;

#ifdef __cplusplus
} // extern "C"
#endif
#endif // TOUR_RECOMP_ABI_H
