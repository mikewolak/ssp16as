/*
 * main.c -- 68000 side of the SVP demonstration cartridge.
 *
 * Commands frame k to the SSP through XST (0xA15000), waits for the SSP's answer (0xA15004 bit 0), and copies the
 * finished frame buffer from the SVP DRAM to VRAM by DMA while the SSP renders the next frame into the other buffer.
 *
 * Display: H32 (256 x 224), viewport 256 x 192 at rows 16..207.  Two VRAM pattern sets of 768 patterns (24 KB):
 * set 0 at 0x0000, set 1 at 0x6000.  Name tables (32 x 32): set 0 at 0xC000, set 1 at 0xE000; planes A and B both
 * point at the shown one.  Sprite table 0xC800, H scroll table 0xCC00, blank pattern 0x780 (VRAM 0xF000).
 *
 * The SSP's frame buffer is linear (svpdoc.txt "cell arrange 1"): read at 0x390000 + band * 0x2000 + b * 0x1000 it
 * gives the band's 128 patterns, column-major in 4-pattern columns: pattern = band * 128 + column * 4 + row-in-band.
 * DMA from the SVP is one word late (svpdoc.txt section 4): the source is the intended address + 2; the first word of
 * each transfer is written again by the CPU, read through the same window.
 *
 * Frame rate counter: 8 sprites "FPS dd.d" at screen x 188..251, rows 4..11 (above the viewport, outside the SSP
 * frame buffer), 5 x 7 font in 15 patterns at VRAM 0xD000 (pattern 0x680), colour 1 of palette line 1.  Vertical
 * blanks are counted by polling the VDP status (bit 3) in every wait; at each SSP answer the frame count rises, and
 * once at least one second of blanks (60 NTSC, 50 PAL: status bit 0) has passed, fps = rate * frames / blanks,
 * rounded to one decimal, and the sprites' patterns are rewritten.
 */
#include <stdint.h>
#include "palette.h"

#define VDP_DATA   (*(volatile uint16_t *)0xC00000)
#define VDP_CTRL   (*(volatile uint16_t *)0xC00004)
#define VDP_CTRL32 (*(volatile uint32_t *)0xC00004)
#define SVP_XST    (*(volatile uint16_t *)0xA15000)
#define SVP_STAT   (*(volatile uint16_t *)0xA15004)
#define SVP_CELL1  0x390000u

#define NT0 0xC000u
#define NT1 0xE000u
#define SET1_BASE 0x6000u
#define BLANK_PAT 0x780u
#define BANDS 6u
#define BAND_BYTES 0x1000u
#define FRAME_VBLANKS 4u                    /* 15 frames a second at 60 Hz */

static volatile uint32_t dma_cmd;           /* the DMA-starting command word is written from RAM */
static uint32_t vbl_total;                  /* vertical blanks seen */
static unsigned vb_prev;

#define FONT_BASE 0xD000u
#define FONT_PAT  (FONT_BASE >> 5)
#define SAT       0xC800u
#define FPS_X     188u
#define FPS_Y     4u
enum { G_DOT = 10, G_F, G_P, G_S, G_SP };
static const uint8_t font5x7[15][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0,0,0,0,0,0,0} };

static void vdp_reg(unsigned r, unsigned v) { VDP_CTRL = (uint16_t)(0x8000u | (r << 8) | (v & 0xFFu)); }

static void vram_addr(uint32_t a) { VDP_CTRL32 = 0x40000000u | ((a & 0x3FFFu) << 16) | (a >> 14); }
static void cram_addr(uint32_t a) { VDP_CTRL32 = 0xC0000000u | ((a & 0x3FFFu) << 16) | (a >> 14); }

static void wait_vblank(void)
{
    while (VDP_CTRL & 8u) ;
    while (!(VDP_CTRL & 8u)) ;
    vbl_total++; vb_prev = 1u;
}

static void poll_vb(void)
{
    unsigned st = VDP_CTRL & 8u;
    if (st && !vb_prev) vbl_total++;
    vb_prev = st;
}

static void font_init(void)
{
    unsigned g, r;
    vram_addr(FONT_BASE);
    for (g = 0; g < 15u; g++)
        for (r = 0; r < 8u; r++) {
            unsigned bits = r ? font5x7[g][r - 1u] : 0u, w0 = 0, w1 = 0, p;
            for (p = 1; p <= 5u; p++)                  /* pixel p shows font column p - 1; pixels 0, 6, 7 blank */
                if (bits & (0x10u >> (p - 1u))) {
                    if (p < 4u) w0 |= 1u << (12u - 4u * p); else w1 |= 1u << (12u - 4u * (p - 4u));
                }
            VDP_DATA = (uint16_t)w0;
            VDP_DATA = (uint16_t)w1;
        }
}

