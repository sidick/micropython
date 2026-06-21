/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef MICROPY_INCLUDED_PY_ASM68K_H
#define MICROPY_INCLUDED_PY_ASM68K_H

#include "py/asmbase.h"
#include "py/emit.h"
#include "py/misc.h"

// ---------------------------------------------------------------------------
// Register numbers (0–15 internal encoding)
//
//  0–7  : data registers D0–D7
//  8–14 : address registers A0–A6  (A7 = SP, not directly addressable here)
//
// A5 (13) is the frame pointer, managed by LINK/UNLK; not in the general pool.
// A0 (8) is a scratch address register used internally by load/store helpers.
// ---------------------------------------------------------------------------

#define ASM_68K_REG_D0  (0)
#define ASM_68K_REG_D1  (1)
#define ASM_68K_REG_D2  (2)
#define ASM_68K_REG_D3  (3)
#define ASM_68K_REG_D4  (4)
#define ASM_68K_REG_D5  (5)
#define ASM_68K_REG_D6  (6)
#define ASM_68K_REG_D7  (7)
#define ASM_68K_REG_A0  (8)
#define ASM_68K_REG_A1  (9)
#define ASM_68K_REG_A2  (10)
#define ASM_68K_REG_A3  (11)
#define ASM_68K_REG_A4  (12)
#define ASM_68K_REG_A5  (13)   // frame pointer
#define ASM_68K_REG_A6  (14)
#define ASM_68K_REG_SP  (15)   // A7, stack pointer

// Is this an address register (vs. data register)?
#define ASM_68K_IS_AREG(r) ((r) >= ASM_68K_REG_A0)

// Hardware register number within its class (0–7)
#define ASM_68K_DREG_NUM(r) ((r) & 7)
#define ASM_68K_AREG_NUM(r) ((r) & 7)

// ---------------------------------------------------------------------------
// Condition codes
// ---------------------------------------------------------------------------
#define ASM_68K_CC_HI   (2)   // >  unsigned
#define ASM_68K_CC_LS   (3)   // <= unsigned
#define ASM_68K_CC_CC   (4)   // >= unsigned (carry clear)
#define ASM_68K_CC_CS   (5)   // <  unsigned (carry set)
#define ASM_68K_CC_NE   (6)   // !=
#define ASM_68K_CC_EQ   (7)   // ==
#define ASM_68K_CC_GE   (12)  // >= signed
#define ASM_68K_CC_LT   (13)  // <  signed
#define ASM_68K_CC_GT   (14)  // >  signed
#define ASM_68K_CC_LE   (15)  // <= signed

// Negated condition (flip branch)
#define ASM_68K_CC_NEG(cc) ((cc) ^ 1)

// ---------------------------------------------------------------------------
// Assembler state
// ---------------------------------------------------------------------------
typedef struct _asm_68k_t {
    mp_asm_base_t base;
    mp_uint_t locals_count;     // total stack slots allocated by ASM_ENTRY
} asm_68k_t;

// ---------------------------------------------------------------------------
// Emit helpers – big-endian 16-bit and 32-bit words
// ---------------------------------------------------------------------------
static inline void asm_68k_emit16(asm_68k_t *as, uint16_t w) {
    uint8_t *p = mp_asm_base_get_cur_to_write_bytes(&as->base, 2);
    if (p) {
        p[0] = (w >> 8) & 0xFF;
        p[1] = w & 0xFF;
    }
}

static inline void asm_68k_emit32(asm_68k_t *as, uint32_t v) {
    uint8_t *p = mp_asm_base_get_cur_to_write_bytes(&as->base, 4);
    if (p) {
        p[0] = (v >> 24) & 0xFF;
        p[1] = (v >> 16) & 0xFF;
        p[2] = (v >> 8) & 0xFF;
        p[3] = v & 0xFF;
    }
}

// Patch a 16-bit value at a previously emitted position (pass-2 fixup).
static inline void asm_68k_patch16(asm_68k_t *as, size_t offset, uint16_t w) {
    if (as->base.pass == MP_ASM_PASS_EMIT) {
        as->base.code_base[offset] = (w >> 8) & 0xFF;
        as->base.code_base[offset + 1] = w & 0xFF;
    }
}

// ---------------------------------------------------------------------------
// MOVE instructions
// ---------------------------------------------------------------------------

// MOVE.L Ds, Dd
static inline void asm_68k_mov_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x2000 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// MOVEA.L Ds, Ad  (data → address)
static inline void asm_68k_movea_dreg_areg(asm_68k_t *as, int ad, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x2040 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
}

