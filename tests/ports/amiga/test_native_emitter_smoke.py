# @micropython.native 68k emitter smoke test.
#
# Regression test for the asm_68k_moveq dest-register encoding bug:
# MOVEQ puts its destination data register in bits 11-9, but the
# original code shifted by 8 rather than 9, so MOVEQ to any Dn (n>0)
# silently mis-targeted by one register slot. That broke every @native
# binary op against a small-int literal in the MOVEQ range (-128..127),
# because LOAD_CONST_SMALL_INT tags 0->1 / 1->3 / ... 63->127, which all
# pass through the MOVEQ fast path in asm_68k_mov_imm_dreg.
#
# The bug had a clean boundary at literal 64 (tagged value 129 spills
# past int8_t and selects the long-form move), so we cover both sides
# of that boundary and several ops to be sure the fix sticks.

import micropython

# --- comparisons against a small-int literal --------------------------


@micropython.native
def lt0(x):
    return x < 0


@micropython.native
def gt0(x):
    return x > 0


@micropython.native
def eq0(x):
    return x == 0


@micropython.native
def ne0(x):
    return x != 0


@micropython.native
def le_one(x):
    return x <= 1


@micropython.native
def ge_one(x):
    return x >= 1


assert lt0(-1) is True, lt0(-1)
assert lt0(0) is False
assert lt0(5) is False
assert gt0(5) is True
assert gt0(0) is False
assert eq0(0) is True
assert eq0(5) is False
assert ne0(0) is False
assert ne0(5) is True
assert le_one(0) is True
assert le_one(2) is False
assert ge_one(2) is True
assert ge_one(0) is False

# --- arithmetic against small-int literals at and across the MOVEQ
# boundary (literal 64 → tagged 129 spills past int8_t) -------------


@micropython.native
def add0(x):
    return x + 0


@micropython.native
def add1(x):
    return x + 1


@micropython.native
def add63(x):
    return x + 63  # last MOVEQ-range value


@micropython.native
def add64(x):
    return x + 64  # first long-form value


@micropython.native
def add_neg64(x):
    return x + (-64)  # negative MOVEQ-range


@micropython.native
def sub1(x):
    return x - 1


@micropython.native
def mul2(x):
    return x * 2


assert add0(5) == 5
assert add1(5) == 6
assert add63(5) == 68
assert add64(5) == 69
assert add_neg64(5) == -59
assert sub1(5) == 4
assert mul2(5) == 10

# --- two-arg ops still work (the path that didn't depend on MOVEQ) --


@micropython.native
def two_lt(a, b):
    return a < b


@micropython.native
def two_add(a, b):
    return a + b


assert two_lt(3, 5) is True
assert two_lt(5, 3) is False
assert two_add(2, 3) == 5

# --- viper int-typed comparisons (separate compile path) ------------


@micropython.viper
def viper_lt(a: int, b: int) -> int:
    return 1 if a < b else 0


assert viper_lt(3, 5) == 1
assert viper_lt(5, 3) == 0

# --- branch on small-int literal must not corrupt state -------------


@micropython.native
def abs_native(x):
    if x < 0:
        return -x
    return x


assert abs_native(-7) == 7
assert abs_native(7) == 7
assert abs_native(0) == 0

# --- nested loops with literal-bounded conditions --------------------


@micropython.native
def sum_lt(n):
    total = 0
    i = 0
    while i < n:
        total = total + i
        i = i + 1
    return total


assert sum_lt(10) == 45  # 0+1+...+9
assert sum_lt(0) == 0
assert sum_lt(1) == 0

print("OK")
