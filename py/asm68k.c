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

#include <stdio.h>
#include <string.h>

#include "py/emit.h"
#include "py/misc.h"
#include "py/mpconfig.h"

#if MICROPY_EMIT_68K

#include "py/asm68k.h"

// ---------------------------------------------------------------------------
// Register save/restore masks
//
// Callee-saved by this port: D2-D7, A2-A4
//
// MOVEM.L reglist, -(SP)  — predecrement form, mask bit order is REVERSED:
//   bit 15=D0, 14=D1, 13=D2, 12=D3, 11=D4, 10=D5, 9=D6, 8=D7,
//   bit 7=A0,  6=A1,  5=A2,  4=A3,  3=A4
// D2-D7: 0x3F00  A2-A4: 0x0038  Total: 0x3F38
//
// MOVEM.L (SP)+, reglist  — postincrement form, normal bit order:
//   bit 0=D0, 1=D1, 2=D2, ..., 7=D7, 8=A0, 9=A1, 10=A2, 11=A3, 12=A4
// D2-D7: 0x00FC  A2-A4: 0x1C00  Total: 0x1CFC
// ---------------------------------------------------------------------------
#define MOVEM_SAVE_MASK    (0x3F38u)
#define MOVEM_RESTORE_MASK (0x1CFCu)

// ---------------------------------------------------------------------------
// Entry / exit
// ---------------------------------------------------------------------------

void asm_68k_entry(asm_68k_t *as, mp_uint_t n_locals) {
    as->locals_count = n_locals;

    // LINK A5, #-N  — allocate n_locals*4 bytes of frame
    int32_t frame_size = -(int32_t)(n_locals * 4);
    asm_68k_emit16(as, 0x4E55);
    asm_68k_emit16(as, (uint16_t)(int16_t)frame_size);

    // MOVEM.L D2-D7/A2-A4, -(SP)  — save callee-saved registers
    asm_68k_emit16(as, 0x48E7);
    asm_68k_emit16(as, MOVEM_SAVE_MASK);

    // Load up to 4 arguments from the caller's frame into D0-D3.
    // AmigaOS/cdecl: 8(A5)=arg1, 12(A5)=arg2, 16(A5)=arg3, 20(A5)=arg4.
    asm_68k_load32_dreg_from_mem(as, ASM_68K_REG_D0, ASM_68K_REG_A5, 8);
    asm_68k_load32_dreg_from_mem(as, ASM_68K_REG_D1, ASM_68K_REG_A5, 12);
    asm_68k_load32_dreg_from_mem(as, ASM_68K_REG_D2, ASM_68K_REG_A5, 16);
    asm_68k_load32_dreg_from_mem(as, ASM_68K_REG_D3, ASM_68K_REG_A5, 20);
}

void asm_68k_exit(asm_68k_t *as) {
    // MOVEM.L (SP)+, D2-D7/A2-A4  — restore callee-saved registers
    asm_68k_emit16(as, 0x4CDF);
    asm_68k_emit16(as, MOVEM_RESTORE_MASK);
    // UNLK A5
    asm_68k_emit16(as, 0x4E5D);
    // RTS
    asm_68k_rts(as);
}

void asm_68k_end_pass(asm_68k_t *as) {
    (void)as;
}

// ---------------------------------------------------------------------------
// Indirect call through fun_table
// ---------------------------------------------------------------------------

void asm_68k_emit_call_ind(asm_68k_t *as, mp_uint_t index) {
    // Push args 4..1 in cdecl order (last arg pushed first).
    asm_68k_push_dreg(as, ASM_68K_REG_D3);
    asm_68k_push_dreg(as, ASM_68K_REG_D2);
    asm_68k_push_dreg(as, ASM_68K_REG_D1);
    asm_68k_push_dreg(as, ASM_68K_REG_D0);
    // MOVEA.L (index*4, A4), A0
    asm_68k_load32_areg_from_mem(as, ASM_68K_REG_A0, ASM_68K_REG_A4,
        (int32_t)(index * 4));
    // JSR (A0)
    asm_68k_jsr_ind(as, ASM_68K_REG_A0);
    // Pop 4*4 = 16 bytes of arguments.
    asm_68k_adda_imm_sp(as, 16);
}

// ---------------------------------------------------------------------------
// Branches
//
// 68k .W branch: opcode word + 16-bit displacement word (4 bytes total).
// Displacement is relative to (instruction_address + 2), i.e. after the
// opcode word.  So: disp = label_offset - (pos + 2).
// cc = 0  → BRA (always), cc = ASM_68K_CC_* → Bcc.
// ---------------------------------------------------------------------------

static void emit_branch(asm_68k_t *as, int cc, mp_uint_t label) {
    size_t pos = as->base.code_offset;
    asm_68k_emit16(as, (uint16_t)(0x6000u | ((unsigned)cc << 8)));
    size_t label_offset = as->base.label_offsets[label];
    int16_t disp = 0;
    if (label_offset != (size_t)-1) {
        disp = (int16_t)((ptrdiff_t)label_offset - (ptrdiff_t)(pos + 2));
    }
    asm_68k_emit16(as, (uint16_t)disp);
}