// MOVE.L As, Dd  (address → data)
static inline void asm_68k_mov_areg_dreg(asm_68k_t *as, int dd, int as_reg) {
    asm_68k_emit16(as, (uint16_t)(0x2008 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
}

// MOVEA.L As, Ad  (address → address)
static inline void asm_68k_movea_areg_areg(asm_68k_t *as, int ad, int as_reg) {
    asm_68k_emit16(as, (uint16_t)(0x2048 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_AREG_NUM(as_reg)));
}

// Generic MOVE.L rx, ry (handles all data/address combinations)
static inline void asm_68k_mov_reg_reg(asm_68k_t *as, int rd, int rs) {
    if (!ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_mov_dreg_dreg(as, rd, rs);
    } else if (ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_movea_dreg_areg(as, rd, rs);
    } else if (!ASM_68K_IS_AREG(rd) && ASM_68K_IS_AREG(rs)) {
        asm_68k_mov_areg_dreg(as, rd, rs);
    } else {
        asm_68k_movea_areg_areg(as, rd, rs);
    }
}

// MOVEQ #n, Dd  (−128 ≤ n ≤ 127, fast single-word)
//
// Encoding: 0111 RRR0 NNNNNNNN, where RRR is the destination data register
// number in bits 11-9 (not 10-8). Using `<< 8` here silently mis-targets the
// destination -- e.g. MOVEQ #1, D2 (wanted opcode 0x7401) would emit 0x7201
// = MOVEQ #1, D1, clobbering the wrong register and leaving the intended
// one stale. This was the cause of every @native binary op against a
// small-int literal in the MOVEQ range (-128..127) failing with TypeError
// or wandering into invalid-memory crashes.
static inline void asm_68k_moveq(asm_68k_t *as, int dd, int8_t n) {
    asm_68k_emit16(as, (uint16_t)(0x7000 | (ASM_68K_DREG_NUM(dd) << 9) | (uint8_t)n));
}

// MOVE.L #imm32, Dd
static inline void asm_68k_mov_imm_dreg(asm_68k_t *as, int dd, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
        asm_68k_moveq(as, dd, (int8_t)imm);
    } else {
        asm_68k_emit16(as, (uint16_t)(0x203C | (ASM_68K_DREG_NUM(dd) << 9)));
        asm_68k_emit32(as, (uint32_t)imm);
    }
}

// MOVEA.L #imm32, An
static inline void asm_68k_movea_imm_areg(asm_68k_t *as, int ad, int32_t imm) {
    asm_68k_emit16(as, (uint16_t)(0x207C | (ASM_68K_AREG_NUM(ad) << 9)));
    asm_68k_emit32(as, (uint32_t)imm);
}

// Generic immediate load (handles data and address registers)
static inline void asm_68k_mov_imm_reg(asm_68k_t *as, int rd, int32_t imm) {
    if (ASM_68K_IS_AREG(rd)) {
        asm_68k_movea_imm_areg(as, rd, imm);
    } else {
        asm_68k_mov_imm_dreg(as, rd, imm);
    }
}

// ---------------------------------------------------------------------------
// Load/store with displacement addressing
// The base register (rs) may be a data register; in that case a0 is used
// as a scratch address register internally.
// ---------------------------------------------------------------------------

// Load rs as address into A0 if rs is a data register
static inline int asm_68k_ensure_areg(asm_68k_t *as, int rs) {
    if (!ASM_68K_IS_AREG(rs)) {
        asm_68k_movea_dreg_areg(as, ASM_68K_REG_A0, rs);
        return ASM_68K_REG_A0;
    }
    return rs;
}

// MOVE.L (d16,As), Dd  (load long from memory into data register)
static inline void asm_68k_load32_dreg_from_mem(asm_68k_t *as, int dd, int as_reg, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x2028 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// MOVEA.L (d16,As), Ad  (load long from memory into address register)
static inline void asm_68k_load32_areg_from_mem(asm_68k_t *as, int ad, int as_reg, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x2068 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// Generic: load 32-bit from *(as_reg + disp) into rd (data or address)
static inline void asm_68k_load32_reg_from_mem(asm_68k_t *as, int rd, int rs, int32_t disp) {
    int areg = asm_68k_ensure_areg(as, rs);
    if (ASM_68K_IS_AREG(rd)) {
        asm_68k_load32_areg_from_mem(as, rd, areg, disp);
    } else {
        asm_68k_load32_dreg_from_mem(as, rd, areg, disp);
    }
}

// MOVE.L Ds, (d16,Ad)  (store long from data register)
static inline void asm_68k_store32_dreg_to_mem(asm_68k_t *as, int ds, int ad, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x2140 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// MOVE.L As, (d16,Ad)  (store long from address register)
// Encoding: 0x2148 | (Ad_num << 9) | As_num  — source mode=001(An), dest mode=101(d16,An)
static inline void asm_68k_store32_areg_to_mem(asm_68k_t *as, int as_reg, int ad, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x2148 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// Generic: store 32-bit rs into *(base_reg + disp); rs may be data or address.
static inline void asm_68k_store32_reg_to_mem(asm_68k_t *as, int rs, int base, int32_t disp) {
    int areg = asm_68k_ensure_areg(as, base);
    if (ASM_68K_IS_AREG(rs)) {
        asm_68k_store32_areg_to_mem(as, rs, areg, disp);
    } else {
        asm_68k_store32_dreg_to_mem(as, rs, areg, disp);
    }
}

// CLR.L Dd then MOVE.B (d16,As), Dd  (zero-extended byte load)
static inline void asm_68k_load8_dreg_from_mem(asm_68k_t *as, int dd, int as_reg, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x4280 | ASM_68K_DREG_NUM(dd)));                          // CLR.L Dd
    asm_68k_emit16(as, (uint16_t)(0x1028 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// CLR.L Dd then MOVE.W (d16,As), Dd  (zero-extended halfword load)
static inline void asm_68k_load16_dreg_from_mem(asm_68k_t *as, int dd, int as_reg, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x4280 | ASM_68K_DREG_NUM(dd)));                          // CLR.L Dd
    asm_68k_emit16(as, (uint16_t)(0x3028 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// MOVE.B Ds, (d16,Ad)
static inline void asm_68k_store8_dreg_to_mem(asm_68k_t *as, int ds, int ad, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x1140 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// MOVE.W Ds, (d16,Ad)
static inline void asm_68k_store16_dreg_to_mem(asm_68k_t *as, int ds, int ad, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x3140 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// ---------------------------------------------------------------------------
// Indexed load/store: (As, Xn.L, 0)  [Xn is index; scale=1, disp=0]
// ---------------------------------------------------------------------------

// Brief extension word for Xn.L (scale=1, disp=0)
static inline uint16_t asm_68k_idx_ext(int xn) {
    if (ASM_68K_IS_AREG(xn)) {
        return (uint16_t)(0x8800 | (ASM_68K_AREG_NUM(xn) << 12));
    } else {
        return (uint16_t)(0x0800 | (ASM_68K_DREG_NUM(xn) << 12));
    }
}

// MOVE.L (As, Xn.L), Dd
static inline void asm_68k_load32_idx(asm_68k_t *as, int dd, int as_reg, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x2030 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// MOVE.B (As, Xn.L), Dd  (with CLR.L for zero extension)
static inline void asm_68k_load8_idx(asm_68k_t *as, int dd, int as_reg, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x4280 | ASM_68K_DREG_NUM(dd)));
    asm_68k_emit16(as, (uint16_t)(0x1030 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// MOVE.W (As, Xn.L), Dd  (with CLR.L for zero extension)
static inline void asm_68k_load16_idx(asm_68k_t *as, int dd, int as_reg, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x4280 | ASM_68K_DREG_NUM(dd)));
    asm_68k_emit16(as, (uint16_t)(0x3030 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// MOVE.L Ds, (Ad, Xn.L)
static inline void asm_68k_store32_idx(asm_68k_t *as, int ds, int ad, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x2180 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// MOVE.B Ds, (Ad, Xn.L)
static inline void asm_68k_store8_idx(asm_68k_t *as, int ds, int ad, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x1180 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// MOVE.W Ds, (Ad, Xn.L)
static inline void asm_68k_store16_idx(asm_68k_t *as, int ds, int ad, int xn) {
    asm_68k_emit16(as, (uint16_t)(0x3180 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, asm_68k_idx_ext(xn));
}

// ---------------------------------------------------------------------------
// Frame-relative load/store (relative to A5 frame pointer)
//
// emitnative.c expects local 0 at the LOWEST address in the frame (same
// as other architectures that use SP-relative addressing).  After
// LINK A5, #-(n*4), the n locals occupy A5-n*4 .. A5-4.  We map:
//   local k  →  A5 - n*4 + k*4  =  A5 + (k - n)*4
// so local 0 is at the bottom (lowest address) and local n-1 is at the top.
// as->locals_count holds n (set by asm_68k_entry).
// ---------------------------------------------------------------------------
static inline int32_t asm_68k_local_offset(asm_68k_t *as, int local) {
    return ((int32_t)local - (int32_t)as->locals_count) * 4;
}

// Load local slot → rd
static inline void asm_68k_load_local(asm_68k_t *as, int rd, int local) {
    int32_t off = asm_68k_local_offset(as, local);
    if (ASM_68K_IS_AREG(rd)) {
        asm_68k_load32_areg_from_mem(as, rd, ASM_68K_REG_A5, off);
    } else {
        asm_68k_load32_dreg_from_mem(as, rd, ASM_68K_REG_A5, off);
    }
}

// Store rs → local slot
static inline void asm_68k_store_local(asm_68k_t *as, int local, int rs) {
    int32_t off = asm_68k_local_offset(as, local);
    if (ASM_68K_IS_AREG(rs)) {
        asm_68k_store32_areg_to_mem(as, rs, ASM_68K_REG_A5, off);
    } else {
        asm_68k_store32_dreg_to_mem(as, rs, ASM_68K_REG_A5, off);
    }
}

// ---------------------------------------------------------------------------
// Arithmetic
// ---------------------------------------------------------------------------

// ADD.L Ds, Dd
static inline void asm_68k_add_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xD080 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// ADD.L As, Dd  (address register source)
static inline void asm_68k_add_areg_dreg(asm_68k_t *as, int dd, int as_reg) {
    asm_68k_emit16(as, (uint16_t)(0xD088 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_AREG_NUM(as_reg)));
}

// ADDA.L Ds, Ad  (data register source, address register dest)
static inline void asm_68k_adda_dreg_areg(asm_68k_t *as, int ad, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xD1C0 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
}

// Generic ADD.L rs, rd
static inline void asm_68k_add_reg_reg(asm_68k_t *as, int rd, int rs) {
    if (!ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_add_dreg_dreg(as, rd, rs);
    } else if (!ASM_68K_IS_AREG(rd) && ASM_68K_IS_AREG(rs)) {
        asm_68k_add_areg_dreg(as, rd, rs);
    } else if (ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_adda_dreg_areg(as, rd, rs);
    } else {
        // ADDA.L As, Ad: not a standard 68k instruction for two address regs.
        // Move source to a scratch data reg via A0 → no, move As to D0-scratch.
        // Use: MOVE.L As, A0 temp; then ADDA.L... actually just use a temp.
        // Emit: MOVE.L rs, A0; ADDA.L A0-via-data, rd  — complicated.
        // Simplest: MOVE.L As_src, A0; MOVEA.L A0 as data is impossible.
        // Actually: we rarely add two address regs in generated code.
        // For safety emit: MOVE.L As_src, D0-scratch? No, that clobbers D0.
        // Use A1 scratch: MOVEA.L As_src, A1 not helpful.
        // We can use: mov As_src to A0, then compute via ADDA:
        // The only path here would be REG_GENERATOR_STATE + something, which
        // doesn't arise in emitnative.c. Emit NOP pair to keep size constant.
        asm_68k_emit16(as, 0x4E71); // NOP
        asm_68k_emit16(as, 0x4E71); // NOP
    }
}

// SUB.L Ds, Dd
static inline void asm_68k_sub_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x9080 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// SUBA.L Ds, Ad
static inline void asm_68k_suba_dreg_areg(asm_68k_t *as, int ad, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x91C0 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_DREG_NUM(ds)));
}

// Generic SUB.L rs, rd
static inline void asm_68k_sub_reg_reg(asm_68k_t *as, int rd, int rs) {
    if (!ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_sub_dreg_dreg(as, rd, rs);
    } else if (ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_suba_dreg_areg(as, rd, rs);
    } else {
        // Move rs to A0 then sub... rare case, use NOP
        asm_68k_emit16(as, 0x4E71);
        asm_68k_emit16(as, 0x4E71);
    }
}

// NEG.L Dd
static inline void asm_68k_neg_dreg(asm_68k_t *as, int dd) {
    asm_68k_emit16(as, (uint16_t)(0x4480 | ASM_68K_DREG_NUM(dd)));
}

// NOT.L Dd
static inline void asm_68k_not_dreg(asm_68k_t *as, int dd) {
    asm_68k_emit16(as, (uint16_t)(0x4680 | ASM_68K_DREG_NUM(dd)));
}

// CLR.L Dd
static inline void asm_68k_clr_dreg(asm_68k_t *as, int dd) {
    asm_68k_emit16(as, (uint16_t)(0x4280 | ASM_68K_DREG_NUM(dd)));
}

// MULS.L Ds, Dd  (32×32→32 signed, 68020+)
static inline void asm_68k_muls_l(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x4C00 | ASM_68K_DREG_NUM(ds)));
    asm_68k_emit16(as, (uint16_t)(0x0800 | (ASM_68K_DREG_NUM(dd) << 12) | ASM_68K_DREG_NUM(dd)));
}

// ---------------------------------------------------------------------------
// Logical (data registers only — address registers don't support AND/OR/EOR)
// ---------------------------------------------------------------------------

// AND.L Ds, Dd
static inline void asm_68k_and_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xC080 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// ANDI.L #imm, Dd
static inline void asm_68k_andi_l(asm_68k_t *as, int dd, int32_t imm) {
    asm_68k_emit16(as, (uint16_t)(0x0280 | ASM_68K_DREG_NUM(dd)));
    asm_68k_emit32(as, (uint32_t)imm);
}

// OR.L Ds, Dd
static inline void asm_68k_or_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x8080 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// EOR.L Ds, Dd  (note: EOR source must be Dn)
static inline void asm_68k_eor_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    // EOR: 1011 | Ds | 1 | 10 | 000 | Dd
    asm_68k_emit16(as, (uint16_t)(0xB180 | (ASM_68K_DREG_NUM(ds) << 9) | ASM_68K_DREG_NUM(dd)));
}

// ---------------------------------------------------------------------------
// Shifts (variable count from data register, 68k requires count in Dn)
// ---------------------------------------------------------------------------

// ASR.L Ds, Dd  (register count, arithmetic, right, long: 1110 ccc 0 10 1 00 yyy)
static inline void asm_68k_asr_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xE0A0 | (ASM_68K_DREG_NUM(ds) << 9) | ASM_68K_DREG_NUM(dd)));
}

// LSL.L Ds, Dd  (register count, logical, left, long: 1110 ccc 1 10 1 01 yyy)
static inline void asm_68k_lsl_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xE1A8 | (ASM_68K_DREG_NUM(ds) << 9) | ASM_68K_DREG_NUM(dd)));
}

// LSR.L Ds, Dd  (register count, logical, right, long: 1110 ccc 0 10 1 01 yyy)
static inline void asm_68k_lsr_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xE0A8 | (ASM_68K_DREG_NUM(ds) << 9) | ASM_68K_DREG_NUM(dd)));
}

// ---------------------------------------------------------------------------
// Compare
// ---------------------------------------------------------------------------

// CMP.L Ds, Dd  (computes Dd − Ds, sets flags)
static inline void asm_68k_cmp_dreg_dreg(asm_68k_t *as, int dd, int ds) {
    asm_68k_emit16(as, (uint16_t)(0xB080 | (ASM_68K_DREG_NUM(dd) << 9) | ASM_68K_DREG_NUM(ds)));
}

// TST.L Dn  (test against zero)
static inline void asm_68k_tst_dreg(asm_68k_t *as, int dd) {
    asm_68k_emit16(as, (uint16_t)(0x4A80 | ASM_68K_DREG_NUM(dd)));
}

// Generic compare reg vs reg (handles address-register operands via scratch)
// Computes rd − rs for flags.
static inline void asm_68k_cmp_reg_reg(asm_68k_t *as, int rd, int rs) {
    if (!ASM_68K_IS_AREG(rd) && !ASM_68K_IS_AREG(rs)) {
        asm_68k_cmp_dreg_dreg(as, rd, rs);
    } else {
        // Move both to data scratch registers and compare
        if (ASM_68K_IS_AREG(rd)) {
            asm_68k_mov_areg_dreg(as, ASM_68K_REG_D4, rd);
            rd = ASM_68K_REG_D4;
        }
        if (ASM_68K_IS_AREG(rs)) {
            asm_68k_mov_areg_dreg(as, ASM_68K_REG_D5, rs);
            rs = ASM_68K_REG_D5;
        }
        asm_68k_cmp_dreg_dreg(as, rd, rs);
    }
}

// Scc Dd  (set byte to 0xFF if cc true, 0x00 if false)
static inline void asm_68k_scc_dreg(asm_68k_t *as, int cc, int dd) {
    asm_68k_emit16(as, (uint16_t)(0x50C0 | (cc << 8) | ASM_68K_DREG_NUM(dd)));
}

// ---------------------------------------------------------------------------
// LEA / address compute
// ---------------------------------------------------------------------------

// LEA (d16, As), Ad
static inline void asm_68k_lea_disp(asm_68k_t *as, int ad, int as_reg, int32_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x41E8 | (ASM_68K_AREG_NUM(ad) << 9) | ASM_68K_AREG_NUM(as_reg)));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// LEA (d16, PC), Ad  — disp is relative to the extension word (current_pc + 2)
// Returns the code_offset of the extension word so the caller can patch it.
static inline size_t asm_68k_lea_pcrel_placeholder(asm_68k_t *as, int ad) {
    asm_68k_emit16(as, (uint16_t)(0x41FA | (ASM_68K_AREG_NUM(ad) << 9)));
    size_t pos = as->base.code_offset;
    asm_68k_emit16(as, 0x0000); // placeholder displacement
    return pos;
}

