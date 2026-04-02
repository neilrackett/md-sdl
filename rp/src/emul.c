/**
 * File: emul.c
 * Description: MD/SDL microfirmware — SDL 1.2 video offload for Atari ST.
 *
 * The RP2040 acts as a C2P co-processor and framebuffer server.  The Atari ST
 * sends chunky pixel data and palette information over the cartridge bus; the
 * RP2040 performs median-cut palette reduction and chunky-to-planar conversion,
 * then writes the resulting ST low-res planar frame into the ROM4 window at
 * $FA8000 where the ST reads it back directly.
 */

#include "emul.h"

#include <stdint.h>
#include <string.h>

/* Included in the .c to avoid multiple-definition errors */
#include "target_firmware.h"

#include "constants.h"
#include "debug.h"
#include "display.h"
#include "hardware/sync.h"
#include "memfunc.h"
#include "pico/stdlib.h"
#include "reset.h"
#include "romemul.h"
#include "sdl_commands.h"
#include "tprotocol.h"

/* =========================================================================
 * Shared surface state (exported via sdl_commands.h)
 * ========================================================================= */
uint8_t  sdl_md_chunky[SDL_MD_CHUNKY_SIZE];
uint16_t sdl_md_hw_pal[16];
uint16_t sdl_md_width  = SDL_MD_MAX_WIDTH;
uint16_t sdl_md_height = SDL_MD_MAX_HEIGHT;
uint8_t  sdl_md_bpp    = 8;

#define SDL_MD_BAYER_DIM     4u
#define SDL_MD_BAYER_PHASES  (SDL_MD_BAYER_DIM * SDL_MD_BAYER_DIM)

static uint8_t sdl_md_bayer_pal_map[SDL_MD_BAYER_PHASES][256];

/* =========================================================================
 * ROM_IN_RAM sub-addresses (computed from __rom_in_ram_start__ at init)
 * ========================================================================= */
static uint32_t mem_framebuffer_addr;
static uint32_t mem_random_token_addr;
static uint32_t mem_palette_return_addr;

/* =========================================================================
 * Protocol double-buffer (same pattern as term.c)
 * ========================================================================= */
static TransmissionProtocol protocol_buffers[2];
static volatile uint8_t  protocol_read_index  = 0;
static volatile uint8_t  protocol_write_index = 1;
static volatile bool     protocol_buffer_ready = false;

/* =========================================================================
 * Default 16-colour EGA palette
 * STE format: 0x0RGB, 4 bits per channel (0-15)
 * ========================================================================= */
static const uint16_t ega_palette[16] = {
    0x0000,  /*  0 Black        */
    0x0007,  /*  1 Dark Blue    */
    0x0070,  /*  2 Dark Green   */
    0x0077,  /*  3 Dark Cyan    */
    0x0700,  /*  4 Dark Red     */
    0x0707,  /*  5 Dark Magenta */
    0x0770,  /*  6 Brown        */
    0x0777,  /*  7 Light Gray   */
    0x0333,  /*  8 Dark Gray    */
    0x000F,  /*  9 Blue         */
    0x00F0,  /* 10 Green        */
    0x00FF,  /* 11 Cyan         */
    0x0F00,  /* 12 Red          */
    0x0F0F,  /* 13 Magenta      */
    0x0FF0,  /* 14 Yellow       */
    0x0FFF,  /* 15 White        */
};

static const uint8_t bayer4x4[SDL_MD_BAYER_PHASES] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

/* Convert a 4-bit STE channel value (0-15) to 8-bit linear (0-255) */
static inline uint8_t ste_chan_to_8bit(uint8_t c4) {
    return (uint8_t)((c4 << 4) | c4);
}