void asm_68k_emit_jump(asm_68k_t *as, mp_uint_t label) {
    emit_branch(as, 0, label); // cc=0 → BRA
}

void asm_68k_emit_jump_if_reg_eq(asm_68k_t *as, mp_uint_t rs1, mp_uint_t rs2,
    mp_uint_t label) {
    asm_68k_cmp_reg_reg(as, (int)rs1, (int)rs2); // rs1 - rs2, set flags
    emit_branch(as, ASM_68K_CC_EQ, label);
}

// Ensure rs is in a data register for TST; returns the data reg to use.
static int ensure_data_reg(asm_68k_t *as, mp_uint_t rs) {
    if (ASM_68K_IS_AREG(rs)) {
        asm_68k_mov_areg_dreg(as, ASM_68K_REG_D4, (int)rs);
        return ASM_68K_REG_D4;
    }
    return (int)rs;
}

void asm_68k_emit_jump_if_reg_nonzero(asm_68k_t *as, mp_uint_t rs,
    mp_uint_t label) {
    asm_68k_tst_dreg(as, ensure_data_reg(as, rs));
    emit_branch(as, ASM_68K_CC_NE, label);
}

void asm_68k_emit_jump_if_reg_zero(asm_68k_t *as, mp_uint_t rs,
    mp_uint_t label) {
    asm_68k_tst_dreg(as, ensure_data_reg(as, rs));
    emit_branch(as, ASM_68K_CC_EQ, label);
}

// ---------------------------------------------------------------------------
// Local variable access  (frame-relative, base = A5)
// ---------------------------------------------------------------------------

void asm_68k_emit_mov_reg_local(asm_68k_t *as, mp_uint_t rd, mp_uint_t local) {
    asm_68k_load_local(as, (int)rd, (int)local);
}

void asm_68k_emit_mov_local_reg(asm_68k_t *as, mp_uint_t local, mp_uint_t rs) {
    asm_68k_store_local(as, (int)local, (int)rs);
}

void asm_68k_emit_mov_reg_local_addr(asm_68k_t *as, mp_uint_t rd,
    mp_uint_t local) {
    int32_t off = asm_68k_local_offset(as, (int)local);
    // LEA (off, A5), An then optionally copy to data register.
    int ad = ASM_68K_IS_AREG(rd) ? (int)rd : ASM_68K_REG_A0;
    asm_68k_lea_disp(as, ad, ASM_68K_REG_A5, off);
    if (!ASM_68K_IS_AREG(rd)) {
        asm_68k_mov_areg_dreg(as, (int)rd, ASM_68K_REG_A0);
    }
}

// ---------------------------------------------------------------------------
// PC-relative address load  (address of a code label into a register)
// ---------------------------------------------------------------------------

void asm_68k_emit_mov_reg_pcrel(asm_68k_t *as, mp_uint_t rd, mp_uint_t label) {
    // LEA (d16, PC), An — d16 is relative to the displacement word itself.
    int ad = ASM_68K_IS_AREG(rd) ? (int)rd : ASM_68K_REG_A0;
    size_t disp_pos = asm_68k_lea_pcrel_placeholder(as, ad);
    size_t label_offset = as->base.label_offsets[label];
    if (label_offset != (size_t)-1) {
        int16_t disp = (int16_t)((ptrdiff_t)label_offset - (ptrdiff_t)disp_pos);
        asm_68k_patch16(as, disp_pos, (uint16_t)disp);
    }
    if (!ASM_68K_IS_AREG(rd)) {
        asm_68k_mov_areg_dreg(as, (int)rd, ASM_68K_REG_A0);
    }
}

// ---------------------------------------------------------------------------
// Load / store with displacement  (offset is an element count, scaled by size)
// ---------------------------------------------------------------------------

void asm_68k_emit_load_reg_reg_offset(asm_68k_t *as, mp_uint_t rd, mp_uint_t rs,
    int32_t offset, mp_uint_t op_size) {
    int32_t byte_off = offset << op_size;
    int areg = asm_68k_ensure_areg(as, (int)rs);
    if (op_size == 0) {
        asm_68k_load8_dreg_from_mem(as, (int)rd, areg, byte_off);
    } else if (op_size == 1) {
        asm_68k_load16_dreg_from_mem(as, (int)rd, areg, byte_off);
    } else {
        asm_68k_load32_reg_from_mem(as, (int)rd, areg, byte_off);
    }
}

void asm_68k_emit_store_reg_reg_offset(asm_68k_t *as, mp_uint_t rs_val,
    mp_uint_t rs_base, int32_t offset, mp_uint_t op_size) {
    int32_t byte_off = offset << op_size;
    int areg = asm_68k_ensure_areg(as, (int)rs_base);
    if (op_size == 0) {
        asm_68k_store8_dreg_to_mem(as, (int)rs_val, areg, byte_off);
    } else if (op_size == 1) {
        asm_68k_store16_dreg_to_mem(as, (int)rs_val, areg, byte_off);
    } else {
        asm_68k_store32_reg_to_mem(as, (int)rs_val, areg, byte_off);
    }
}

