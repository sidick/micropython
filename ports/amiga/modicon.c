// Phase 35 Step 1: thin wrapper around icon.library for reading
// AmigaOS .info files into Python-side DiskObject objects.
//
//   _icon.read(path) -> DiskObject
//     GetDiskObject(path) wrapped in a managed Python type. The
//     path is the file name without the trailing ".info" suffix,
//     matching the AmigaOS convention (icon.library appends it
//     itself).  Raises OSError if the .info isn't there.
//
//   _icon.WBDISK / WBDRAWER / WBTOOL / WBPROJECT / WBGARBAGE /
//   WBDEVICE / WBKICK / WBAPPICON
//     The do_Type values straight out of <workbench/workbench.h>.
//
//   _icon.DiskObject
//     The Python type itself, re-exported so callers can do
//     `isinstance(d, icon.DiskObject)`.
//
// The DiskObject Python type exposes (read-only this step):
//   .type           str  -- one of "disk" / "drawer" / "tool" /
//                            "project" / "garbage" / "device" /
//                            "kick" / "appicon", or the raw int
//                            for any future do_Type
//   .default_tool   str|None -- do_DefaultTool, None if NULL/empty
//   .stack_size     int     -- do_StackSize
//   .current_x      int     -- do_CurrentX
//   .current_y      int     -- do_CurrentY
//   .tooltypes      DiskObjectTooltypes -- mapping over do_ToolTypes
//   .close()                -- FreeDiskObject(do); idempotent
//   .__del__()              -- forwards to .close()
//
// The DiskObjectTooltypes type is a mapping over the parent
// DiskObject's NULL-terminated STRPTR[] (do_ToolTypes).  Each
// underlying string is "KEY=VALUE" or just "KEY" (flag-style).
// Step 1 is read-only:
//   m["KEY"]            -> bytes value after the '=' (b"" for flag-style)
//   "KEY" in m          -> bool
//   for k in m: ...     -> iterates keys as str (ASCII)
//   len(m)              -> int
//   m.keys() / values() / items() / get(k, default=None)
//
// Step 2 will add __setitem__/__delitem__ plus _icon.write()/.new().

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include <string.h>

#include <exec/types.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/icon.h>

#if MICROPY_PY_AMIGA

// icon.library accessor is owned by main.c -- it opens IconBase
// lazily and the system reaps it at process exit.  Reusing it
// here keeps a single source of truth for the library handle.
extern bool amiga_icon_open(void);

// ---------- DiskObject Python type ----------

typedef struct _amiga_diskobject_obj_t {
    mp_obj_base_t base;
    struct DiskObject *dobj;   // NULL after .close()
} amiga_diskobject_obj_t;

static const mp_obj_type_t amiga_diskobject_type;
static const mp_obj_type_t amiga_tooltypes_type;

// Wrap an already-allocated DiskObject* in a Python object.  Steals
// ownership: .close() / __del__ will FreeDiskObject() it.
static mp_obj_t amiga_diskobject_make(struct DiskObject *dobj) {
    amiga_diskobject_obj_t *self = mp_obj_malloc(
        amiga_diskobject_obj_t, &amiga_diskobject_type);
    self->dobj = dobj;
    return MP_OBJ_FROM_PTR(self);
}

// Map do_Type to the canonical lower-case string used in Python.
// Unknown types fall through to the raw integer.
static mp_obj_t amiga_diskobject_type_str(UBYTE t) {
    switch (t) {
        case WBDISK:    return MP_OBJ_NEW_QSTR(MP_QSTR_disk);
        case WBDRAWER:  return MP_OBJ_NEW_QSTR(MP_QSTR_drawer);
        case WBTOOL:    return MP_OBJ_NEW_QSTR(MP_QSTR_tool);
        case WBPROJECT: return MP_OBJ_NEW_QSTR(MP_QSTR_project);
        case WBGARBAGE: return MP_OBJ_NEW_QSTR(MP_QSTR_garbage);
        case WBDEVICE:  return MP_OBJ_NEW_QSTR(MP_QSTR_device);
        case WBKICK:    return MP_OBJ_NEW_QSTR(MP_QSTR_kick);
        case WBAPPICON: return MP_OBJ_NEW_QSTR(MP_QSTR_appicon);
        default:        return MP_OBJ_NEW_SMALL_INT(t);
    }
}

// Build the DiskObjectTooltypes mapping bound to this DiskObject.
static mp_obj_t amiga_tooltypes_make(amiga_diskobject_obj_t *parent);

