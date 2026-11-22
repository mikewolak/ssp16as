/*
 * lrshot.c -- headless libretro front end (also writes <prefix>_<f>.ram: the 68000 work RAM): loads a libretro core (Genesis Plus GX), runs a cartridge for a number of
 * frames with no input and writes chosen frames as binary PPM files.
 *
 *   lrshot core.dylib rom.bin frames outprefix f1 [f2 ...]     writes <outprefix>_<f>.ppm for each listed frame
 *
 * With an SVP cartridge the core's global svp_t (core/cart_hw/svp/svp.h: iram_rom[0x20000], dram[0x20000], host-order
 * 16-bit words) is read through dlsym("svp"), and <outprefix>_<f>.dram receives the 128 KB DRAM as big-endian words.
 *
 * Pixel formats: RETRO_PIXEL_FORMAT_RGB565 and XRGB8888 (the core selects one through the environment call).
 * Core options: none are set; the core's defaults apply.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static unsigned pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static const void *last_fb; static unsigned last_w, last_h; static size_t last_pitch;
static uint8_t *copy; static size_t copy_sz;

static void log_cb(enum retro_log_level l, const char *fmt, ...)
{
    va_list ap; if (l < RETRO_LOG_WARN) return;
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixfmt = *(const unsigned *)data; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char **)data = "."; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback *)data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    default: return false;
    }
}

static void video_cb(const void *d, unsigned w, unsigned h, size_t pitch)
{
    if (!d) return;
    if (copy_sz < pitch * h) { copy_sz = pitch * h; copy = realloc(copy, copy_sz); }
    memcpy(copy, d, pitch * h);
    last_fb = copy; last_w = w; last_h = h; last_pitch = pitch;
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void poll_cb(void) {}
static int16_t input_cb(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

static void write_dram(void *h, const char *name)
{
    unsigned char **pp = (unsigned char **)dlsym(h, "svp"); FILE *f; unsigned i;
    if (!pp || !*pp) return;
    f = fopen(name, "wb"); if (!f) return;
    for (i = 0; i < 0x20000; i += 2) {
        uint16_t w; memcpy(&w, *pp + 0x20000 + i, 2);
        fputc(w >> 8, f); fputc(w & 0xff, f);
    }
    fclose(f);
}

static void *(*get_mem)(unsigned); static size_t (*get_mem_size)(unsigned);
static void write_ram(const char *name)                 /* 68000 work RAM as the core holds it (host order) */
{
    FILE *f; void *p = get_mem ? get_mem(RETRO_MEMORY_SYSTEM_RAM) : NULL; size_t n = get_mem_size ? get_mem_size(RETRO_MEMORY_SYSTEM_RAM) : 0;
    if (!p || !n) return;
    f = fopen(name, "wb"); if (!f) return;
    fwrite(p, 1, n, f); fclose(f);
}

static void write_ppm(const char *name)
{
    FILE *f = fopen(name, "wb"); unsigned x, y;
    if (!f || !last_fb) { fprintf(stderr, "no frame for %s\n", name); if (f) fclose(f); return; }
    fprintf(f, "P6\n%u %u\n255\n", last_w, last_h);
    for (y = 0; y < last_h; y++) {
        const uint8_t *row = (const uint8_t *)last_fb + y * last_pitch;
        for (x = 0; x < last_w; x++) {
            uint8_t rgb[3];
            if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p = ((const uint32_t *)row)[x];
                rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
            } else {
                uint16_t p = ((const uint16_t *)row)[x];
                if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) { rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 63) << 2; rgb[2] = (p & 31) << 3; }
                else { rgb[0] = ((p >> 10) & 31) << 3; rgb[1] = ((p >> 5) & 31) << 3; rgb[2] = (p & 31) << 3; }
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

#define SYM(n) do { *(void **)&n = dlsym(h, #n); if (!n) { fprintf(stderr, "missing %s\n", #n); return 1; } } while (0)
int main(int argc, char **argv)
{
    void *h; long frames, i; int k;
    void (*retro_set_environment)(retro_environment_t); void (*retro_set_video_refresh)(retro_video_refresh_t);
    void (*retro_set_audio_sample)(retro_audio_sample_t); void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*retro_set_input_poll)(retro_input_poll_t); void (*retro_set_input_state)(retro_input_state_t);
    void (*retro_init)(void); bool (*retro_load_game)(const struct retro_game_info *); void (*retro_run)(void);
    void (*retro_deinit)(void); void (*retro_unload_game)(void);
    struct retro_game_info gi; FILE *f; static uint8_t rom[0x800000];
    if (argc < 6) { fprintf(stderr, "usage: lrshot core.dylib rom.bin frames outprefix f1 [f2 ...]\n"); return 1; }
    h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state);
    SYM(retro_init); SYM(retro_load_game); SYM(retro_run); SYM(retro_deinit); SYM(retro_unload_game);
    retro_set_environment(env_cb); retro_set_video_refresh(video_cb); retro_set_audio_sample(audio_cb);
    retro_set_audio_sample_batch(audio_batch_cb); retro_set_input_poll(poll_cb); retro_set_input_state(input_cb);
    *(void **)&get_mem = dlsym(h, "retro_get_memory_data"); *(void **)&get_mem_size = dlsym(h, "retro_get_memory_size");
    retro_init();
    f = fopen(argv[2], "rb"); if (!f) { perror(argv[2]); return 1; }
    memset(&gi, 0, sizeof gi); gi.path = argv[2]; gi.size = fread(rom, 1, sizeof rom, f); gi.data = rom; fclose(f);
    if (!retro_load_game(&gi)) { fprintf(stderr, "load failed\n"); return 1; }
    frames = strtol(argv[3], 0, 0);
    for (i = 1; i <= frames; i++) {
        retro_run();
        for (k = 5; k < argc; k++) if (strtol(argv[k], 0, 0) == i) {
            char n[512]; snprintf(n, sizeof n, "%s_%04ld.ppm", argv[4], i); write_ppm(n);
            snprintf(n, sizeof n, "%s_%04ld.dram", argv[4], i); write_dram(h, n);
            snprintf(n, sizeof n, "%s_%04ld.ram", argv[4], i); write_ram(n);
        }
    }
    retro_unload_game(); retro_deinit();
    return 0;
}
