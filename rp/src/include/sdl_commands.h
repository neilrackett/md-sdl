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
#define SDL_MD_FLIP          0x06u  /* C2P full surface → planar framebuffer */
#define SDL_MD_UPDATE_RECT   0x07u  /* Partial C2P: d3=(x<<16|y), d4=(w<<16|h) */
#define SDL_MD_PING          0x08u  /* Detection ping: d3=0x4D44534C ('MDSL') */

/* -------------------------------------------------------------------------
 * Offsets within ROM_IN_RAM (added to __rom_in_ram_start__ at runtime)
 * ST sees these as $FA0000 + offset
 * ------------------------------------------------------------------------- */
#define SDL_MD_FRAMEBUFFER_OFFSET    0x8000u  /* 32000 B planar output  → $FA8000 */
#define SDL_MD_RANDOM_TOKEN_OFFSET   0xF000u  /* 4 B random token       → $FAF000 */
#define SDL_MD_RANDOM_SEED_OFFSET    0xF004u  /* 4 B token seed         → $FAF004 */
#define SDL_MD_PALETTE_RETURN_OFFSET 0xF400u  /* 32 B hw palette return → $FAF400 */

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

/* -------------------------------------------------------------------------
 * Shared state (defined in emul.c)
 * ------------------------------------------------------------------------- */
extern uint8_t  sdl_md_chunky[SDL_MD_CHUNKY_SIZE];
extern uint8_t  sdl_md_pal_map[256];
extern uint16_t sdl_md_hw_pal[16];
extern uint16_t sdl_md_width;
extern uint16_t sdl_md_height;
extern uint8_t  sdl_md_bpp;

#endif /* SDL_COMMANDS_H */
