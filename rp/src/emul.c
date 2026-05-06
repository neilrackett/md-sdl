/**
 * File: emul.c
 * Description: MD/SDL microfirmware — SDL 1.2 video offload for Atari ST.
 *
 * The RP2040 acts as a C2P co-processor and framebuffer server.  The Atari ST
 * sends chunky pixel data and palette information over the cartridge bus; the
 * RP2040 performs median-cut palette reduction and chunky-to-planar conversion,
 * then writes the resulting ST low-res planar frame into one of two ST-visible
 * ROM4 slots where the ST copies it back into screen RAM.
 */

#include "emul.h"

#include <stdint.h>
#include <string.h>

/* Included in the .c to avoid multiple-definition errors */
#include "target_firmware.h"

#include "constants.h"
#include "debug.h"
#include "hardware/sync.h"
#include "memfunc.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
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
static uint32_t __attribute__((aligned(64)))
    sdl_md_plane_mask_lo[16][16];
static uint32_t __attribute__((aligned(64)))
    sdl_md_plane_mask_hi[16][16];
static uint8_t sdl_md_current_palette_rgb[256 * 3];
static uint32_t sdl_md_current_palette_seq = 0;
static uint32_t sdl_md_worker_palette_seq = 0;

typedef enum {
    SDL_MD_PLANAR_SLOT_FREE = 0,
    SDL_MD_PLANAR_SLOT_RENDERING = 1,
    SDL_MD_PLANAR_SLOT_READY = 2,
} SdlMdPlanarSlotState;

typedef struct {
    uint32_t seq;
    uint32_t palette_seq;
    uint32_t submit_time_us;
    uint8_t *chunky;
    uint8_t target_slot;
    bool full_frame;
    uint16_t x;
    uint16_t y;
    uint16_t rw;
    uint16_t rh;
    uint8_t palette_rgb[256 * 3];
} SdlMdJob;

/* =========================================================================
 * ROM_IN_RAM sub-addresses (computed from __rom_in_ram_start__ at init)
 * ========================================================================= */
static uint32_t mem_framebuffer_addr[SDL_MD_PLANAR_SLOTS];
static uint32_t mem_random_token_addr;
static uint32_t mem_random_token_seed_addr;
static uint32_t mem_mailbox_addr;
static uint32_t mem_palette_return_addr;
static uint8_t *sdl_md_chunky_buffers[2];
static uint8_t sdl_md_upload_chunky_index = 0;

static critical_section_t sdl_md_pipeline_lock;
static volatile bool sdl_md_worker_launched = false;
static volatile bool sdl_md_job_pending = false;
static volatile bool sdl_md_worker_busy = false;
static SdlMdJob sdl_md_pending_job;
static volatile uint8_t sdl_md_planar_slot_state[SDL_MD_PLANAR_SLOTS];
static uint32_t sdl_md_planar_slot_seq[SDL_MD_PLANAR_SLOTS];
static int8_t sdl_md_display_base_slot = -1;
static uint32_t sdl_md_display_base_seq = 0;

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

static void fill_default_palette_rgb(uint8_t *rgb768) {
    for (int i = 0; i < 256; i++) {
        uint16_t c = ega_palette[i & 15];
        rgb768[i * 3 + 0] = ste_chan_to_8bit((c >> 8) & 0xF);
        rgb768[i * 3 + 1] = ste_chan_to_8bit((c >> 4) & 0xF);
        rgb768[i * 3 + 2] = ste_chan_to_8bit(c & 0xF);
    }
}

static void init_c2p_mask_lut(void) {
    for (int px = 0; px < 16; px++) {
        uint32_t bit = 1u << (15 - px);
        for (int mapped = 0; mapped < 16; mapped++) {
            uint32_t lo = 0;
            uint32_t hi = 0;

            if (mapped & 0x1) lo |= bit;
            if (mapped & 0x2) lo |= bit << 16;
            if (mapped & 0x4) hi |= bit;
            if (mapped & 0x8) hi |= bit << 16;

            sdl_md_plane_mask_lo[px][mapped] = lo;
            sdl_md_plane_mask_hi[px][mapped] = hi;
        }
    }
}

