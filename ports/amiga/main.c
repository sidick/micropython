#include <stdint.h>
#include <string.h>

#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"

static void *heap_ptr;

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // Scan the live portion of the task stack: from the current SP (approximated
    // by a local variable) up to tc_SPUpper (the top of the stack allocation).
    void *dummy;
    struct Task *task = FindTask(NULL);
    gc_collect_start();
    gc_collect_root(&dummy,
        ((char *)task->tc_SPUpper - (char *)&dummy) / sizeof(void *));
    gc_collect_end();
}
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Allocate heap from Fast RAM; fall back to any available RAM.
    heap_ptr = AllocVec(MICROPY_HEAP_SIZE, MEMF_FAST | MEMF_PUBLIC);
    if (!heap_ptr) {
        heap_ptr = AllocVec(MICROPY_HEAP_SIZE, MEMF_ANY | MEMF_PUBLIC);
    }
    if (!heap_ptr) {
        return 1;
    }

    #if MICROPY_STACK_CHECK
    mp_stack_ctrl_init();
    mp_stack_set_limit(40 * 1024);
    #endif

    #if MICROPY_ENABLE_GC
    gc_init(heap_ptr, (char *)heap_ptr + MICROPY_HEAP_SIZE);
    #endif

    mp_init();

    #if MICROPY_ENABLE_COMPILER
    pyexec_friendly_repl();
    #endif

    mp_deinit();
    FreeVec(heap_ptr);
    return 0;
}

void nlr_jump_fail(void *val) {
    (void)val;
    for (;;) {
    }
}

void MP_NORETURN __fatal_error(const char *msg) {
    (void)msg;
    for (;;) {
    }
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    mp_hal_stdout_tx_strn("Assertion '", 11);
    mp_hal_stdout_tx_strn(expr, strlen(expr));
    mp_hal_stdout_tx_strn("' failed\r\n", 10);
    __fatal_error("Assertion failed");
}
#endif
