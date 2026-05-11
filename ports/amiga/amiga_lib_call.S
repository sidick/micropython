| ports/amiga/amiga_lib_call.S — runtime AmigaOS library-call trampoline.
|
| Calls any AmigaOS library function given a base pointer and signed LVO
| offset (e.g. -96 for DisplayBeep), with the full D0-D7 / A0-A5 register
| set populated from the C-side argument array.  A6 is loaded with the
| library base before the call (standard AmigaOS ABI); the function's
| RTS returns control here so we can restore callee-saved registers.
|
| C prototype (matched by the order args are read from the stack below):
|   uint32_t amiga_lib_call_asm(
|       uint32_t base, int32_t offset,
|       uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
|       uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7,
|       uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
|       uint32_t a4, uint32_t a5);
|
| Implementation note: we use the RTS-trick rather than a computed JSR.
| With all of D0-D7 / A0-A5 already committed to user-supplied values,
| there's no free register left to hold the call target when the JSR
| itself executes.  Pushing the target plus a local return address and
| issuing RTS gets us there with zero free registers.

    .text
    .align  2
    .globl  _amiga_lib_call_asm

_amiga_lib_call_asm:
    | Save callee-saved registers (m68k SysV ABI: d2-d7, a2-a5; plus a6
    | which we will overwrite with the library base).  11 longs = 44 bytes.
    movem.l d2-d7/a2-a6, -(sp)
    | After movem, sp -= 44.  Caller-arg offsets now:
    |   48(sp) = base
    |   52(sp) = offset (signed)
    |   56(sp) = d0     60(sp) = d1     64(sp) = d2     68(sp) = d3
    |   72(sp) = d4     76(sp) = d5     80(sp) = d6     84(sp) = d7
    |   88(sp) = a0     92(sp) = a1     96(sp) = a2    100(sp) = a3
    |  104(sp) = a4    108(sp) = a5

    move.l  48(sp), a6           | A6 = library base (AmigaOS requirement)
    move.l  52(sp), d0           | scratch: signed offset
    add.l   a6, d0               | D0 = absolute call target

    | Stage an RTS-style indirect call:
    |   push retaddr (`1f`, where the library's RTS will return us to)
    |   push target  (where our RTS below jumps to)
    | At the moment of `rts`, top-of-stack is the call target.  Issuing
    | `rts` pops it into PC, transferring control to the library; the
    | library's own `rts` then pops the `1f` retaddr and returns to us
    | for cleanup.  Effectively a JSR through a runtime-computed pointer
    | without burning a scratch register.
    pea     1f(pc)
    move.l  d0, -(sp)
    | sp shifted by another 8 from the movem block.  Offsets now:
    |   56(sp) = base    60(sp) = offset
    |   64(sp) = d0      68(sp) = d1      72(sp) = d2      76(sp) = d3
    |   80(sp) = d4      84(sp) = d5      88(sp) = d6      92(sp) = d7
    |   96(sp) = a0     100(sp) = a1     104(sp) = a2     108(sp) = a3
    |  112(sp) = a4     116(sp) = a5

    | Load D1-D7 and A0-A5 from the args; D0 is loaded last because the
    | register held the scratch target above.
    move.l  68(sp), d1
    move.l  72(sp), d2
    move.l  76(sp), d3
    move.l  80(sp), d4
    move.l  84(sp), d5
    move.l  88(sp), d6
    move.l  92(sp), d7
    move.l  96(sp), a0
    move.l  100(sp), a1
    move.l  104(sp), a2
    move.l  108(sp), a3
    move.l  112(sp), a4
    move.l  116(sp), a5
    move.l  64(sp), d0

    rts                          | pops call target → jumps to library

1:  | Library function's RTS returns here.  D0 holds the call result.
    movem.l (sp)+, d2-d7/a2-a6
    rts
