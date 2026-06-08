// Phase 34 Step 1: AmigaOS-specific os.* primitives.
//
// Surfaces SetProtection / Lock+Examine and the FIBF_* protection
// bits so the frozen os.py can expose:
//
//   os.chmod(path, mask)    -> None
//   os.getprotect(path)     -> int (fib_Protection)
//   os.FIBF_READ / WRITE / EXECUTE / DELETE / ARCHIVE / PURE /
//      SCRIPT / HIDDEN
//
// We ship these in a separate `_osamiga` module rather than amending
// extmod/modos.c (the Amiga port stays out of upstream files). The
// frozen os.py imports the names so `os.chmod(...)` / `os.FIBF_READ`
// Just Work for callers.
//
// Bit semantics caveat: AmigaDOS protection flags are *inverted* for
// the four RWED bits -- a *set* bit means "denied", a *clear* bit
// means "allowed." So chmod(path, 0) grants all four (no denials).
// ARCHIVE / PURE / SCRIPT / HIDDEN follow the "set means yes"
// convention. Document this loudly in the frozen os.py docstring.
//
// Auto-requester suppression: AmigaDOS Lock() can pop a
// "Please insert volume X: in any drive" dialog when probing a path
// on an unmounted volume. We set pr_WindowPtr = -1 around the call
// and restore on exit so callers see a clean OSError, matching the
// pattern used by amiga.exists() / amiga.match().

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#if MICROPY_PY_AMIGA

// Translate AmigaDOS error codes into POSIX errno values. Mirrors
// the table in modamiga.c -- duplicated rather than exposed across
// the module boundary to keep modos/modosamiga independent.
static int osamiga_dos_errno_from(LONG err) {
    switch (err) {
        case ERROR_OBJECT_NOT_FOUND:
            return MP_ENOENT;
        case ERROR_OBJECT_EXISTS:
            return MP_EEXIST;
        case ERROR_DISK_FULL:
            return MP_ENOSPC;
        case ERROR_OBJECT_IN_USE:
            return MP_EBUSY;
        case ERROR_READ_PROTECTED:
        case ERROR_WRITE_PROTECTED:
            return MP_EACCES;
        case ERROR_OBJECT_WRONG_TYPE:
            return MP_EISDIR;
        case ERROR_NO_FREE_STORE:
            return MP_ENOMEM;
        case ERROR_BAD_TEMPLATE:
            return MP_EINVAL;
        case ERROR_DEVICE_NOT_MOUNTED:
        case ERROR_NOT_A_DOS_DISK:
        case ERROR_NO_DISK:
            return MP_ENODEV;
        case ERROR_INVALID_COMPONENT_NAME:
            return MP_EINVAL;
        default:
            return MP_EIO;
    }
}

// _osamiga.chmod(path, mask) -- SetProtection wrapper. Raises
// OSError on failure with the AmigaDOS error translated to POSIX.
static mp_obj_t mod_osamiga_chmod(mp_obj_t path_in, mp_obj_t mask_in) {
    const char *path = mp_obj_str_get_str(path_in);
    LONG mask = mp_obj_get_int(mask_in);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BOOL ok = SetProtection((STRPTR)path, mask);
    LONG err = IoErr();
    me->pr_WindowPtr = saved_wp;
    if (!ok) {
        mp_raise_OSError(osamiga_dos_errno_from(err));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_osamiga_chmod_obj, mod_osamiga_chmod);

// _osamiga.getprotect(path) -- Lock + Examine + fib_Protection
// readout. Returns the raw 32-bit protection mask as an int.
// Raises OSError if the path can't be locked or examined.
static mp_obj_t mod_osamiga_getprotect(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    if (lock == 0) {
        LONG err = IoErr();
        me->pr_WindowPtr = saved_wp;
        mp_raise_OSError(osamiga_dos_errno_from(err));
    }
    // FileInfoBlock is 260 bytes; AllocVec it to avoid blowing the
    // shell stack (default ~4 KB) on deeply nested calls.
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocVec(
        sizeof(struct FileInfoBlock), MEMF_CLEAR);
    if (fib == NULL) {
        UnLock(lock);
        me->pr_WindowPtr = saved_wp;
        mp_raise_OSError(MP_ENOMEM);
    }
    BOOL ok = Examine(lock, fib);
    LONG mask = ok ? fib->fib_Protection : 0;
    LONG err = ok ? 0 : IoErr();
    FreeVec(fib);
    UnLock(lock);
    me->pr_WindowPtr = saved_wp;
    if (!ok) {
        mp_raise_OSError(osamiga_dos_errno_from(err));
    }
    return mp_obj_new_int_from_uint((mp_uint_t)(ULONG)mask);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_osamiga_getprotect_obj,
    mod_osamiga_getprotect);

// ---------- module globals ----------

static const mp_rom_map_elem_t mod_osamiga_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR__osamiga) },
    { MP_ROM_QSTR(MP_QSTR_chmod),        MP_ROM_PTR(&mod_osamiga_chmod_obj) },
    { MP_ROM_QSTR(MP_QSTR_getprotect),   MP_ROM_PTR(&mod_osamiga_getprotect_obj) },
    // FIBF_* constants from <dos/dos.h>. AmigaDOS convention:
    // set bit ⇒ DENIED for RWED, set bit ⇒ ASSERTED for APSH.
    { MP_ROM_QSTR(MP_QSTR_FIBF_READ),    MP_ROM_INT(FIBF_READ) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_WRITE),   MP_ROM_INT(FIBF_WRITE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_EXECUTE), MP_ROM_INT(FIBF_EXECUTE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_DELETE),  MP_ROM_INT(FIBF_DELETE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_ARCHIVE), MP_ROM_INT(FIBF_ARCHIVE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_PURE),    MP_ROM_INT(FIBF_PURE) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_SCRIPT),  MP_ROM_INT(FIBF_SCRIPT) },
    { MP_ROM_QSTR(MP_QSTR_FIBF_HOLD),    MP_ROM_INT(FIBF_HOLD) },
};
static MP_DEFINE_CONST_DICT(mod_osamiga_globals, mod_osamiga_globals_table);

const mp_obj_module_t mod_osamiga_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_osamiga_globals,
};

MP_REGISTER_MODULE(MP_QSTR__osamiga, mod_osamiga_module);

#endif // MICROPY_PY_AMIGA
