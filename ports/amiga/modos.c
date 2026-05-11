// Port-local os.getenv / os.putenv / os.unsetenv bodies. Included by
// extmod/modos.c via MICROPY_PY_OS_INCLUDEFILE (see mpconfigport.h).
//
// Backed by dos.library V36+ GetVar/SetVar/DeleteVar, so MicroPython and
// the AmigaShell share one env-var store. setting a variable here is
// visible at the shell prompt afterwards, and vice versa. That's a real
// AmigaDOS property -- ENV: is a filesystem-backed assign, not a
// per-process copy as on Unix.
//
// All three calls use flags=0 (no GVF_GLOBAL_ONLY / GVF_LOCAL_ONLY). On
// real AmigaOS this means:
//   - getenv: look up local CLI vars first, fall through to global (ENV:).
//   - putenv: create or replace a local CLI variable. Inherited by child
//     processes launched via amiga.execute(); not visible to other
//     unrelated shells.
//   - unsetenv: remove the local variable.
// This matches Unix os.putenv semantics ("affects this process and its
// children") and works under both vamos and real AmigaOS. (Vamos only
// implements local-var bookkeeping — GVF_GLOBAL_ONLY paths fall through
// to DOSFALSE.)
//
// Users wanting a system-wide variable visible to all shells should
// write directly to ENV:, which is just a filesystem assign:
//
//     with open("ENV:MY_VAR", "w") as f:
//         f.write("value")
//
// And for persistence across reboots, also write the same value to
// ENVARC: (boot-time AmigaDOS copies ENVARC: into ENV:).
//
// Vamos workarounds:
//
//   * GetVar returns 0 (rather than -1) for missing variables, so we
//     can't reliably distinguish "not found" from "found but empty" via
//     the return value alone. Code below treats `len <= 0` as not-found,
//     which means a genuine empty-string variable comes back as missing
//     on vamos. Real AmigaOS GetVar returns -1 for missing and 0 for
//     empty, so the distinction works correctly there. Empty-string
//     env vars on AmigaOS are rare in practice (paths, names, and
//     flags always have content), so this is acceptable. (FindVar
//     would be the obvious test-existence call, but vamos's FindVar
//     hits an AttributeError in its success-path logging.)
//
//   * DeleteVar reads its `flags` argument from D4 in vamos (the NDK
//     fd specifies D2). With stale data in D4, vamos can take the
//     GVF_GLOBAL_ONLY branch and silently skip the deletion. We use
//     the V36-documented "SetVar with NULL buffer" form for delete,
//     which vamos handles correctly (its SetVar register reads match
//     the NDK fd).

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/var.h>
#include <proto/dos.h>

// 1 KiB is comfortably above the size of any conventional ENV: variable.
// AmigaDOS doesn't impose a hard limit on env-var length, but anything
// longer than this is essentially using ENV: as a file store, which has
// better idioms (open() the file).
#define AMIGA_ENV_BUF_SIZE 1024

static mp_obj_t mp_os_getenv(size_t n_args, const mp_obj_t *args) {
    const char *name = mp_obj_str_get_str(args[0]);
    char buf[AMIGA_ENV_BUF_SIZE];
    LONG len = GetVar((STRPTR)name, (STRPTR)buf, sizeof(buf), 0);
    if (len <= 0) {
        // Real AmigaOS: len < 0 means not found, len == 0 means found-empty.
        // Vamos: len == 0 means either. Treating both as not-found here
        // gives the correct answer on vamos for missing vars and only
        // misreports the (rare) empty-string case on real AmigaOS.
        if (n_args == 2) {
            return args[1];
        }
        return mp_const_none;
    }
    // GetVar returned the byte count copied (excluding the trailing NUL
    // it writes when GVF_BINARY_VAR is not set).
    return mp_obj_new_str(buf, len);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mp_os_getenv_obj, 1, 2, mp_os_getenv);

static mp_obj_t mp_os_putenv(mp_obj_t key_in, mp_obj_t value_in) {
    const char *key = mp_obj_str_get_str(key_in);
    size_t value_len;
    const char *value = mp_obj_str_get_data(value_in, &value_len);
    // SetVar returns DOSTRUE / DOSFALSE. Failure is rare (out of memory,
    // or invalid name); surface as OSError using IoErr().
    if (!SetVar((STRPTR)key, (STRPTR)value, (LONG)value_len, 0)) {
        mp_raise_OSError((int)IoErr());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mp_os_putenv_obj, mp_os_putenv);

static mp_obj_t mp_os_unsetenv(mp_obj_t key_in) {
    const char *key = mp_obj_str_get_str(key_in);
    // V36-documented "SetVar with NULL buffer deletes the variable" form,
    // used in preference to DeleteVar because vamos's DeleteVar reads
    // flags from the wrong register. Returns DOSTRUE whether the variable
    // existed or not, so we don't need to filter ERROR_OBJECT_NOT_FOUND
    // here. SetVar(NULL) failing is essentially impossible — only out-of-
    // memory while the name is being looked up — but surface it anyway.
    if (!SetVar((STRPTR)key, NULL, 0, 0)) {
        LONG err = IoErr();
        if (err != 0) {
            mp_raise_OSError((int)err);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mp_os_unsetenv_obj, mp_os_unsetenv);