// ---------------------------------------------------------------------------
// Indexed load / store  (base + index * scale, scale = 1 << op_size)
//
// 68020 brief extension word with L-sized index and scale field:
//   [15:12] Xn number
//   [11]    W/L: 1 = L (32-bit index)
//   [10:9]  scale: 00=*1, 01=*2, 10=*4, 11=*8
//   [8]     0 = brief form
//   [7:0]   8-bit displacement (0)
// ---------------------------------------------------------------------------

static uint16_t idx_ext_scaled(int xn, mp_uint_t scale_log2) {
    uint16_t base = ASM_68K_IS_AREG(xn)
        ? (uint16_t)(0x8800u | ((unsigned)ASM_68K_AREG_NUM(xn) << 12))
        : (uint16_t)(0x0800u | ((unsigned)ASM_68K_DREG_NUM(xn) << 12));
    return base | (uint16_t)(scale_log2 << 9);
}

// Move An to D4 if xn is an address register (scale requires Dn index).
static int ensure_dreg_index(asm_68k_t *as, int xn) {
    if (ASM_68K_IS_AREG(xn)) {
        asm_68k_mov_areg_dreg(as, ASM_68K_REG_D4, xn);
        return ASM_68K_REG_D4;
    }
    return xn;
}

void asm_68k_emit_load_reg_reg_reg(asm_68k_t *as, mp_uint_t rd, mp_uint_t rs1,
    mp_uint_t rs2, mp_uint_t op_size) {
    int areg = asm_68k_ensure_areg(as, (int)rs1);
    int xn = ensure_dreg_index(as, (int)rs2);
    uint16_t ext = idx_ext_scaled(xn, op_size);
    if (op_size == 0) {
        // CLR.L rd; MOVE.B (areg, xn*1, 0), rd
        asm_68k_emit16(as, (uint16_t)(0x4280u | ASM_68K_DREG_NUM(rd)));
        asm_68k_emit16(as, (uint16_t)(0x1030u | ((unsigned)ASM_68K_DREG_NUM(rd) << 9) | ASM_68K_AREG_NUM(areg)));
        asm_68k_emit16(as, ext);
    } else if (op_size == 1) {
        // CLR.L rd; MOVE.W (areg, xn*2, 0), rd
        asm_68k_emit16(as, (uint16_t)(0x4280u | ASM_68K_DREG_NUM(rd)));
        asm_68k_emit16(as, (uint16_t)(0x3030u | ((unsigned)ASM_68K_DREG_NUM(rd) << 9) | ASM_68K_AREG_NUM(areg)));
        asm_68k_emit16(as, ext);
    } else {
        // MOVE.L / MOVEA.L (areg, xn*4, 0), rd
        if (ASM_68K_IS_AREG(rd)) {
            asm_68k_emit16(as, (uint16_t)(0x2070u | ((unsigned)ASM_68K_AREG_NUM(rd) << 9) | ASM_68K_AREG_NUM(areg)));
        } else {
            asm_68k_emit16(as, (uint16_t)(0x2030u | ((unsigned)ASM_68K_DREG_NUM(rd) << 9) | ASM_68K_AREG_NUM(areg)));
        }
        asm_68k_emit16(as, ext);
    }
}

void asm_68k_emit_store_reg_reg_reg(asm_68k_t *as, mp_uint_t rs_val,
    mp_uint_t rs1, mp_uint_t rs2, mp_uint_t op_size) {
    int areg = asm_68k_ensure_areg(as, (int)rs1);
    int xn = ensure_dreg_index(as, (int)rs2);
    uint16_t ext = idx_ext_scaled(xn, op_size);
    int dval = (int)rs_val;
    if (ASM_68K_IS_AREG(dval)) {
        // Indexed stores require a Dn source; copy An to D4 scratch.
        asm_68k_mov_areg_dreg(as, ASM_68K_REG_D4, dval);
        dval = ASM_68K_REG_D4;
    }
    if (op_size == 0) {
        // MOVE.B dval, (areg, xn*1, 0)
        asm_68k_emit16(as, (uint16_t)(0x1180u | ((unsigned)ASM_68K_AREG_NUM(areg) << 9) | ASM_68K_DREG_NUM(dval)));
        asm_68k_emit16(as, ext);
    } else if (op_size == 1) {
        // MOVE.W dval, (areg, xn*2, 0)
        asm_68k_emit16(as, (uint16_t)(0x3180u | ((unsigned)ASM_68K_AREG_NUM(areg) << 9) | ASM_68K_DREG_NUM(dval)));
        asm_68k_emit16(as, ext);
    } else {
        // MOVE.L dval, (areg, xn*4, 0)
        asm_68k_emit16(as, (uint16_t)(0x2180u | ((unsigned)ASM_68K_AREG_NUM(areg) << 9) | ASM_68K_DREG_NUM(dval)));
        asm_68k_emit16(as, ext);
    }
}

#endif // MICROPY_EMIT_68K