static inline SdlMdMailbox *sdl_md_mailbox(void) {
    return (SdlMdMailbox *)mem_mailbox_addr;
}

static uint32_t sdl_md_next_token_seed(void) {
    static uint32_t token_seed = 0x4D44534Cu;

    token_seed ^= token_seed << 13;
    token_seed ^= token_seed >> 17;
    token_seed ^= token_seed << 5;
    if (token_seed == 0) {
        token_seed = 0x13579BDFu;
    }
    return token_seed;
}

static inline uint32_t sdl_md_now_us(void) {
    return (uint32_t)to_us_since_boot(get_absolute_time());
}

static inline uint16_t *sdl_md_planar_ptr(uint8_t slot) {
    return (uint16_t *)mem_framebuffer_addr[slot];
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

static inline __attribute__((always_inline)) void sdl_md_pack_block(
    const uint8_t *map0, const uint8_t *map1,
    const uint8_t *map2, const uint8_t *map3,
    const uint8_t *src, uint16_t *dst) {
    uint32_t lo = 0;
    uint32_t hi = 0;

    lo |= sdl_md_plane_mask_lo[0][map0[src[0]]];
    hi |= sdl_md_plane_mask_hi[0][map0[src[0]]];
    lo |= sdl_md_plane_mask_lo[1][map1[src[1]]];
    hi |= sdl_md_plane_mask_hi[1][map1[src[1]]];
    lo |= sdl_md_plane_mask_lo[2][map2[src[2]]];
    hi |= sdl_md_plane_mask_hi[2][map2[src[2]]];
    lo |= sdl_md_plane_mask_lo[3][map3[src[3]]];
    hi |= sdl_md_plane_mask_hi[3][map3[src[3]]];
    lo |= sdl_md_plane_mask_lo[4][map0[src[4]]];
    hi |= sdl_md_plane_mask_hi[4][map0[src[4]]];
    lo |= sdl_md_plane_mask_lo[5][map1[src[5]]];
    hi |= sdl_md_plane_mask_hi[5][map1[src[5]]];
    lo |= sdl_md_plane_mask_lo[6][map2[src[6]]];
    hi |= sdl_md_plane_mask_hi[6][map2[src[6]]];
    lo |= sdl_md_plane_mask_lo[7][map3[src[7]]];
    hi |= sdl_md_plane_mask_hi[7][map3[src[7]]];
    lo |= sdl_md_plane_mask_lo[8][map0[src[8]]];
    hi |= sdl_md_plane_mask_hi[8][map0[src[8]]];
    lo |= sdl_md_plane_mask_lo[9][map1[src[9]]];
    hi |= sdl_md_plane_mask_hi[9][map1[src[9]]];
    lo |= sdl_md_plane_mask_lo[10][map2[src[10]]];
    hi |= sdl_md_plane_mask_hi[10][map2[src[10]]];
    lo |= sdl_md_plane_mask_lo[11][map3[src[11]]];
    hi |= sdl_md_plane_mask_hi[11][map3[src[11]]];
    lo |= sdl_md_plane_mask_lo[12][map0[src[12]]];
    hi |= sdl_md_plane_mask_hi[12][map0[src[12]]];
    lo |= sdl_md_plane_mask_lo[13][map1[src[13]]];
    hi |= sdl_md_plane_mask_hi[13][map1[src[13]]];
    lo |= sdl_md_plane_mask_lo[14][map2[src[14]]];
    hi |= sdl_md_plane_mask_hi[14][map2[src[14]]];
    lo |= sdl_md_plane_mask_lo[15][map3[src[15]]];
    hi |= sdl_md_plane_mask_hi[15][map3[src[15]]];

    dst[0] = (uint16_t)lo;
    dst[1] = (uint16_t)(lo >> 16);
    dst[2] = (uint16_t)hi;
    dst[3] = (uint16_t)(hi >> 16);
}

/* =========================================================================
 * C2P: chunky (8bpp indices) → ST planar using Bayer-phase palette maps
 *
 * ST low-res planar layout: for each row, 4 interleaved bitplane words per
 * 16-pixel block.  Pixel p occupies bit (15 - p%16) of each plane word.
 * Plane 0 = LSB, plane 3 = MSB of the 4-bit palette index.
 * ========================================================================= */
static void __not_in_flash_func(sdl_c2p)(const uint8_t *chunky,
                                         uint16_t *planar,
                                         uint16_t w,
                                         uint16_t h) {
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
            const uint8_t *src = row + blk * 16;
            sdl_md_pack_block(map0, map1, map2, map3,
                              src, &prow[blk * 4]);
        }
    }
}