static void amiga_diskobject_close_inplace(amiga_diskobject_obj_t *self) {
    if (self->dobj != NULL) {
        FreeDiskObject(self->dobj);
        self->dobj = NULL;
    }
}

static mp_obj_t amiga_diskobject_close(mp_obj_t self_in) {
    amiga_diskobject_obj_t *self = MP_OBJ_TO_PTR(self_in);
    amiga_diskobject_close_inplace(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_diskobject_close_obj,
    amiga_diskobject_close);

static void amiga_diskobject_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    amiga_diskobject_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // load
        if (self->dobj == NULL) {
            // .close() / __del__ remain callable on a closed object.
            if (attr == MP_QSTR_close || attr == MP_QSTR___del__) {
                dest[1] = MP_OBJ_SENTINEL;
            }
            return;
        }
        if (attr == MP_QSTR_type) {
            dest[0] = amiga_diskobject_type_str(self->dobj->do_Type);
            return;
        }
        if (attr == MP_QSTR_default_tool) {
            STRPTR dt = self->dobj->do_DefaultTool;
            if (dt == NULL || dt[0] == '\0') {
                dest[0] = mp_const_none;
            } else {
                dest[0] = mp_obj_new_str((const char *)dt, strlen((const char *)dt));
            }
            return;
        }
        if (attr == MP_QSTR_stack_size) {
            dest[0] = mp_obj_new_int(self->dobj->do_StackSize);
            return;
        }
        if (attr == MP_QSTR_current_x) {
            dest[0] = mp_obj_new_int(self->dobj->do_CurrentX);
            return;
        }
        if (attr == MP_QSTR_current_y) {
            dest[0] = mp_obj_new_int(self->dobj->do_CurrentY);
            return;
        }
        if (attr == MP_QSTR_tooltypes) {
            dest[0] = amiga_tooltypes_make(self);
            return;
        }
        // Fall through to locals_dict (close / __del__).
        dest[1] = MP_OBJ_SENTINEL;
        return;
    }
    // store / delete: nothing settable in Step 1.
}

static const mp_rom_map_elem_t amiga_diskobject_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_close),   MP_ROM_PTR(&amiga_diskobject_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&amiga_diskobject_close_obj) },
};
static MP_DEFINE_CONST_DICT(amiga_diskobject_locals_dict,
    amiga_diskobject_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    amiga_diskobject_type,
    MP_QSTR_DiskObject,
    MP_TYPE_FLAG_NONE,
    attr, amiga_diskobject_attr,
    locals_dict, &amiga_diskobject_locals_dict);

// ---------- DiskObjectTooltypes mapping ----------
//
// Wraps the parent DiskObject's do_ToolTypes pointer.  The parent
// is held by reference so the underlying array stays alive for as
// long as the mapping does.  We never own the array in Step 1.

typedef struct _amiga_tooltypes_obj_t {
    mp_obj_base_t base;
    amiga_diskobject_obj_t *parent;
} amiga_tooltypes_obj_t;

static mp_obj_t amiga_tooltypes_make(amiga_diskobject_obj_t *parent) {
    amiga_tooltypes_obj_t *self = mp_obj_malloc(
        amiga_tooltypes_obj_t, &amiga_tooltypes_type);
    self->parent = parent;
    return MP_OBJ_FROM_PTR(self);
}

static STRPTR *amiga_tooltypes_array(amiga_tooltypes_obj_t *self) {
    if (self->parent->dobj == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("DiskObject: closed"));
    }
    return self->parent->dobj->do_ToolTypes;
}

// Split a "KEY=VALUE" or "KEY" entry into key / value-after-equals.
// `value_out` points just past the '=' (or to the empty string when
// no '=' is present).  `key_len` is the number of bytes in the key.
static void amiga_tooltypes_split(const char *entry, size_t *key_len,
                                  const char **value_out) {
    const char *eq = strchr(entry, '=');
    if (eq == NULL) {
        *key_len = strlen(entry);
        *value_out = "";
    } else {
        *key_len = (size_t)(eq - entry);
        *value_out = eq + 1;
    }
}

static mp_obj_t amiga_tooltypes_subscr(mp_obj_t self_in, mp_obj_t key_in,
                                       mp_obj_t value) {
    if (value != MP_OBJ_SENTINEL) {
        // store / delete -- deferred to Step 2.
        mp_raise_TypeError(MP_ERROR_TEXT("tooltypes is read-only"));
    }
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    STRPTR *tt = amiga_tooltypes_array(self);
    if (tt == NULL) {
        mp_raise_type(&mp_type_KeyError);
    }
    size_t klen;
    const char *kdata = mp_obj_str_get_data(key_in, &klen);
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t elen;
        const char *evalue;
        amiga_tooltypes_split((const char *)*p, &elen, &evalue);
        if (elen == klen && memcmp(*p, kdata, klen) == 0) {
            return mp_obj_new_bytes((const byte *)evalue, strlen(evalue));
        }
    }
    mp_raise_type_arg(&mp_type_KeyError, key_in);
}

