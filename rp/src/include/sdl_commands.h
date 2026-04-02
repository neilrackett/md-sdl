/**
 * File: sdl_commands.h
 * Description: SDL command IDs, address offsets, and shared state declarations
 *              for the MD/SDL microfirmware.
 */

#ifndef SDL_COMMANDS_H
#define SDL_COMMANDS_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Command IDs (ST → RP2040)
 * ------------------------------------------------------------------------- */
#define SDL_MD_INIT          0x01u  /* Init surface: d3=(w<<16|h), d4=(bpp<<16) */
#define SDL_MD_QUIT          0x02u  /* Release resources */
#define SDL_MD_SET_PALETTE   0x03u  /* Set palette: d3=(n_colors<<16), buf=RGB×256 */
#define SDL_MD_BLIT_SURFACE  0x04u  /* Blit chunk: d3=(x<<16|y), d4=(w<<16|h), d5=(pitch<<16), buf=pixels */
#define SDL_MD_FILL_RECT     0x05u  /* Fill rect: d3=(x<<16|y), d4=(w<<16|h), d5=(color8<<16) */
#define SDL_MD_FLIP          0x06u  /* Submit full-frame async C2P job: d3=seq */
#define SDL_MD_UPDATE_RECT   0x07u  /* Submit partial async C2P job: d3=(x<<16|y), d4=(w<<16|h), d5=seq */
#define SDL_MD_PING          0x08u  /* Detection ping: d3=0x4D44534C ('MDSL') */
#define SDL_MD_RELEASE_FRAME 0x09u  /* Release a presented planar slot: d3=seq */

/* -------------------------------------------------------------------------
 * Offsets within ROM_IN_RAM (added to __rom_in_ram_start__ at runtime)
 * ST sees these as $FA0000 + offset
 * ------------------------------------------------------------------------- */
#define SDL_MD_FRAMEBUFFER0_OFFSET   0x0000u  /* 32000 B planar slot 0  → $FA0000 */
#define SDL_MD_FRAMEBUFFER1_OFFSET   0x7D00u  /* 32000 B planar slot 1  → $FA7D00 */
#define SDL_MD_CONTROL_OFFSET        0xFA00u  /* Runtime control block   → $FAFA00 */
#define SDL_MD_RANDOM_TOKEN_OFFSET   0xFA00u  /* 4 B random token        → $FAFA00 */
#define SDL_MD_RANDOM_SEED_OFFSET    0xFA04u  /* 4 B token seed          → $FAFA04 */
#define SDL_MD_MAILBOX_OFFSET        0xFA20u  /* Async mailbox           → $FAFA20 */
#define SDL_MD_PALETTE_RETURN_OFFSET 0xFA80u  /* 32 B hw palette return  → $FAFA80 */
#define SDL_MD_SHARED_VARIABLES_OFFSET 0xFC00u /* Boot stub shared vars  → $FAFC00 */

/* -------------------------------------------------------------------------
 * Detection magic
 * ------------------------------------------------------------------------- */
#define SDL_MD_PING_MAGIC  0x4D44534CUL  /* 'MDSL' */

/* -------------------------------------------------------------------------
 * Surface geometry limits
 * ------------------------------------------------------------------------- */
#define SDL_MD_MAX_WIDTH   320u
#define SDL_MD_MAX_HEIGHT  200u
#define SDL_MD_CHUNKY_SIZE (SDL_MD_MAX_WIDTH * SDL_MD_MAX_HEIGHT)  /* 64000 B */
#define SDL_MD_PLANAR_SIZE 32000u
#define SDL_MD_PLANAR_SLOTS 2u

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t submit_seq;
    uint32_t ready_seq;
    uint32_t palette_seq;
    uint32_t worker_busy;
    uint32_t dropped_frames;
    uint32_t ready_planar_slot;
    uint32_t submit_time_us;
    uint32_t worker_start_us;
    uint32_t worker_end_us;
} SdlMdMailbox;

/* -------------------------------------------------------------------------
 * Shared state (defined in emul.c)
 * ------------------------------------------------------------------------- */
extern uint8_t  sdl_md_chunky[SDL_MD_CHUNKY_SIZE];
extern uint16_t sdl_md_hw_pal[16];
extern uint16_t sdl_md_width;
extern uint16_t sdl_md_height;
extern uint8_t  sdl_md_bpp;

#endif /* SDL_COMMANDS_H */
