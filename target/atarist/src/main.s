; MD/SDL: Atari ST-side boot stub for SidecarTridge Multi-device
; (C) 2024-2026
; License: GPL v3
;
; This stub runs at TOS init time (after GEMDOS, before disk boot) to inform
; the RP2040 of the machine type and TOS version.  All SDL video operations
; are handled by SDL_xbios_md.c running in the SDL library, not here.
;
; Cartridge ROM header format:
;   $FA0000  CA_MAGIC  $abcdef42
;   $FA0004  CA_NEXT   0 (no further ROM programs)
;   $FA0008  CA_INIT   bit 27 set = run after GEMDOS init, before disk boot
;   $FA000C  CA_RUN    0 (not a standalone GEM app)
;   $FA0010  CA_TIME   GEMDOS timestamp
;   $FA0012  CA_DATE   GEMDOS date
;   $FA0014  CA_SIZE   code size
;   $FA0018  CA_NAME   filename (8.3, NUL-terminated)

ROM4_ADDR   equ $FA0000
SCREEN_SIZE equ (-4096)     ; scratch area just before screen memory

; Protocol addresses
RANDOM_TOKEN_ADDR       equ (ROM4_ADDR + $F000)
RANDOM_TOKEN_SEED_ADDR  equ (RANDOM_TOKEN_ADDR + 4)
SHARED_VARIABLES        equ (RANDOM_TOKEN_ADDR + $200)
ROMCMD_START_ADDR       equ $FB0000
CMD_MAGIC_NUMBER        equ $ABCD
CMD_RETRIES_COUNT       equ 3
CMD_SET_SHARED_VAR      equ 1

_dskbufp    equ $4c6

    include inc/sidecart_macros.s
    include inc/tos.s

    section

; ROM cartridge header
    org ROM4_ADDR

    dc.l $abcdef42                      ; CA_MAGIC
    dc.l 0                              ; CA_NEXT
    dc.l $08000000 + pre_auto           ; CA_INIT: bit 27 = after GEMDOS init
    dc.l 0                              ; CA_RUN
    dc.w GEMDOS_TIME                    ; CA_TIME
    dc.w GEMDOS_DATE                    ; CA_DATE
    dc.l end_pre_auto - pre_auto        ; CA_SIZE
    dc.b "MDSDL",0
    even

pre_auto:
; Relocate code to RAM to avoid running from cartridge ROM
    move.w #$2,-(sp)            ; Physbase XBIOS call
    trap #14
    addq.l #2,sp
    move.l d0, a2

    lea SCREEN_SIZE(a2), a2     ; scratch area before screen buffer
    move.l a2, a3               ; save relocation destination

    move.l #end_rom_code - start_rom_code, d6
    lea start_rom_code, a1
    lsr.w #2, d6
    subq #1, d6
.copy_rom_code:
    move.l (a1)+, (a2)+
    dbf d6, .copy_rom_code
    jmp (a3)                    ; jump to RAM copy

start_rom_code:
; Detect hardware type and send it to the RP2040 via CMD_SET_SHARED_VAR
    bsr detect_hw

; Get and send TOS version
    bsr get_tos_version

; Done — return to TOS to continue normal boot
    rts

; Shared library functions (detect_hw, get_tos_version, send_sync_command_to_sidecart, etc.)
    include "inc/sidecart_functions.s"

end_rom_code:
end_pre_auto:
    even
    dc.l 0