static mp_obj_t amiga_tooltypes_contains(amiga_tooltypes_obj_t *self,
                                         mp_obj_t key_in) {
    STRPTR *tt = amiga_tooltypes_array(self);
    if (tt == NULL) {
        return mp_const_false;
    }
    size_t klen;
    const char *kdata = mp_obj_str_get_data(key_in, &klen);
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t elen;
        const char *evalue;
        (void)evalue;
        amiga_tooltypes_split((const char *)*p, &elen, &evalue);
        if (elen == klen && memcmp(*p, kdata, klen) == 0) {
            return mp_const_true;
        }
    }
    return mp_const_false;
}

// Iterator state: a pointer into the parent's STRPTR[].  We carry
// the parent reference too so the array can't be freed underneath
// us mid-iteration.
typedef struct _amiga_tooltypes_iter_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    amiga_diskobject_obj_t *parent;
    size_t index;
} amiga_tooltypes_iter_t;

static mp_obj_t amiga_tooltypes_iternext(mp_obj_t self_in) {
    amiga_tooltypes_iter_t *it = MP_OBJ_TO_PTR(self_in);
    if (it->parent->dobj == NULL) {
        return MP_OBJ_STOP_ITERATION;
    }
    STRPTR *tt = it->parent->dobj->do_ToolTypes;
    if (tt == NULL || tt[it->index] == NULL) {
        return MP_OBJ_STOP_ITERATION;
    }
    const char *entry = (const char *)tt[it->index++];
    size_t klen;
    const char *evalue;
    (void)evalue;
    amiga_tooltypes_split(entry, &klen, &evalue);
    return mp_obj_new_str(entry, klen);
}

static mp_obj_t amiga_tooltypes_getiter(mp_obj_t self_in, mp_obj_iter_buf_t *iter_buf) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    assert(sizeof(amiga_tooltypes_iter_t) <= sizeof(mp_obj_iter_buf_t));
    amiga_tooltypes_iter_t *it = (amiga_tooltypes_iter_t *)iter_buf;
    it->base.type = &mp_type_polymorph_iter;
    it->iternext  = amiga_tooltypes_iternext;
    it->parent    = self->parent;
    it->index     = 0;
    return MP_OBJ_FROM_PTR(it);
}

static mp_obj_t amiga_tooltypes_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (op == MP_UNARY_OP_LEN) {
        STRPTR *tt = amiga_tooltypes_array(self);
        size_t n = 0;
        if (tt != NULL) {
            while (tt[n] != NULL) n++;
        }
        return mp_obj_new_int(n);
    }
    if (op == MP_UNARY_OP_BOOL) {
        STRPTR *tt = amiga_tooltypes_array(self);
        return mp_obj_new_bool(tt != NULL && tt[0] != NULL);
    }
    return MP_OBJ_NULL;
}

static mp_obj_t amiga_tooltypes_binary_op(mp_binary_op_t op, mp_obj_t lhs, mp_obj_t rhs) {
    if (op == MP_BINARY_OP_CONTAINS) {
        return amiga_tooltypes_contains(MP_OBJ_TO_PTR(lhs), rhs);
    }
    return MP_OBJ_NULL;
}

// dict-shaped helpers: keys() / values() / items() / get().  These
// build fresh lists / tuples on demand; the underlying array stays
// the source of truth.

static mp_obj_t amiga_tooltypes_keys(mp_obj_t self_in) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    STRPTR *tt = amiga_tooltypes_array(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (tt == NULL) {
        return list;
    }
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t klen;
        const char *evalue;
        (void)evalue;
        amiga_tooltypes_split((const char *)*p, &klen, &evalue);
        mp_obj_list_append(list, mp_obj_new_str((const char *)*p, klen));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_tooltypes_keys_obj,
    amiga_tooltypes_keys);

static mp_obj_t amiga_tooltypes_values(mp_obj_t self_in) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    STRPTR *tt = amiga_tooltypes_array(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (tt == NULL) {
        return list;
    }
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t klen;
        const char *evalue;
        amiga_tooltypes_split((const char *)*p, &klen, &evalue);
        mp_obj_list_append(list,
            mp_obj_new_bytes((const byte *)evalue, strlen(evalue)));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_tooltypes_values_obj,
    amiga_tooltypes_values);

