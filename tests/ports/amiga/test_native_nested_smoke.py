# @micropython.native nested-function / closure / generator smoke test.
#
# Regression test for the big-endian prelude-index byte-order bug
# (py/emitnative.c). The native emitter stores prelude_ptr_index in the
# first machine word of the function data via mp_asm_base_data(), which
# emits little-endian. On the big-endian 68k that word is read back as a
# native uintptr_t by mp_obj_fun_native_get_prelude_ptr(), so an index of
# 1 came back as 0x01000000 and indexed child_table[] miles out of bounds
# -> Invalid Memory Access. Any @micropython.native function that creates
# another function object (nested def, closure, generator) tripped it.
#
# Fixed by byte-swapping the prelude_ptr_index / generator start_offset
# words on N_68K. These cases all exercise that path.

import micropython


# --- nested native function defined and returned, then called ---------


@micropython.native
def make_adder():
    @micropython.native
    def g():
        return 123

    return g


assert make_adder()() == 123, make_adder()()


# --- plain (non-native) nested def inside a native function -----------


@micropython.native
def make_plain():
    def g():
        return 7

    return g


assert make_plain()() == 7, make_plain()()


# --- closure: native inner capturing a native outer's local ----------


@micropython.native
def make_counter(start):
    n = start

    @micropython.native
    def step():
        nonlocal n
        n += 1
        return n

    return step


c = make_counter(10)
assert c() == 11, c()
assert c() == 12, c()


# --- native generator ------------------------------------------------


@micropython.native
def gen(n):
    i = 0
    while i < n:
        yield i
        i += 1


assert list(gen(4)) == [0, 1, 2, 3], list(gen(4))


# --- native generator with a value sent in / exception path ----------


@micropython.native
def echo():
    while True:
        x = yield
        if x is None:
            return
        yield x * 2


g = echo()
next(g)  # prime
assert g.send(5) == 10, "send"


# --- nested native defined inside a loop (fresh function each time) ---


@micropython.native
def factory():
    out = []
    i = 0
    while i < 3:

        @micropython.native
        def leaf():
            return 42

        out.append(leaf)
        i += 1
    return out


fns = factory()
assert len(fns) == 3
assert all(f() == 42 for f in fns), "leaf calls"


print("OK")
