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

// Motorola 68k (68020+) native code emitter — thin wrapper around emitnative.c

#include "py/mpconfig.h"

#if MICROPY_EMIT_68K

// Export the generic assembler API macros from asm68k.h
#define GENERIC_ASM_API (1)
#include "py/asm68k.h"

// Word index within nlr_buf_t for the exception handler PC local.
// This is regs[0], where py/nlr68k.S saves REG_LOCAL_1 (D7).
// The native emitter carries the exception handler PC in D7 across the
// nlr_push call and reads it back from this slot after a non-local return.
#define NLR_BUF_IDX_LOCAL_1 (2)

#define N_68K (1)
#define EXPORT_FUN(name) emit_native_68k_##name
#include "py/emitnative.c"

#endif // MICROPY_EMIT_68K