/* Partial C2P for UPDATE_RECT — clips to 16-pixel column boundaries */
static void sdl_c2p_rect(const uint8_t *chunky, uint16_t *planar,
                         uint16_t x, uint16_t y, uint16_t rw, uint16_t rh) {
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

    for (int row = y; row < y + rh; row++) {
        uint8_t row_phase = (uint8_t)((row & (SDL_MD_BAYER_DIM - 1u)) << 2);
        const uint8_t *map0 = sdl_md_bayer_pal_map[row_phase | 0u];
        const uint8_t *map1 = sdl_md_bayer_pal_map[row_phase | 1u];
        const uint8_t *map2 = sdl_md_bayer_pal_map[row_phase | 2u];
        const uint8_t *map3 = sdl_md_bayer_pal_map[row_phase | 3u];
        const uint8_t *src = chunky + row * sdl_md_width;
        uint16_t *prow = planar + row * blocks_per_row * 4;
        for (int blk = blk_start; blk < blk_end; blk++) {
            const uint8_t *blk_src = src + blk * 16;
            sdl_md_pack_block(map0, map1, map2, map3,
                              blk_src, &prow[blk * 4]);
        }
    }
}

static void sdl_md_reset_mailbox(void) {
    SdlMdMailbox *mailbox = sdl_md_mailbox();

    memset((void *)mailbox, 0, sizeof(*mailbox));
    mailbox->worker_busy = 0;
}

static void sdl_md_reset_pipeline_state(void) {
    critical_section_enter_blocking(&sdl_md_pipeline_lock);
    sdl_md_job_pending = false;
    sdl_md_worker_busy = false;
    sdl_md_upload_chunky_index = 0;
    sdl_md_display_base_slot = -1;
    sdl_md_display_base_seq = 0;
    for (int i = 0; i < SDL_MD_PLANAR_SLOTS; i++) {
        sdl_md_planar_slot_state[i] = SDL_MD_PLANAR_SLOT_FREE;
        sdl_md_planar_slot_seq[i] = 0;
    }
    critical_section_exit(&sdl_md_pipeline_lock);
    sdl_md_reset_mailbox();
}

static void sdl_md_clear_runtime_buffers(void) {
    memset(sdl_md_chunky_buffers[0], 0, SDL_MD_CHUNKY_SIZE);
    memset(sdl_md_chunky_buffers[1], 0, SDL_MD_CHUNKY_SIZE);
    for (int i = 0; i < SDL_MD_PLANAR_SLOTS; i++) {
        memset((void *)mem_framebuffer_addr[i], 0, SDL_MD_PLANAR_SIZE);
    }
}

static int sdl_md_find_free_planar_slot(void) {
    for (int i = 0; i < SDL_MD_PLANAR_SLOTS; i++) {
        if (sdl_md_planar_slot_state[i] == SDL_MD_PLANAR_SLOT_FREE) {
            return i;
        }
    }
    return -1;
}

static int sdl_md_find_partial_target_slot(void) {
    if ((sdl_md_display_base_slot >= 0) &&
        (sdl_md_display_base_slot < SDL_MD_PLANAR_SLOTS) &&
        (sdl_md_display_base_seq != 0) &&
        (sdl_md_planar_slot_state[(uint8_t)sdl_md_display_base_slot] ==
         SDL_MD_PLANAR_SLOT_FREE)) {
        return sdl_md_display_base_slot;
    }

    return -1;
}