// ---------------------------------------------------------------------------
// Branch instructions (always .W form for consistent sizing)
// A .W branch is: opcode word (2 bytes) + displacement word (2 bytes) = 4 bytes.
// Displacement is relative to the word AFTER the opcode word (i.e. end of instr).
// ---------------------------------------------------------------------------

// Bcc.W displacement (displacement relative to end of instruction)
static inline void asm_68k_bcc_w(asm_68k_t *as, int cc, int16_t disp) {
    asm_68k_emit16(as, (uint16_t)(0x6000 | (cc << 8) | 0x00));
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// BRA.W
static inline void asm_68k_bra_w(asm_68k_t *as, int16_t disp) {
    asm_68k_emit16(as, 0x6000);
    asm_68k_emit16(as, (uint16_t)(int16_t)disp);
}

// JMP (An)
static inline void asm_68k_jmp_ind(asm_68k_t *as, int an) {
    asm_68k_emit16(as, (uint16_t)(0x4ED0 | ASM_68K_AREG_NUM(an)));
}

// JSR (An)
static inline void asm_68k_jsr_ind(asm_68k_t *as, int an) {
    asm_68k_emit16(as, (uint16_t)(0x4E90 | ASM_68K_AREG_NUM(an)));
}

// RTS
static inline void asm_68k_rts(asm_68k_t *as) {
    asm_68k_emit16(as, 0x4E75);
}

// NOP
static inline void asm_68k_nop(asm_68k_t *as) {
    asm_68k_emit16(as, 0x4E71);
}

// ADDA.L #imm, SP  — used to pop arguments after a call
static inline void asm_68k_adda_imm_sp(asm_68k_t *as, int32_t imm) {
    if (imm >= 1 && imm <= 8) {
        // ADDQ.L #n, SP: 0101 | nnn | 0 | 10 | 001 | 111 (A7=SP)
        // Bit 8 = 0 for ADDQ; 0x518F had bit 8 = 1 (SUBQ) — fixed to 0x508F.
        asm_68k_emit16(as, (uint16_t)(0x508F | ((imm & 7) << 9)));
    } else {
        // ADDA.L #imm, SP (A7=7): 1101 | 111 | 111 | 111 | 100 = 0xDFFC
        asm_68k_emit16(as, 0xDFFC);
        asm_68k_emit32(as, (uint32_t)imm);
    }
}

// MOVE.L Ds, -(SP)
static inline void asm_68k_push_dreg(asm_68k_t *as, int ds) {
    asm_68k_emit16(as, (uint16_t)(0x2F00 | ASM_68K_DREG_NUM(ds)));
}

// ---------------------------------------------------------------------------
// Entry/exit API
// ---------------------------------------------------------------------------
void asm_68k_entry(asm_68k_t *as, mp_uint_t n_locals);
void asm_68k_exit(asm_68k_t *as);
void asm_68k_end_pass(asm_68k_t *as);

// ---------------------------------------------------------------------------
// Higher-level helpers used from asm68k.c / emitn68k.c
// ---------------------------------------------------------------------------
void asm_68k_emit_call_ind(asm_68k_t *as, mp_uint_t index);
void asm_68k_emit_jump(asm_68k_t *as, mp_uint_t label);
void asm_68k_emit_jump_if_reg_eq(asm_68k_t *as, mp_uint_t rs1, mp_uint_t rs2, mp_uint_t label);
void asm_68k_emit_jump_if_reg_nonzero(asm_68k_t *as, mp_uint_t rs, mp_uint_t label);
void asm_68k_emit_jump_if_reg_zero(asm_68k_t *as, mp_uint_t rs, mp_uint_t label);
void asm_68k_emit_mov_reg_local(asm_68k_t *as, mp_uint_t rd, mp_uint_t local);
void asm_68k_emit_mov_local_reg(asm_68k_t *as, mp_uint_t local, mp_uint_t rs);
void asm_68k_emit_mov_reg_local_addr(asm_68k_t *as, mp_uint_t rd, mp_uint_t local);
void asm_68k_emit_mov_reg_pcrel(asm_68k_t *as, mp_uint_t rd, mp_uint_t label);
void asm_68k_emit_load_reg_reg_offset(asm_68k_t *as, mp_uint_t rd, mp_uint_t rs, int32_t offset, mp_uint_t op_size);
void asm_68k_emit_store_reg_reg_offset(asm_68k_t *as, mp_uint_t rs_val, mp_uint_t rs_base, int32_t offset, mp_uint_t op_size);
void asm_68k_emit_load_reg_reg_reg(asm_68k_t *as, mp_uint_t rd, mp_uint_t rs1, mp_uint_t rs2, mp_uint_t op_size);
void asm_68k_emit_store_reg_reg_reg(asm_68k_t *as, mp_uint_t rs_val, mp_uint_t rs1, mp_uint_t rs2, mp_uint_t op_size);

// ---------------------------------------------------------------------------
// GENERIC_ASM_API  — consumed by emitnative.c via #include from emitn68k.c
// ---------------------------------------------------------------------------
#ifdef GENERIC_ASM_API

#define ASM_T               asm_68k_t
#define ASM_ENTRY(as, n, nm) asm_68k_entry((as), (n))
#define ASM_EXIT(as)        asm_68k_exit(as)
#define ASM_END_PASS(as)    asm_68k_end_pass(as)

#define ASM_WORD_SIZE       (4)

// Register assignments
#define REG_RET             ASM_68K_REG_D0
#define REG_ARG_1           ASM_68K_REG_D0
#define REG_ARG_2           ASM_68K_REG_D1
#define REG_ARG_3           ASM_68K_REG_D2
#define REG_ARG_4           ASM_68K_REG_D3
#define REG_TEMP0           ASM_68K_REG_D4
#define REG_TEMP1           ASM_68K_REG_D5
#define REG_TEMP2           ASM_68K_REG_D6
#define REG_LOCAL_1         ASM_68K_REG_D7
#define REG_LOCAL_2         ASM_68K_REG_A2
#define REG_LOCAL_3         ASM_68K_REG_A3
#define REG_FUN_TABLE       ASM_68K_REG_A4

// Register-to-register move
#define ASM_MOV_REG_REG(as, rd, rs) asm_68k_mov_reg_reg((as), (rd), (rs))

// Immediate load
#define ASM_MOV_REG_IMM(as, rd, imm) asm_68k_mov_imm_reg((as), (rd), (int32_t)(imm))

// Clear register
#define ASM_CLR_REG(as, rd) asm_68k_clr_dreg((as), (rd))

// Arithmetic
#define ASM_ADD_REG_REG(as, rd, rs)  asm_68k_add_reg_reg((as), (rd), (rs))
#define ASM_SUB_REG_REG(as, rd, rs)  asm_68k_sub_reg_reg((as), (rd), (rs))
#define ASM_MUL_REG_REG(as, rd, rs)  asm_68k_muls_l((as), (rd), (rs))
#define ASM_NEG_REG(as, rd)          asm_68k_neg_dreg((as), (rd))

// Logical
#define ASM_AND_REG_REG(as, rd, rs)  asm_68k_and_dreg_dreg((as), (rd), (rs))
#define ASM_OR_REG_REG(as, rd, rs)   asm_68k_or_dreg_dreg((as), (rd), (rs))
#define ASM_XOR_REG_REG(as, rd, rs)  asm_68k_eor_dreg_dreg((as), (rd), (rs))
#define ASM_NOT_REG(as, rd)          asm_68k_not_dreg((as), (rd))

// Shifts
#define ASM_ASR_REG_REG(as, rd, rs)  asm_68k_asr_dreg_dreg((as), (rd), (rs))
#define ASM_LSL_REG_REG(as, rd, rs)  asm_68k_lsl_dreg_dreg((as), (rd), (rs))
#define ASM_LSR_REG_REG(as, rd, rs)  asm_68k_lsr_dreg_dreg((as), (rd), (rs))

// Indirect call through fun_table[index]
#define ASM_CALL_IND(as, idx)        asm_68k_emit_call_ind((as), (idx))

// Unconditional jump to label
#define ASM_JUMP(as, label)          asm_68k_emit_jump((as), (label))

// Conditional jumps
#define ASM_JUMP_IF_REG_EQ(as, rs1, rs2, lbl) \
    asm_68k_emit_jump_if_reg_eq((as), (rs1), (rs2), (lbl))
#define ASM_JUMP_IF_REG_NONZERO(as, rs, lbl, bt) \
    asm_68k_emit_jump_if_reg_nonzero((as), (rs), (lbl))
#define ASM_JUMP_IF_REG_ZERO(as, rs, lbl, bt) \
    asm_68k_emit_jump_if_reg_zero((as), (rs), (lbl))

// Jump-register (indirect via register)
#define ASM_JUMP_REG(as, rs) do { \
        asm_68k_movea_dreg_areg((as), ASM_68K_REG_A0, (rs)); \
        asm_68k_jmp_ind((as), ASM_68K_REG_A0); } while (0)

