// minimal variant: stock unaccelerated A1200 (68EC020, 2 MB Chip, no FPU).
// Same CPU target as the standard variant but trimmed for a chip-RAM-only
// configuration with no Fast RAM expansion.

#define MICROPY_HW_MCU_NAME                 "68EC020"

// 128 KB heap — half the standard default. Stock A1200 has 2 MB Chip RAM
// total; AmigaOS itself consumes 300–500 KB, and any reasonably useful
// shell session will have a few apps resident too. Users with a memory
// expansion should switch to the standard or 68020fpu variant.
#define MICROPY_HEAP_SIZE                   (128 * 1024)

// No native code emitter: substantial code-size saving, and @native is
// less valuable on an unaccelerated 68020 anyway (no FPU, no caches).
#define MICROPY_EMIT_68K                    (0)

// No bsdsocket.library binding: networking is rare on stock A1200
// configurations (no Ethernet port, no PCMCIA card on the base machine),
// and the module pulls in a substantial amount of code.
#define MICROPY_PY_AMIGA_SOCKET             (0)