static void sdl_md_publish_ready_frame(uint8_t slot, uint32_t seq,
                                       uint32_t palette_seq,
                                       uint32_t worker_start_us,
                                       uint32_t worker_end_us) {
    SdlMdMailbox *mailbox = sdl_md_mailbox();

    critical_section_enter_blocking(&sdl_md_pipeline_lock);
    sdl_md_planar_slot_state[slot] = SDL_MD_PLANAR_SLOT_READY;
    sdl_md_planar_slot_seq[slot] = seq;
    mailbox->ready_planar_slot = slot;
    mailbox->palette_seq = palette_seq;
    mailbox->worker_start_us = worker_start_us;
    mailbox->worker_end_us = worker_end_us;
    mailbox->worker_busy = 0;
    mailbox->ready_seq = seq;
    sdl_md_worker_busy = false;
    critical_section_exit(&sdl_md_pipeline_lock);
}

static void sdl_md_worker_loop(void) {
    while (true) {
        SdlMdJob job;
        bool have_job = false;

        critical_section_enter_blocking(&sdl_md_pipeline_lock);
        if (sdl_md_job_pending) {
            job = sdl_md_pending_job;
            sdl_md_job_pending = false;
            sdl_md_worker_busy = true;
            sdl_md_mailbox()->worker_busy = 1;
            have_job = true;
        }
        critical_section_exit(&sdl_md_pipeline_lock);

        if (!have_job) {
            tight_loop_contents();
            continue;
        }

        uint32_t worker_start_us = sdl_md_now_us();
        uint32_t palette_end_us = worker_start_us;
        if (job.palette_seq != sdl_md_worker_palette_seq) {
            uint8_t hw_count = median_cut(job.palette_rgb, sdl_md_hw_pal);
            build_bayer_pal_map(job.palette_rgb, hw_count);
            sdl_md_worker_palette_seq = job.palette_seq;
            palette_end_us = sdl_md_now_us();
            DPRINTF("SDL worker palette: seq=%lu time=%lu us\n",
                    (unsigned long)job.palette_seq,
                    (unsigned long)(palette_end_us - worker_start_us));
        }
        if (palette_end_us == worker_start_us) {
            palette_end_us = worker_start_us;
        }

        uint16_t *target_planar = sdl_md_planar_ptr(job.target_slot);
        if (job.full_frame) {
            sdl_c2p(job.chunky, target_planar, sdl_md_width, sdl_md_height);
        } else {
            sdl_c2p_rect(job.chunky, target_planar, job.x, job.y,
                         job.rw, job.rh);
        }
        uint32_t c2p_end_us = sdl_md_now_us();
        DPRINTF("SDL worker c2p: %s seq=%lu time=%lu us\n",
                job.full_frame ? "full" : "partial",
                (unsigned long)job.seq,
                (unsigned long)(c2p_end_us - palette_end_us));

        write_palette_return();
        sdl_md_publish_ready_frame(job.target_slot, job.seq, job.palette_seq,
                                   worker_start_us, c2p_end_us);
    }
}

static void sdl_md_launch_worker_if_needed(void) {
    if (!sdl_md_worker_launched) {
        sdl_md_worker_launched = true;
        multicore_launch_core1(sdl_md_worker_loop);
    }
}