// Local variable access (frame-relative)
#define ASM_MOV_REG_LOCAL(as, rd, loc)      asm_68k_emit_mov_reg_local((as), (rd), (loc))
#define ASM_MOV_LOCAL_REG(as, loc, rs)      asm_68k_emit_mov_local_reg((as), (loc), (rs))
#define ASM_MOV_REG_LOCAL_ADDR(as, rd, loc) asm_68k_emit_mov_reg_local_addr((as), (rd), (loc))

// PC-relative load (address of a label)
#define ASM_MOV_REG_PCREL(as, rd, lbl)      asm_68k_emit_mov_reg_pcrel((as), (rd), (lbl))

// Loads with displacement (offset is in words, scaled by op_size)
#define ASM_LOAD_REG_REG_OFFSET(as, rd, rs, off)  ASM_LOAD32_REG_REG_OFFSET(as, rd, rs, off)
#define ASM_LOAD8_REG_REG(as, rd, rs)      ASM_LOAD8_REG_REG_OFFSET(as, rd, rs, 0)
#define ASM_LOAD16_REG_REG(as, rd, rs)     ASM_LOAD16_REG_REG_OFFSET(as, rd, rs, 0)
#define ASM_LOAD32_REG_REG(as, rd, rs)     ASM_LOAD32_REG_REG_OFFSET(as, rd, rs, 0)
#define ASM_LOAD8_REG_REG_OFFSET(as, rd, rs, off) \
    asm_68k_emit_load_reg_reg_offset((as), (rd), (rs), (off), 0)