static inline uint8_t clamp_u8(int value) {
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/* Squared Euclidean distance in RGB space */
static inline int rgb_dist2(int r1, int g1, int b1, int r2, int g2, int b2) {
    int dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return dr*dr + dg*dg + db*db;
}

static void unpack_hw_palette_rgb(uint8_t *hr, uint8_t *hg, uint8_t *hb,
                                  uint8_t hw_count) {
    for (int i = 0; i < hw_count; i++) {
        uint16_t c = sdl_md_hw_pal[i];
        hr[i] = ste_chan_to_8bit((c >> 8) & 0xF);
        hg[i] = ste_chan_to_8bit((c >> 4) & 0xF);
        hb[i] = ste_chan_to_8bit(c & 0xF);
    }
    for (int i = hw_count; i < 16; i++) {
        hr[i] = 0;
        hg[i] = 0;
        hb[i] = 0;
    }
}

static int nearest_hw_color_index(uint8_t r, uint8_t g, uint8_t b,
                                  const uint8_t *hr, const uint8_t *hg,
                                  const uint8_t *hb, uint8_t hw_count) {
    int best = 0;
    int best_d = 0x7FFFFFFF;
    for (int j = 0; j < hw_count; j++) {
        int d = rgb_dist2(r, g, b, hr[j], hg[j], hb[j]);
        if (d < best_d) {
            best_d = d;
            best = j;
        }
    }
    return best;
}

static void copy_uniform_bayer_pal_map(const uint8_t *base_map) {
    for (int phase = 0; phase < SDL_MD_BAYER_PHASES; phase++) {
        memcpy(sdl_md_bayer_pal_map[phase], base_map, 256);
    }
}

static void build_bayer_pal_map(const uint8_t *rgb768, uint8_t hw_count) {
    uint8_t hr[16], hg[16], hb[16];
    uint8_t base_map[256];

    unpack_hw_palette_rgb(hr, hg, hb, hw_count);
    for (int i = 0; i < 256; i++) {
        uint8_t r = rgb768[i*3 + 0];
        uint8_t g = rgb768[i*3 + 1];
        uint8_t b = rgb768[i*3 + 2];
        base_map[i] = (uint8_t)nearest_hw_color_index(
            r, g, b, hr, hg, hb, hw_count
        );
    }

    for (int phase = 0; phase < SDL_MD_BAYER_PHASES; phase++) {
        int threshold = ((int)bayer4x4[phase] * 2) - (SDL_MD_BAYER_PHASES - 1);
        for (int i = 0; i < 256; i++) {
            int base = base_map[i];
            int src_r = rgb768[i*3 + 0];
            int src_g = rgb768[i*3 + 1];
            int src_b = rgb768[i*3 + 2];
            int err_r = src_r - hr[base];
            int err_g = src_g - hg[base];
            int err_b = src_b - hb[base];
            uint8_t adj_r = clamp_u8(src_r + ((err_r * threshold) / 16));
            uint8_t adj_g = clamp_u8(src_g + ((err_g * threshold) / 16));
            uint8_t adj_b = clamp_u8(src_b + ((err_b * threshold) / 16));

            sdl_md_bayer_pal_map[phase][i] = (uint8_t)nearest_hw_color_index(
                adj_r, adj_g, adj_b, hr, hg, hb, hw_count
            );
        }
    }
}

static void init_default_palette(void) {
    uint8_t base_map[256];

    memcpy(sdl_md_hw_pal, ega_palette, sizeof(ega_palette));
    /* EGA palette: index i maps to hardware entry i mod 16 */
    for (int i = 0; i < 256; i++) {
        base_map[i] = (uint8_t)(i & 15);
    }
    copy_uniform_bayer_pal_map(base_map);
}

/* Write hw palette to the ST-visible palette return area */
static void write_palette_return(void) {
    volatile uint16_t *dest = (volatile uint16_t *)mem_palette_return_addr;
    for (int i = 0; i < 16; i++) {
        dest[i] = sdl_md_hw_pal[i];
    }
}

/* =========================================================================
 * Median-cut palette reduction
 * Input:  rgb768  — 256 × 3 bytes (R, G, B)
 * Output: hw_pal_out[16] in STE format
 * ========================================================================= */

typedef struct {
    uint8_t r_min, r_max;
    uint8_t g_min, g_max;
    uint8_t b_min, b_max;
    uint8_t indices[256];  /* logical color indices in this box */
    int     count;
} MedianBox;

static uint8_t median_cut(const uint8_t *rgb768, uint16_t *hw_pal_out) {
    static MedianBox boxes[16];
    int num_boxes = 1;

    /* Initialise the single box with all 256 colors */
    MedianBox *b0 = &boxes[0];
    b0->r_min = 255; b0->r_max = 0;
    b0->g_min = 255; b0->g_max = 0;
    b0->b_min = 255; b0->b_max = 0;
    b0->count = 0;

    for (int i = 0; i < 256; i++) {
        uint8_t r = rgb768[i*3 + 0];
        uint8_t g = rgb768[i*3 + 1];
        uint8_t b = rgb768[i*3 + 2];
        if (r < b0->r_min) b0->r_min = r;
        if (r > b0->r_max) b0->r_max = r;
        if (g < b0->g_min) b0->g_min = g;
        if (g > b0->g_max) b0->g_max = g;
        if (b < b0->b_min) b0->b_min = b;
        if (b > b0->b_max) b0->b_max = b;
        b0->indices[b0->count++] = (uint8_t)i;
    }

    while (num_boxes < 16) {
        int split_box = 0;
        int max_range = 0;
        for (int bi = 0; bi < num_boxes; bi++) {
            MedianBox *bx = &boxes[bi];
            int rr = bx->r_max - bx->r_min;
            int gr = bx->g_max - bx->g_min;
            int br = bx->b_max - bx->b_min;
            int range = (rr > gr) ? ((rr > br) ? rr : br)
                                  : ((gr > br) ? gr : br);
            if (range > max_range) { max_range = range; split_box = bi; }
        }

        if (max_range == 0) break;  /* all boxes are uniform */

        MedianBox *src = &boxes[split_box];
        int rr = src->r_max - src->r_min;
        int gr = src->g_max - src->g_min;
        int br = src->b_max - src->b_min;

        int axis = (rr >= gr && rr >= br) ? 0 : (gr >= br) ? 1 : 2;
        for (int i = 1; i < src->count; i++) {
            uint8_t key_idx = src->indices[i];
            uint8_t key_val = rgb768[key_idx*3 + axis];
            int j = i - 1;
            while (j >= 0 && rgb768[src->indices[j]*3 + axis] > key_val) {
                src->indices[j+1] = src->indices[j];
                j--;
            }
            src->indices[j+1] = key_idx;
        }

        int mid = src->count / 2;
        MedianBox *new_box = &boxes[num_boxes++];
        new_box->count = 0;
        new_box->r_min = 255; new_box->r_max = 0;
        new_box->g_min = 255; new_box->g_max = 0;
        new_box->b_min = 255; new_box->b_max = 0;
        for (int i = mid; i < src->count; i++) {
            uint8_t idx = src->indices[i];
            uint8_t r = rgb768[idx*3 + 0];
            uint8_t g = rgb768[idx*3 + 1];
            uint8_t b = rgb768[idx*3 + 2];
            if (r < new_box->r_min) new_box->r_min = r;
            if (r > new_box->r_max) new_box->r_max = r;
            if (g < new_box->g_min) new_box->g_min = g;
            if (g > new_box->g_max) new_box->g_max = g;
            if (b < new_box->b_min) new_box->b_min = b;
            if (b > new_box->b_max) new_box->b_max = b;
            new_box->indices[new_box->count++] = idx;
        }

        src->count = mid;
        src->r_min = 255; src->r_max = 0;
        src->g_min = 255; src->g_max = 0;
        src->b_min = 255; src->b_max = 0;
        for (int i = 0; i < src->count; i++) {
            uint8_t idx = src->indices[i];
            uint8_t r = rgb768[idx*3 + 0];
            uint8_t g = rgb768[idx*3 + 1];
            uint8_t b = rgb768[idx*3 + 2];
            if (r < src->r_min) src->r_min = r;
            if (r > src->r_max) src->r_max = r;
            if (g < src->g_min) src->g_min = g;
            if (g > src->g_max) src->g_max = g;
            if (b < src->b_min) src->b_min = b;
            if (b > src->b_max) src->b_max = b;
        }
    }

    /* Compute representative colour for each box (average) */
    uint8_t rep_r[16], rep_g[16], rep_b[16];
    for (int bi = 0; bi < num_boxes; bi++) {
        MedianBox *bx = &boxes[bi];
        uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
        for (int i = 0; i < bx->count; i++) {
            uint8_t idx = bx->indices[i];
            sum_r += rgb768[idx*3 + 0];
            sum_g += rgb768[idx*3 + 1];
            sum_b += rgb768[idx*3 + 2];
        }
        int cnt = bx->count > 0 ? bx->count : 1;
        rep_r[bi] = (uint8_t)(sum_r / cnt);
        rep_g[bi] = (uint8_t)(sum_g / cnt);
        rep_b[bi] = (uint8_t)(sum_b / cnt);
    }
    /* Pad unused boxes with black */
    for (int bi = num_boxes; bi < 16; bi++) {
        rep_r[bi] = rep_g[bi] = rep_b[bi] = 0;
    }

    /* Convert to STE format (4 bits per channel) and store */
    for (int i = 0; i < 16; i++) {
        uint8_t r4 = rep_r[i] >> 4;
        uint8_t g4 = rep_g[i] >> 4;
        uint8_t b4 = rep_b[i] >> 4;
        hw_pal_out[i] = (uint16_t)((r4 << 8) | (g4 << 4) | b4);
    }

    return (uint8_t)num_boxes;
}

/* =========================================================================
 * C2P: chunky (8bpp indices) → ST planar using Bayer-phase palette maps
 *
 * ST low-res planar layout: for each row, 4 interleaved bitplane words per
 * 16-pixel block.  Pixel p occupies bit (15 - p%16) of each plane word.
 * Plane 0 = LSB, plane 3 = MSB of the 4-bit palette index.
 * ========================================================================= */
static void sdl_c2p(const uint8_t *chunky, uint16_t *planar,
                    uint16_t w, uint16_t h) {
    int blocks_per_row = w / 16;
    for (int y = 0; y < h; y++) {
        uint8_t row_phase = (uint8_t)((y & (SDL_MD_BAYER_DIM - 1u)) << 2);
        const uint8_t *map0 = sdl_md_bayer_pal_map[row_phase | 0u];
        const uint8_t *map1 = sdl_md_bayer_pal_map[row_phase | 1u];
        const uint8_t *map2 = sdl_md_bayer_pal_map[row_phase | 2u];
        const uint8_t *map3 = sdl_md_bayer_pal_map[row_phase | 3u];
        const uint8_t *row = chunky + y * w;
        uint16_t *prow = planar + y * blocks_per_row * 4;
        for (int blk = 0; blk < blocks_per_row; blk++) {
            uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
            const uint8_t *src = row + blk * 16;
            for (int px = 0; px < 16; px += 4) {
                uint8_t mapped = map0[src[px + 0]];
                uint16_t bit = (uint16_t)(1u << (15 - (px + 0)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map1[src[px + 1]];
                bit = (uint16_t)(1u << (15 - (px + 1)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map2[src[px + 2]];
                bit = (uint16_t)(1u << (15 - (px + 2)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map3[src[px + 3]];
                bit = (uint16_t)(1u << (15 - (px + 3)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;
            }
            prow[blk*4 + 0] = p0;
            prow[blk*4 + 1] = p1;
            prow[blk*4 + 2] = p2;
            prow[blk*4 + 3] = p3;
        }
    }
}

/* Partial C2P for UPDATE_RECT — clips to 16-pixel column boundaries */
static void sdl_c2p_rect(uint16_t x, uint16_t y, uint16_t rw, uint16_t rh) {
    if ((rw == 0) || (rh == 0) || (x >= sdl_md_width) || (y >= sdl_md_height)) {
        return;
    }

    uint16_t x1 = x & ~15u;
    uint16_t x2 = (uint16_t)((((uint32_t)x + rw) + 15u) & ~15u);
    uint16_t max_x2 = (uint16_t)(sdl_md_width & ~15u);
    if (x2 > max_x2) x2 = max_x2;
    if (x1 >= x2) return;

    if (((uint32_t)y + rh) > sdl_md_height) {
        rh = (uint16_t)(sdl_md_height - y);
    }
    if (rh == 0) return;

    int blocks_per_row = sdl_md_width / 16;
    int blk_start = x1 / 16;
    int blk_end   = x2 / 16;
    uint16_t *planar = (uint16_t *)mem_framebuffer_addr;

    for (int row = y; row < y + rh; row++) {
        uint8_t row_phase = (uint8_t)((row & (SDL_MD_BAYER_DIM - 1u)) << 2);
        const uint8_t *map0 = sdl_md_bayer_pal_map[row_phase | 0u];
        const uint8_t *map1 = sdl_md_bayer_pal_map[row_phase | 1u];
        const uint8_t *map2 = sdl_md_bayer_pal_map[row_phase | 2u];
        const uint8_t *map3 = sdl_md_bayer_pal_map[row_phase | 3u];
        const uint8_t *src = sdl_md_chunky + row * sdl_md_width;
        uint16_t *prow = planar + row * blocks_per_row * 4;
        for (int blk = blk_start; blk < blk_end; blk++) {
            uint16_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
            const uint8_t *blk_src = src + blk * 16;
            for (int px = 0; px < 16; px += 4) {
                uint8_t mapped = map0[blk_src[px + 0]];
                uint16_t bit = (uint16_t)(1u << (15 - (px + 0)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map1[blk_src[px + 1]];
                bit = (uint16_t)(1u << (15 - (px + 1)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map2[blk_src[px + 2]];
                bit = (uint16_t)(1u << (15 - (px + 2)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;

                mapped = map3[blk_src[px + 3]];
                bit = (uint16_t)(1u << (15 - (px + 3)));
                if (mapped & 1) p0 |= bit;
                if (mapped & 2) p1 |= bit;
                if (mapped & 4) p2 |= bit;
                if (mapped & 8) p3 |= bit;
            }
            prow[blk*4 + 0] = p0;
            prow[blk*4 + 1] = p1;
            prow[blk*4 + 2] = p2;
            prow[blk*4 + 3] = p3;
        }
    }
}

/* =========================================================================
 * Command handlers
 * ========================================================================= */

static void cmd_init(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    /* Skip 2 words (random token low/high) */
    uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);
    uint32_t d4 = TPROTO_GET_PAYLOAD_PARAM32(payload + 4);
    uint16_t width = (uint16_t)(d3 >> 16);
    uint16_t height = (uint16_t)(d3 & 0xFFFFu);
    uint8_t bpp = (uint8_t)(d4 >> 16);

    if ((width == 0) || (width > SDL_MD_MAX_WIDTH)) width = SDL_MD_MAX_WIDTH;
    width = (uint16_t)(width & ~15u);
    if (width == 0) width = 16;

    if ((height == 0) || (height > SDL_MD_MAX_HEIGHT)) {
        height = SDL_MD_MAX_HEIGHT;
    }
    if (bpp != 8) bpp = 8;

    sdl_md_width  = width;
    sdl_md_height = height;
    sdl_md_bpp    = bpp;
    memset(sdl_md_chunky, 0, SDL_MD_CHUNKY_SIZE);
    init_default_palette();
    write_palette_return();
    DPRINTF("SDL_MD_INIT: %ux%u bpp=%u\n", sdl_md_width, sdl_md_height, sdl_md_bpp);
}

static void cmd_quit(const TransmissionProtocol *proto) {
    (void)proto;
    memset(sdl_md_chunky, 0, SDL_MD_CHUNKY_SIZE);
    memset((void *)mem_framebuffer_addr, 0, 32000);
    init_default_palette();
    DPRINTF("SDL_MD_QUIT\n");
}

static void cmd_set_palette(const TransmissionProtocol *proto) {
    /* Inline RGB buffer starts at byte offset 16 (after token+d3+d4+d5) */
    const uint8_t *rgb = proto->payload + 16;
    uint16_t available = proto->payload_size > 16
                         ? (uint16_t)(proto->payload_size - 16) : 0;
    if (available < 768) {
        DPRINTF("SDL_MD_SET_PALETTE: short payload (%u)\n", available);
        return;
    }
    uint8_t hw_count = median_cut(rgb, sdl_md_hw_pal);
    build_bayer_pal_map(rgb, hw_count);
    write_palette_return();
    DPRINTF("SDL_MD_SET_PALETTE: done\n");
}

static void cmd_blit_surface(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);
    uint32_t d4 = TPROTO_GET_PAYLOAD_PARAM32(payload + 4);
    uint32_t d5 = TPROTO_GET_PAYLOAD_PARAM32(payload + 6);

    uint16_t x        = (uint16_t)(d3 >> 16);
    uint16_t y        = (uint16_t)(d3 & 0xFFFFu);
    uint16_t bw       = (uint16_t)(d4 >> 16);
    uint16_t bh       = (uint16_t)(d4 & 0xFFFFu);
    uint16_t srcpitch = (uint16_t)(d5 >> 16);

    const uint8_t *src = proto->payload + 16;
    uint16_t available = proto->payload_size > 16
                         ? (uint16_t)(proto->payload_size - 16) : 0;
    if ((bw == 0) || (bh == 0) || (x >= sdl_md_width) || (y >= sdl_md_height)) {
        return;
    }

    if (bw > (sdl_md_width - x)) bw = (uint16_t)(sdl_md_width - x);
    if (bh > (sdl_md_height - y)) bh = (uint16_t)(sdl_md_height - y);
    if (srcpitch == 0) srcpitch = bw;
    if (srcpitch < bw) bw = srcpitch;
    if (bw == 0) return;

    for (int row = 0; row < bh; row++) {
        uint32_t src_offset = (uint32_t)row * srcpitch;
        if (src_offset >= available) break;

        uint16_t copy_len = bw;
        if (src_offset + copy_len > available) {
            copy_len = (uint16_t)(available - src_offset);
        }
        if (copy_len == 0) break;

        uint32_t dst_offset = (uint32_t)(y + row) * sdl_md_width + x;
        if (dst_offset + copy_len > SDL_MD_CHUNKY_SIZE) break;
        memcpy(&sdl_md_chunky[dst_offset], src + src_offset, copy_len);
    }
}

static void cmd_fill_rect(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);
    uint32_t d4 = TPROTO_GET_PAYLOAD_PARAM32(payload + 4);
    uint32_t d5 = TPROTO_GET_PAYLOAD_PARAM32(payload + 6);

    uint16_t x  = (uint16_t)(d3 >> 16);
    uint16_t y  = (uint16_t)(d3 & 0xFFFFu);
    uint16_t fw = (uint16_t)(d4 >> 16);
    uint16_t fh = (uint16_t)(d4 & 0xFFFFu);
    uint8_t  color = (uint8_t)(d5 >> 16);

    if ((fw == 0) || (fh == 0) || (x >= sdl_md_width) || (y >= sdl_md_height)) {
        return;
    }
    if (fw > (sdl_md_width - x)) fw = (uint16_t)(sdl_md_width - x);
    if (fh > (sdl_md_height - y)) fh = (uint16_t)(sdl_md_height - y);

    for (int row = y; row < y + fh && row < sdl_md_height; row++) {
        uint32_t dst_offset = (uint32_t)row * sdl_md_width + x;
        memset(&sdl_md_chunky[dst_offset], color, fw);
    }
}

static void cmd_flip(const TransmissionProtocol *proto) {
    (void)proto;
    sdl_c2p(sdl_md_chunky, (uint16_t *)mem_framebuffer_addr,
            sdl_md_width, sdl_md_height);
    DPRINTF("SDL_MD_FLIP\n");
}

static void cmd_update_rect(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);
    uint32_t d4 = TPROTO_GET_PAYLOAD_PARAM32(payload + 4);
    uint16_t x  = (uint16_t)(d3 >> 16);
    uint16_t y  = (uint16_t)(d3 & 0xFFFFu);
    uint16_t rw = (uint16_t)(d4 >> 16);
    uint16_t rh = (uint16_t)(d4 & 0xFFFFu);
    sdl_c2p_rect(x, y, rw, rh);
}

static void cmd_ping(const TransmissionProtocol *proto) {
    (void)proto;
    /* Token is echoed by the normal path in sdl_dispatch */
}

/* =========================================================================
 * Protocol DMA IRQ handler (runs in RAM, called from DMA IRQ1)
 * ========================================================================= */
static inline void __not_in_flash_func(handle_sdl_command)(
    const TransmissionProtocol *protocol) {
    uint8_t write_idx = protocol_write_index;
    TransmissionProtocol *wbuf = &protocol_buffers[write_idx];

    wbuf->command_id     = protocol->command_id;
    wbuf->payload_size   = protocol->payload_size;
    wbuf->bytes_read     = protocol->bytes_read;
    wbuf->final_checksum = protocol->final_checksum;

    uint16_t size = protocol->payload_size;
    if (size > MAX_PROTOCOL_PAYLOAD_SIZE) size = MAX_PROTOCOL_PAYLOAD_SIZE;
    memcpy(wbuf->payload, protocol->payload, size);

    uint8_t read_idx = protocol_read_index;
    protocol_read_index  = write_idx;
    protocol_write_index = read_idx;
    protocol_buffer_ready = true;
}

static inline void __not_in_flash_func(handle_sdl_checksum_error)(
    const TransmissionProtocol *protocol) {
    DPRINTF("SDL protocol error (ID=%u, Size=%u)\n",
            protocol->command_id, protocol->payload_size);
}

void __not_in_flash_func(sdl_md_dma_irq_handler_lookup)(void) {
    int lookup_ch = romemul_getLookupDataRomDmaChannel();
    if ((lookup_ch < 0) || (lookup_ch >= NUM_DMA_CHANNELS)) return;

    dma_hw->ints1 = 1u << (uint)lookup_ch;
    uint32_t addr = dma_hw->ch[(uint)lookup_ch].al3_read_addr_trig;

    if (__builtin_expect(addr & 0x00010000u, 0)) {
        uint16_t addr_lsb = (uint16_t)(addr ^ 0x8000u);
        tprotocol_parse(addr_lsb, handle_sdl_command, handle_sdl_checksum_error);
    }
}

/* =========================================================================
 * Command dispatcher
 * ========================================================================= */
static void sdl_dispatch(const TransmissionProtocol *proto) {
    /* Extract the random token from the payload (first 4 bytes) so we can
     * echo it back to the ST after the command completes. */
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t token = TPROTO_GET_RANDOM_TOKEN(payload);

    switch (proto->command_id) {
        case SDL_MD_INIT:         cmd_init(proto);         break;
        case SDL_MD_QUIT:         cmd_quit(proto);         break;
        case SDL_MD_SET_PALETTE:  cmd_set_palette(proto);  break;
        case SDL_MD_BLIT_SURFACE: cmd_blit_surface(proto); break;
        case SDL_MD_FILL_RECT:    cmd_fill_rect(proto);    break;
        case SDL_MD_FLIP:         cmd_flip(proto);         break;
        case SDL_MD_UPDATE_RECT:  cmd_update_rect(proto);  break;
        case SDL_MD_PING:         cmd_ping(proto);         break;
        default:
            DPRINTF("SDL unknown command: %u\n", proto->command_id);
            break;
    }

    TPROTO_SET_RANDOM_TOKEN(mem_random_token_addr, token);
}

/* =========================================================================
 * emul_start — firmware entry point called from main.c
 * ========================================================================= */
void emul_start(void) {
    /* Compute sub-addresses within the ST-visible ROM4 window */
    uint32_t rom_base = (uint32_t)&__rom_in_ram_start__;
    mem_framebuffer_addr   = rom_base + SDL_MD_FRAMEBUFFER_OFFSET;
    mem_random_token_addr  = rom_base + SDL_MD_RANDOM_TOKEN_OFFSET;
    mem_palette_return_addr = rom_base + SDL_MD_PALETTE_RETURN_OFFSET;

    /* Initialise default EGA palette */
    init_default_palette();
    write_palette_return();

    /* Copy the ST-side boot stub into ROM_IN_RAM */
    COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

    /* Render the boot message into the framebuffer at $FA8000.
     * display_setupU8g2() points u8g2 directly at ROM_IN_RAM + 0x8000, which
     * is the same region the ST's main.s copies to screen memory every vsync.
     * The message is therefore visible on the ST as soon as the bus is live. */
    display_setupU8g2();
    u8g2_t *u8g2 = display_getU8g2Ref();
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_squeezed_b7_tr);
    u8g2_DrawStr(u8g2, 4, 12, "MD/SDL: SDL coprocessor is ready");
    u8g2_SendBuffer(u8g2);

    /* Start the cartridge bus emulator with our DMA IRQ handler */
    if (init_romemul(NULL, sdl_md_dma_irq_handler_lookup, false) < 0) {
        DPRINTF("MD/SDL: ROM emulator init failed\n");
        while (true) {
            tight_loop_contents();
        }
    }

    DPRINTF("MD/SDL: ready, waiting for commands\n");

    /* Main command loop */
    while (true) {
        bool ready = false;
        TransmissionProtocol snapshot;

        uint32_t irq_state = save_and_disable_interrupts();
        if (protocol_buffer_ready) {
            snapshot = protocol_buffers[protocol_read_index];
            protocol_buffer_ready = false;
            ready = true;
        }
        restore_interrupts(irq_state);

        if (ready) {
            sdl_dispatch(&snapshot);
        }
    }
}