static bool sdl_md_submit_job(bool full_frame,
                              uint32_t seq,
                              uint16_t x, uint16_t y,
                              uint16_t rw, uint16_t rh) {
    SdlMdMailbox *mailbox = sdl_md_mailbox();
    bool accepted = false;
    int next_upload_index = 0;

    critical_section_enter_blocking(&sdl_md_pipeline_lock);
    int target_slot;
    bool job_full_frame = full_frame;

    if (full_frame) {
        target_slot = sdl_md_find_free_planar_slot();
    } else {
        target_slot = sdl_md_find_partial_target_slot();
        if (target_slot < 0) {
            target_slot = sdl_md_find_free_planar_slot();
            job_full_frame = true;
        }
    }
    if (!sdl_md_job_pending && !sdl_md_worker_busy && (target_slot >= 0)) {
        sdl_md_pending_job.seq = seq;
        sdl_md_pending_job.palette_seq = sdl_md_current_palette_seq;
        sdl_md_pending_job.submit_time_us = sdl_md_now_us();
        sdl_md_pending_job.chunky = sdl_md_chunky_buffers[sdl_md_upload_chunky_index];
        sdl_md_pending_job.target_slot = (uint8_t)target_slot;
        sdl_md_pending_job.full_frame = job_full_frame;
        sdl_md_pending_job.x = x;
        sdl_md_pending_job.y = y;
        sdl_md_pending_job.rw = rw;
        sdl_md_pending_job.rh = rh;
        memcpy(sdl_md_pending_job.palette_rgb, sdl_md_current_palette_rgb,
               sizeof(sdl_md_pending_job.palette_rgb));
        sdl_md_planar_slot_state[target_slot] = SDL_MD_PLANAR_SLOT_RENDERING;
        mailbox->submit_time_us = sdl_md_pending_job.submit_time_us;
        mailbox->submit_seq = seq;
        next_upload_index = sdl_md_upload_chunky_index ^ 1u;
        sdl_md_upload_chunky_index = (uint8_t)next_upload_index;
        sdl_md_job_pending = true;
        accepted = true;
    } else {
        mailbox->dropped_frames++;
    }
    mailbox->worker_busy = (sdl_md_job_pending || sdl_md_worker_busy) ? 1u : 0u;
    critical_section_exit(&sdl_md_pipeline_lock);

    if (accepted) {
        memcpy(sdl_md_chunky_buffers[sdl_md_upload_chunky_index],
               sdl_md_pending_job.chunky, SDL_MD_CHUNKY_SIZE);
        if (!full_frame && job_full_frame) {
            DPRINTF("SDL submit: partial fallback to full seq=%lu\n",
                    (unsigned long)seq);
        }
    }

    return accepted;
}

static void sdl_md_release_frame(uint32_t seq) {
    critical_section_enter_blocking(&sdl_md_pipeline_lock);
    for (int i = 0; i < SDL_MD_PLANAR_SLOTS; i++) {
        if ((sdl_md_planar_slot_state[i] == SDL_MD_PLANAR_SLOT_READY) &&
            (sdl_md_planar_slot_seq[i] == seq)) {
            sdl_md_planar_slot_state[i] = SDL_MD_PLANAR_SLOT_FREE;
            sdl_md_display_base_slot = (int8_t)i;
            sdl_md_display_base_seq = seq;
            break;
        }
    }
    critical_section_exit(&sdl_md_pipeline_lock);
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
    sdl_md_clear_runtime_buffers();
    init_default_palette();
    fill_default_palette_rgb(sdl_md_current_palette_rgb);
    sdl_md_current_palette_seq = 0;
    sdl_md_worker_palette_seq = 0;
    sdl_md_reset_pipeline_state();
    write_palette_return();
    DPRINTF("SDL_MD_INIT: %ux%u bpp=%u\n", sdl_md_width, sdl_md_height, sdl_md_bpp);
}

static void cmd_quit(const TransmissionProtocol *proto) {
    (void)proto;
    sdl_md_clear_runtime_buffers();
    init_default_palette();
    fill_default_palette_rgb(sdl_md_current_palette_rgb);
    sdl_md_current_palette_seq = 0;
    sdl_md_worker_palette_seq = 0;
    sdl_md_reset_pipeline_state();
    write_palette_return();
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
    memcpy(sdl_md_current_palette_rgb, rgb, 768);
    sdl_md_current_palette_seq++;
    DPRINTF("SDL_MD_SET_PALETTE: seq=%lu\n",
            (unsigned long)sdl_md_current_palette_seq);
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
    uint8_t *chunky = sdl_md_chunky_buffers[sdl_md_upload_chunky_index];
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
        memcpy(&chunky[dst_offset], src + src_offset, copy_len);
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

    uint8_t *chunky = sdl_md_chunky_buffers[sdl_md_upload_chunky_index];

    if ((fw == 0) || (fh == 0) || (x >= sdl_md_width) || (y >= sdl_md_height)) {
        return;
    }
    if (fw > (sdl_md_width - x)) fw = (uint16_t)(sdl_md_width - x);
    if (fh > (sdl_md_height - y)) fh = (uint16_t)(sdl_md_height - y);

    for (int row = y; row < y + fh && row < sdl_md_height; row++) {
        uint32_t dst_offset = (uint32_t)row * sdl_md_width + x;
        memset(&chunky[dst_offset], color, fw);
    }
}

