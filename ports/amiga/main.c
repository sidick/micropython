#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"

// Static heap — avoids needing AllocVec from the NDK for now.
// Increase MICROPY_HEAP_SIZE in mpconfigport.h as required.
static char heap[MICROPY_HEAP_SIZE];

// Used by gc_collect() to find the top of the C stack.
static char *stack_top;

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // WARNING: does not capture roots held only in CPU registers; a task
    // switch during GC could theoretically miss some. Once the NDK is
    // available, replace with FindTask(NULL)->tc_SPLower/tc_SPUpper for
    // exact stack bounds.
    void *dummy;
    gc_collect_start();
    gc_collect_root(&dummy, ((mp_uint_t)stack_top - (mp_uint_t)&dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}
#endif

int main(int argc, char **argv) {
    int stack_dummy;
    stack_top = (char *)&stack_dummy;

    #if MICROPY_STACK_CHECK
    mp_stack_ctrl_init();
    mp_stack_set_limit(40 * 1024);
    #endif

    #if MICROPY_ENABLE_GC
    gc_init(heap, heap + sizeof(heap));
    #endif

    mp_init();

    #if MICROPY_ENABLE_COMPILER
    pyexec_friendly_repl();
    #endif

    mp_deinit();
    return 0;
}

// Called when an exception propagates with no enclosing NLR frame.
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
    printf("Assertion '%s' failed, at file %s:%d\n", expr, file, line);
    __fatal_error("Assertion failed");
}
#endif
