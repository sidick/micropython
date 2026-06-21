/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
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

/* This file is preprocessed (.S), so it is compiled by every port but only
 * emits code on m68k; on all other architectures it preprocesses to nothing,
 * the same way the nlr<arch>.c files self-disable via their MICROPY_NLR_*
 * guards. */
#if defined(__m68k__)

| py/nlr68k.S — 68k non-local-return register save/restore.
|
| Companion to nlr68k_jump.c (which holds the C nlr_jump bookkeeping).  These
| two routines are kept in hand-written assembly so they do not depend on
| the toolchain supporting __attribute__((naked)) on m68k, and so the
| nlr_buf_t register layout is laid out by hand with no compiler prologue
| in the way.
|
| nlr_buf_t layout (machine words, 4 bytes each):
|     word 0  /  0(buf)  = prev          (managed by C nlr_push_tail)
|     word 1  /  4(buf)  = ret_val
|     word 2  /  8(buf)  = regs[0]  = D7   <-- NLR_BUF_IDX_LOCAL_1 (emitn68k.c)
|     word 3  / 12(buf)  = regs[1]  = D2
|     word 4  / 16(buf)  = regs[2]  = D3
|     word 5  / 20(buf)  = regs[3]  = D4
|     word 6  / 24(buf)  = regs[4]  = D5
|     word 7  / 28(buf)  = regs[5]  = D6
|     word 8  / 32(buf)  = regs[6]  = A2
|     word 9  / 36(buf)  = regs[7]  = A3
|     word 10 / 40(buf)  = regs[8]  = A4
|     word 11 / 44(buf)  = regs[9]  = A5
|     word 12 / 48(buf)  = regs[10] = A6
|     word 13 / 52(buf)  = regs[11] = caller SP (above the return address)
|     word 14 / 56(buf)  = regs[12] = return PC
|
| D7 must live at regs[0] because the native emitter carries the exception
| handler PC in REG_LOCAL_1 (=D7) across the nlr_push call and reads it back
| from word index NLR_BUF_IDX_LOCAL_1 (=2); see py/emitn68k.c and the
| emit_native_push_exc_stack protocol in py/emitnative.c.
|
| AmigaOS/cdecl callee-saved set is D2-D7 / A2-A6 (D0/D1/A0/A1 scratch,
| A7=SP).  We save the full set so a longjmp restores the caller exactly.

    .text
    .align  2
    .globl  _nlr_push
    .globl  _nlr_jump_tail

| unsigned int nlr_push(nlr_buf_t *nlr)
|   On entry: 0(sp) = return address, 4(sp) = nlr.
|   Saves the callee-saved register set into *nlr, then tail-calls
|   nlr_push_tail (which links the buffer and returns 0 in D0 straight to
|   our caller, leaving the return address on the stack untouched).
_nlr_push:
    move.l  4(sp),a0             | A0 = nlr
    move.l  d7,8(a0)             | regs[0] = D7
    movem.l d2-d6,12(a0)         | regs[1..5] = D2,D3,D4,D5,D6
    movem.l a2-a6,32(a0)         | regs[6..10] = A2,A3,A4,A5,A6
    lea     4(sp),a1             | caller SP = SP after our RTS pops retaddr
    move.l  a1,52(a0)            | regs[11] = caller SP
    move.l  (sp),d0              | return address
    move.l  d0,56(a0)            | regs[12] = return PC
    jmp     _nlr_push_tail       | C side: link buffer, return 0 to caller

| MP_NORETURN void nlr_jump_tail(nlr_buf_t *top)
|   Called from the C nlr_jump after MP_NLR_JUMP_HEAD has done its
|   bookkeeping.  Restores the saved register set and returns 1 (non-local
|   return) to the original caller of nlr_push.
|   On entry: 0(sp) = (our) return address [discarded], 4(sp) = top.
_nlr_jump_tail:
    move.l  4(sp),a0             | A0 = top
    move.l  8(a0),d7             | restore D7
    movem.l 12(a0),d2-d6         | restore D2..D6
    movem.l 32(a0),a2-a6         | restore A2..A6
    move.l  56(a0),a1            | A1 = saved return PC
    move.l  52(a0),sp            | restore caller SP
    move.l  a1,-(sp)             | push return PC for the RTS
    moveq   #1,d0                | non-local return value
    rts                          | resume at nlr_push's caller with D0 = 1

#endif /* defined(__m68k__) */