static void fps_text(unsigned fps10)                /* "FPS dd.d", right-aligned, fixed width */
{
    uint8_t g[8]; unsigned i;
    if (fps10 > 999u) fps10 = 999u;
    g[0] = G_F; g[1] = G_P; g[2] = G_S; g[3] = G_SP;
    g[4] = fps10 >= 100u ? (uint8_t)(fps10 / 100u) : G_SP;
    g[5] = (uint8_t)(fps10 / 10u % 10u); g[6] = G_DOT; g[7] = (uint8_t)(fps10 % 10u);
    for (i = 0; i < 8u; i++) {
        vram_addr(SAT + 8u * i);
        VDP_DATA = (uint16_t)(128u + FPS_Y);
        VDP_DATA = (uint16_t)(i < 7u ? i + 1u : 0u);   /* 1 x 1 cell, link */
        VDP_DATA = (uint16_t)(0x8000u | 0x2000u | (FONT_PAT + g[i]));
        VDP_DATA = (uint16_t)(128u + FPS_X + 8u * i);
    }
}

static void dma_svp_to_vram(uint32_t src, uint32_t dst, uint32_t bytes)
{
    uint32_t words = bytes >> 1, s = (src + 2u) >> 1;
    vdp_reg(0x13, words & 0xFFu);
    vdp_reg(0x14, (words >> 8) & 0xFFu);
    vdp_reg(0x15, s & 0xFFu);
    vdp_reg(0x16, (s >> 8) & 0xFFu);
    vdp_reg(0x17, (s >> 16) & 0x7Fu);
    dma_cmd = 0x40000080u | ((dst & 0x3FFFu) << 16) | (dst >> 14);
    VDP_CTRL32 = dma_cmd;
    vram_addr(dst);                         /* the first word again, by the CPU */
    VDP_DATA = *(volatile uint16_t *)src;
}

static void name_table(uint32_t nt, unsigned base)
{
    unsigned row, col;
    vram_addr(nt);
    for (row = 0; row < 32u; row++)
        for (col = 0; col < 32u; col++) {
            unsigned p = BLANK_PAT;
            if (row >= 2u && row < 26u) {
                unsigned r = row - 2u;
                p = base + (r >> 2) * 128u + col * 4u + (r & 3u);
            }
            VDP_DATA = (uint16_t)p;
        }
}

static void svp_command(uint16_t k) { SVP_XST = k; }
static void svp_wait(void) { while (!(SVP_STAT & 1u)) poll_vb(); }

int main(void)
{
    static const uint8_t regs[] = { 0x04, 0x54, NT0 >> 10, 0x00, NT0 >> 13, 0xC800u >> 9, 0x00, 0x00, 0x00, 0x00,
                                    0xFF, 0x00, 0x00, 0xCC00u >> 10, 0x00, 0x02, 0x00, 0x00, 0x00 };
    unsigned i, vb = 0, frames = 0, rate;
    uint32_t vbl_base;
    uint16_t k;
    (void)VDP_CTRL;
    for (i = 0; i < sizeof regs; i++) vdp_reg(i, regs[i]);
    vdp_reg(1, 0x14);                       /* display off while VRAM is set up */
    vram_addr(0);
    for (i = 0; i < 0x8000u; i++) VDP_DATA = 0;
    cram_addr(0);
    for (i = 0; i < 16u; i++) VDP_DATA = palette0[i];
    VDP_DATA = 0; VDP_DATA = 0x0EEE;        /* palette line 1: colour 1 white (FPS text) */
    font_init();
    fps_text(0);
    name_table(NT0, 0);
    name_table(NT1, SET1_BASE >> 5);
    VDP_CTRL32 = 0x40000010u; VDP_DATA = 0; VDP_DATA = 0;   /* VSRAM 0 */
    vdp_reg(1, 0x54);

    rate = (VDP_CTRL & 1u) ? 50u : 60u;
    k = 1;
    svp_command(k);
    svp_wait();
    vbl_base = vbl_total;
    for (;;) {
        unsigned b = k & 1u, band;
        uint32_t set = b ? SET1_BASE : 0u;
        svp_command((uint16_t)(k + 1u));     /* the SSP renders k + 1 into the other buffer */
        for (band = 0; band < BANDS; band++) {
            if (!(band & 1u)) { wait_vblank(); vb++; }
            dma_svp_to_vram(SVP_CELL1 + band * 0x2000u + b * BAND_BYTES, set + band * BAND_BYTES, BAND_BYTES);
        }
        while (vb < FRAME_VBLANKS) { wait_vblank(); vb++; }
        wait_vblank();
        vdp_reg(2, (b ? NT1 : NT0) >> 10);
        vdp_reg(4, (b ? NT1 : NT0) >> 13);
        vb = 1;
        svp_wait();
        frames++;
        if (vbl_total - vbl_base >= rate) {
            uint32_t el = vbl_total - vbl_base;
            fps_text((unsigned)((frames * rate * 10u + el / 2u) / el));
            frames = 0; vbl_base = vbl_total;
        }
        k++;
        if (k == 0) k = 1;
    }
}