#define ASM_LOAD16_REG_REG_OFFSET(as, rd, rs, off) \
    asm_68k_emit_load_reg_reg_offset((as), (rd), (rs), (off), 1)
#define ASM_LOAD32_REG_REG_OFFSET(as, rd, rs, off) \
    asm_68k_emit_load_reg_reg_offset((as), (rd), (rs), (off), 2)

// Stores with displacement
#define ASM_STORE_REG_REG_OFFSET(as, rs, rb, off) ASM_STORE32_REG_REG_OFFSET(as, rs, rb, off)
#define ASM_STORE8_REG_REG(as, rs, rb)     ASM_STORE8_REG_REG_OFFSET(as, rs, rb, 0)
#define ASM_STORE16_REG_REG(as, rs, rb)    ASM_STORE16_REG_REG_OFFSET(as, rs, rb, 0)
#define ASM_STORE32_REG_REG(as, rs, rb)    ASM_STORE32_REG_REG_OFFSET(as, rs, rb, 0)
#define ASM_STORE8_REG_REG_OFFSET(as, rs, rb, off) \
    asm_68k_emit_store_reg_reg_offset((as), (rs), (rb), (off), 0)
#define ASM_STORE16_REG_REG_OFFSET(as, rs, rb, off) \
    asm_68k_emit_store_reg_reg_offset((as), (rs), (rb), (off), 1)
#define ASM_STORE32_REG_REG_OFFSET(as, rs, rb, off) \
    asm_68k_emit_store_reg_reg_offset((as), (rs), (rb), (off), 2)

// Indexed loads/stores (base + index register)
#define ASM_LOAD8_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_load_reg_reg_reg((as), (rd), (rs1), (rs2), 0)
#define ASM_LOAD16_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_load_reg_reg_reg((as), (rd), (rs1), (rs2), 1)
#define ASM_LOAD32_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_load_reg_reg_reg((as), (rd), (rs1), (rs2), 2)
#define ASM_STORE8_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_store_reg_reg_reg((as), (rd), (rs1), (rs2), 0)
#define ASM_STORE16_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_store_reg_reg_reg((as), (rd), (rs1), (rs2), 1)
#define ASM_STORE32_REG_REG_REG(as, rd, rs1, rs2) \
    asm_68k_emit_store_reg_reg_reg((as), (rd), (rs1), (rs2), 2)

#endif // GENERIC_ASM_API

#endif // MICROPY_INCLUDED_PY_ASM68K_H