static void cmd_flip(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t seq = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);

    if (seq == 0) {
        seq = sdl_md_mailbox()->submit_seq + 1u;
    }
    if (!sdl_md_submit_job(true, seq, 0, 0, 0, 0)) {
        DPRINTF("SDL_MD_FLIP: busy/drop seq=%lu\n", (unsigned long)seq);
        return;
    }
    DPRINTF("SDL_MD_FLIP: queued seq=%lu\n", (unsigned long)seq);
}

static void cmd_update_rect(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t d3 = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);
    uint32_t d4 = TPROTO_GET_PAYLOAD_PARAM32(payload + 4);
    uint32_t d5 = TPROTO_GET_PAYLOAD_PARAM32(payload + 6);
    uint16_t x  = (uint16_t)(d3 >> 16);
    uint16_t y  = (uint16_t)(d3 & 0xFFFFu);
    uint16_t rw = (uint16_t)(d4 >> 16);
    uint16_t rh = (uint16_t)(d4 & 0xFFFFu);
    uint32_t seq = d5;

    if (seq == 0) {
        seq = sdl_md_mailbox()->submit_seq + 1u;
    }
    if (!sdl_md_submit_job(false, seq, x, y, rw, rh)) {
        DPRINTF("SDL_MD_UPDATE_RECT: busy/drop seq=%lu\n",
                (unsigned long)seq);
    }
}

static void cmd_ping(const TransmissionProtocol *proto) {
    (void)proto;
    /* Token is echoed by the normal path in sdl_dispatch */
}

static void cmd_release_frame(const TransmissionProtocol *proto) {
    const uint16_t *payload = (const uint16_t *)proto->payload;
    uint32_t seq = TPROTO_GET_PAYLOAD_PARAM32(payload + 2);

    if (seq != 0) {
        sdl_md_release_frame(seq);
    }
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
        case SDL_MD_RELEASE_FRAME: cmd_release_frame(proto); break;
        default:
            DPRINTF("SDL unknown command: %u\n", proto->command_id);
            break;
    }

    TPROTO_SET_RANDOM_TOKEN(mem_random_token_addr, token);
    TPROTO_SET_RANDOM_TOKEN(mem_random_token_seed_addr, sdl_md_next_token_seed());
}

/* =========================================================================
 * emul_start — firmware entry point called from main.c
 * ========================================================================= */
void emul_start(void) {
    /* Compute sub-addresses within the ST-visible ROM4 window */
    uint32_t rom_base = (uint32_t)&__rom_in_ram_start__;
    mem_framebuffer_addr[0] = rom_base + SDL_MD_FRAMEBUFFER0_OFFSET;
    mem_framebuffer_addr[1] = rom_base + SDL_MD_FRAMEBUFFER1_OFFSET;
    mem_random_token_addr  = rom_base + SDL_MD_RANDOM_TOKEN_OFFSET;
    mem_random_token_seed_addr = rom_base + SDL_MD_RANDOM_SEED_OFFSET;
    mem_mailbox_addr = rom_base + SDL_MD_MAILBOX_OFFSET;
    mem_palette_return_addr = rom_base + SDL_MD_PALETTE_RETURN_OFFSET;
    sdl_md_chunky_buffers[0] = sdl_md_chunky;
    sdl_md_chunky_buffers[1] = (uint8_t *)(rom_base + ROM_SIZE_BYTES);
    critical_section_init(&sdl_md_pipeline_lock);
    init_c2p_mask_lut();

    /* Initialise default EGA palette */
    init_default_palette();
    fill_default_palette_rgb(sdl_md_current_palette_rgb);
    sdl_md_current_palette_seq = 0;
    sdl_md_worker_palette_seq = 0;
    sdl_md_reset_pipeline_state();
    write_palette_return();
    TPROTO_SET_RANDOM_TOKEN(mem_random_token_seed_addr, sdl_md_next_token_seed());
    TPROTO_SET_RANDOM_TOKEN(mem_random_token_addr, 0);

    /* Copy the ST-side boot stub into ROM_IN_RAM */
    COPY_FIRMWARE_TO_RAM((uint16_t *)target_firmware, target_firmware_length);

    sdl_md_launch_worker_if_needed();

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
