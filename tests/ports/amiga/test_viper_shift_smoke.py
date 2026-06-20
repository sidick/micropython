# @micropython.viper 68k long-shift smoke test.
#
# Regression test for the asm68k shift-size bug: asm_68k_lsl/lsr/asr_dreg_dreg
# emitted the WORD-size shift opcodes (LSL.W/LSR.W; ASR was even encoded as
# ASL.W — wrong direction too) instead of the LONG forms. Any viper shift
# whose result depended on bits above 15 was wrong: 1 << 30 came out 0,
# signed >> shifted the wrong way, and byte-assembly like (a<<24)|(b<<16)|...
# lost its high bytes (the viper_subscr_multi failure). Fixed by emitting
# LSL.L (0xE1A8), LSR.L (0xE0A8) and ASR.L (0xE0A0).

import micropython


@micropython.viper
def shl(x: int, y: int) -> int:
    return x << y


# Left shifts past bit 15 — the WORD-size bug truncated these.
assert shl(1, 16) == 65536, shl(1, 16)
assert shl(1, 30) == 1073741824, shl(1, 30)
assert shl(42, 20) == 44040192, shl(42, 20)


@micropython.viper
def shr(x: int, y: int) -> int:
    return x >> y


# Arithmetic (signed) right shift — the bug emitted ASL.W (wrong size and
# wrong direction), so sign and magnitude were both wrong.
assert shr(1073741824, 30) == 1, shr(1073741824, 30)
assert shr(-1024, 4) == -64, shr(-1024, 4)
assert shr(-1, 1) == -1, shr(-1, 1)


@micropython.viper
def ushr(x: uint, y: uint) -> uint:
    return x >> y


# Logical (unsigned) right shift, full 32-bit width.
assert ushr(0x80000000, 31) == 1, ushr(0x80000000, 31)
assert ushr(0xFFFFFFFF, 16) == 0xFFFF, ushr(0xFFFFFFFF, 16)


@micropython.viper
def ushl(x: uint, y: uint) -> uint:
    return x << y


assert ushl(1, 31) == 0x80000000, ushl(1, 31)


# Byte assembly via shifts — the shape behind viper_subscr_multi.
@micropython.viper
def assemble(a: int, b: int, c: int, d: int) -> int:
    return (a << 24) | (b << 16) | (c << 8) | d


assert assemble(1, 2, 3, 4) == 0x01020304, assemble(1, 2, 3, 4)


print("OK")