static mp_obj_t amiga_tooltypes_items(mp_obj_t self_in) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(self_in);
    STRPTR *tt = amiga_tooltypes_array(self);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (tt == NULL) {
        return list;
    }
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t klen;
        const char *evalue;
        amiga_tooltypes_split((const char *)*p, &klen, &evalue);
        mp_obj_t pair[2] = {
            mp_obj_new_str((const char *)*p, klen),
            mp_obj_new_bytes((const byte *)evalue, strlen(evalue)),
        };
        mp_obj_list_append(list, mp_obj_new_tuple(2, pair));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_tooltypes_items_obj,
    amiga_tooltypes_items);

static mp_obj_t amiga_tooltypes_get(size_t n_args, const mp_obj_t *args) {
    amiga_tooltypes_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_obj_t default_val = (n_args >= 3) ? args[2] : mp_const_none;
    STRPTR *tt = amiga_tooltypes_array(self);
    if (tt == NULL) {
        return default_val;
    }
    size_t klen;
    const char *kdata = mp_obj_str_get_data(args[1], &klen);
    for (STRPTR *p = tt; *p != NULL; p++) {
        size_t elen;
        const char *evalue;
        amiga_tooltypes_split((const char *)*p, &elen, &evalue);
        if (elen == klen && memcmp(*p, kdata, klen) == 0) {
            return mp_obj_new_bytes((const byte *)evalue, strlen(evalue));
        }
    }
    return default_val;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_tooltypes_get_obj,
    2, 3, amiga_tooltypes_get);

static const mp_rom_map_elem_t amiga_tooltypes_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_keys),   MP_ROM_PTR(&amiga_tooltypes_keys_obj) },
    { MP_ROM_QSTR(MP_QSTR_values), MP_ROM_PTR(&amiga_tooltypes_values_obj) },
    { MP_ROM_QSTR(MP_QSTR_items),  MP_ROM_PTR(&amiga_tooltypes_items_obj) },
    { MP_ROM_QSTR(MP_QSTR_get),    MP_ROM_PTR(&amiga_tooltypes_get_obj) },
};
static MP_DEFINE_CONST_DICT(amiga_tooltypes_locals_dict,
    amiga_tooltypes_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    amiga_tooltypes_type,
    MP_QSTR_DiskObjectTooltypes,
    MP_TYPE_FLAG_ITER_IS_GETITER,
    subscr, amiga_tooltypes_subscr,
    iter, amiga_tooltypes_getiter,
    unary_op, amiga_tooltypes_unary_op,
    binary_op, amiga_tooltypes_binary_op,
    locals_dict, &amiga_tooltypes_locals_dict);

// ---------- _icon.read(path) ----------

static mp_obj_t mod_icon_read(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    if (!amiga_icon_open()) {
        mp_raise_OSError(MP_EIO);
    }
    struct DiskObject *dobj = GetDiskObject((STRPTR)path);
    if (dobj == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    return amiga_diskobject_make(dobj);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_icon_read_obj, mod_icon_read);

// ---------- module globals ----------

static const mp_rom_map_elem_t mod_icon_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR__icon) },
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&mod_icon_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_DiskObject),
        MP_ROM_PTR(&amiga_diskobject_type) },
    // do_Type values from <workbench/workbench.h>.
    { MP_ROM_QSTR(MP_QSTR_WBDISK),    MP_ROM_INT(WBDISK) },
    { MP_ROM_QSTR(MP_QSTR_WBDRAWER),  MP_ROM_INT(WBDRAWER) },
    { MP_ROM_QSTR(MP_QSTR_WBTOOL),    MP_ROM_INT(WBTOOL) },
    { MP_ROM_QSTR(MP_QSTR_WBPROJECT), MP_ROM_INT(WBPROJECT) },
    { MP_ROM_QSTR(MP_QSTR_WBGARBAGE), MP_ROM_INT(WBGARBAGE) },
    { MP_ROM_QSTR(MP_QSTR_WBDEVICE),  MP_ROM_INT(WBDEVICE) },
    { MP_ROM_QSTR(MP_QSTR_WBKICK),    MP_ROM_INT(WBKICK) },
    { MP_ROM_QSTR(MP_QSTR_WBAPPICON), MP_ROM_INT(WBAPPICON) },
};
static MP_DEFINE_CONST_DICT(mod_icon_globals, mod_icon_globals_table);

const mp_obj_module_t mod_icon_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_icon_globals,
};

MP_REGISTER_MODULE(MP_QSTR__icon, mod_icon_module);

#endif // MICROPY_PY_AMIGA
