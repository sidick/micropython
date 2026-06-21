# @micropython.native 68k non-local-return (NLR) smoke test.
#
# Regression test for py/nlr68k.S + py/nlr68k_jump.c, the 68k
# register-based NLR that replaced the setjmp fallback. Under setjmp the
# native emitter's exception-handler PC (carried in REG_LOCAL_1 = D7 across
# nlr_push) landed inside the jmp_buf that setjmp overwrote, so every
# try/except/with inside @micropython.native code faulted the CPU
# (Invalid Memory Access). With nlr68k.S, D7 is saved at nlr_buf regs[0]
# (word index NLR_BUF_IDX_LOCAL_1 = 2; see py/emitn68k.c) and restored
# verbatim on the longjmp, so the handler PC survives.
#
# This exercises the save/restore path directly: the assertions only pass
# if the saved registers (D7 and the callee-saved set) come back intact
# across the non-local return.

import micropython

# --- try/except catches an explicit raise; exception args survive the
# longjmp (the value is read back out of nlr_buf.ret_val) --------------


@micropython.native
def catch_value():
    try:
        raise ValueError(42)
    except ValueError as e:
        return e.args[0]


assert catch_value() == 42, catch_value()


# --- try/finally runs the finally on both the normal and the exception
# path; the normal-path return value must be preserved -----------------


@micropython.native
def finally_normal(log):
    try:
        return 1
    finally:
        log.append("fin")


order = []
assert finally_normal(order) == 1
assert order == ["fin"], order


@micropython.native
def finally_raise(log):
    try:
        raise KeyError("k")
    finally:
        log.append("fin")


order = []
try:
    finally_raise(order)
    raised = False
except KeyError:
    raised = True
assert raised
assert order == ["fin"], order


# --- locals written inside the try block keep their values when the
# except block runs. This is the key register-restore check: `a` lives
# in a callee-saved register that nlr_jump must restore from nlr_buf. --


@micropython.native
def locals_survive():
    a = 100
    try:
        a = 200
        raise NameError
    except NameError:
        return a


assert locals_survive() == 200, locals_survive()


# --- nested try/except: inner re-raises, outer catches. Exercises two
# nlr buffers pushed/popped within one native frame -------------------


@micropython.native
def nested_reraise():
    try:
        try:
            raise IndexError(7)
        except IndexError:
            raise ValueError(8)
    except ValueError as e:
        return e.args[0]


assert nested_reraise() == 8, nested_reraise()


# --- exception propagates OUT of the native function to the caller:
# nlr_jump unwinds across the native frame's nlr_pop entirely ----------


@micropython.native
def propagates():
    raise RuntimeError("up")


caught = None
try:
    propagates()
except RuntimeError as e:
    caught = e.args[0]
assert caught == "up", caught


# --- with: __enter__/__exit__ ordering on the normal path, and __exit__
# observes the exception type on the error path -----------------------


class CM:
    def __init__(self, log):
        self.log = log

    def __enter__(self):
        self.log.append("enter")
        return self

    def __exit__(self, exc_type, exc, tb):
        self.log.append("exit:" + (exc_type.__name__ if exc_type else "None"))
        return False  # do not swallow


@micropython.native
def with_normal(log):
    with CM(log):
        log.append("body")


order = []
with_normal(order)
assert order == ["enter", "body", "exit:None"], order


@micropython.native
def with_raise(log):
    with CM(log):
        log.append("body")
        raise TypeError


order = []
try:
    with_raise(order)
    raised = False
except TypeError:
    raised = True
assert raised
assert order == ["enter", "body", "exit:TypeError"], order


# --- many sequential try blocks in one function: the nlr push/pop count
# must stay balanced so the chain doesn't leak or unwind too far -------


@micropython.native
def repeated():
    n = 0
    while n < 5:
        try:
            raise ValueError
        except ValueError:
            n += 1
    return n


assert repeated() == 5, repeated()


print("OK")
