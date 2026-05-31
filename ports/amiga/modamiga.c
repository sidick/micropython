#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/objlist.h"
#include "py/gc.h"
#include "py/mperrno.h"

#include <string.h>

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dosasl.h>
#include <rexx/storage.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/rexxsyslib.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>

#if MICROPY_PY_AMIGA

// amiga.os_version() -> (version, revision)
static mp_obj_t amiga_os_version(void) {
    mp_obj_t items[2] = {
        mp_obj_new_int(SysBase->LibNode.lib_Version),
        mp_obj_new_int(SysBase->LibNode.lib_Revision),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_os_version_obj, amiga_os_version);

// amiga.find_task(name=None) -> int address or None
static mp_obj_t amiga_find_task(size_t n_args, const mp_obj_t *args) {
    const char *name = NULL;
    if (n_args == 1 && args[0] != mp_const_none) {
        name = mp_obj_str_get_str(args[0]);
    }
    struct Task *task = FindTask((STRPTR)name);
    if (task == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_int((mp_int_t)(uintptr_t)task);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_find_task_obj, 0, 1, amiga_find_task);

// amiga.alloc_vec(size, flags=MEMF_ANY) -> int address
static mp_obj_t amiga_alloc_vec(size_t n_args, const mp_obj_t *args) {
    ULONG size = (ULONG)mp_obj_get_int(args[0]);
    ULONG flags = (n_args >= 2) ? (ULONG)mp_obj_get_int(args[1]) : MEMF_ANY;
    APTR mem = AllocVec(size, flags);
    if (mem == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("AllocVec failed"));
    }
    return mp_obj_new_int((mp_int_t)(uintptr_t)mem);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_alloc_vec_obj, 1, 2, amiga_alloc_vec);

// amiga.free_vec(addr)
static mp_obj_t amiga_free_vec(mp_obj_t addr_obj) {
    APTR mem = (APTR)(uintptr_t)mp_obj_get_int(addr_obj);
    FreeVec(mem);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_free_vec_obj, amiga_free_vec);

// amiga.execute(command) -> return code (0=OK, 5=WARN, 10=ERROR, 20=FAILURE, -1=failed to start)
static mp_obj_t amiga_execute(mp_obj_t cmd_obj) {
    const char *cmd = mp_obj_str_get_str(cmd_obj);
    LONG rc = SystemTagList((STRPTR)cmd, NULL);
    return mp_obj_new_int(rc);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_execute_obj, amiga_execute);

// amiga.heap_info() -> (total_bytes, free_bytes, num_arenas).
// Wraps gc_info() (live total/free) and the port-local chunk-count
// tracker in main.c. Useful for tuning -X heap / -X maxheap to a
// workload, or for monitoring how aggressively the heap has grown.
extern size_t amiga_heap_chunk_count(void);
static mp_obj_t amiga_heap_info(void) {
    gc_info_t info;
    gc_info(&info);
    mp_obj_t items[3] = {
        mp_obj_new_int_from_uint(info.total),
        mp_obj_new_int_from_uint(info.free),
        mp_obj_new_int_from_uint((mp_uint_t)amiga_heap_chunk_count()),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_heap_info_obj, amiga_heap_info);

// Bebbo crt0 fills _WBenchMsg with the WBStartup pointer on a Workbench
// launch (or leaves it NULL on CLI). main.c handles its lifecycle; we just
// dereference it here. amiga_icon_open() / amiga_wb_get_diskobject() are
// the cached icon.library / DiskObject set up on startup in main.c.
extern struct WBStartup *_WBenchMsg;
extern bool amiga_icon_open(void);
extern struct DiskObject *amiga_wb_get_diskobject(void);

// amiga.launched_from_workbench() -> True if the process was started by
// double-clicking its icon on the Workbench (or via WBStartup), False if
// from a Shell. amiga.wb_selected_files() / amiga.tooltype() are only
// meaningful in the True case.
static mp_obj_t amiga_launched_from_workbench(void) {
    return mp_obj_new_bool(_WBenchMsg != NULL);
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_launched_from_workbench_obj,
    amiga_launched_from_workbench);

// amiga.wb_selected_files() -> list[str] of every shift-clicked icon
// alongside the tool. sm_ArgList[0] is the tool itself and is skipped;
// the rest carry (wa_Lock, wa_Name) pairs which are converted to absolute
// paths via NameFromLock + the wa_Name suffix. A wa_Lock of zero
// (entries given by Workbench from the "Information" requester) is
// treated as relative to the current directory. Empty list if not WB
// launched, or only the tool is selected.
static mp_obj_t amiga_wb_selected_files(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    if (_WBenchMsg == NULL) {
        return list;
    }
    char path[AMIGA_PATH_MAX];
    for (LONG i = 1; i < _WBenchMsg->sm_NumArgs; i++) {
        struct WBArg *arg = &_WBenchMsg->sm_ArgList[i];
        path[0] = '\0';
        if (arg->wa_Lock != 0) {
            if (!NameFromLock(arg->wa_Lock, (STRPTR)path, sizeof(path))) {
                path[0] = '\0';
            }
        }
        if (arg->wa_Name != NULL && arg->wa_Name[0] != '\0') {
            // AddPart appends wa_Name to path, inserting "/" or ":" as
            // appropriate for AmigaDOS path syntax.
            if (!AddPart((STRPTR)path, (STRPTR)arg->wa_Name, sizeof(path))) {
                continue;
            }
        }
        if (path[0] == '\0') {
            continue;
        }
        mp_obj_list_append(list, mp_obj_new_str(path, strlen(path)));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_wb_selected_files_obj,
    amiga_wb_selected_files);

// amiga.tooltype(name, default=None) -> str|default. Looks up a tooltype
// in the executable's .info. Tooltypes are the standard mechanism for
// configuring a Workbench-launched program ("Information" requester);
// they're conventionally KEY=VALUE strings stored alongside the icon.
// Returns the value portion after "=", or "" if the tooltype is present
// without "=", or the default if absent / no diskobject / no icon.library.
// Looked up against the cached DiskObject from main.c.
static mp_obj_t amiga_tooltype(size_t n_args, const mp_obj_t *args) {
    mp_obj_t default_val = (n_args >= 2) ? args[1] : mp_const_none;
    const char *name = mp_obj_str_get_str(args[0]);
    struct DiskObject *dobj = amiga_wb_get_diskobject();
    if (dobj == NULL || dobj->do_ToolTypes == NULL) {
        return default_val;
    }
    UBYTE *val = FindToolType((CONST_STRPTR *)dobj->do_ToolTypes, (CONST_STRPTR)name);
    if (val == NULL) {
        return default_val;
    }
    return mp_obj_new_str((const char *)val, strlen((const char *)val));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_tooltype_obj, 1, 2, amiga_tooltype);

// amiga.exists(path) -> True if the path resolves to a file, directory or
// volume, False otherwise. Suppresses AmigaDOS auto-requesters around the
// Lock() so a caller probing a path on an unmounted volume gets a clean
// False back instead of a "Please insert volume X: in any drive" dialog.
// pr_WindowPtr is restored on exit so the rest of the binary is unaffected.
static mp_obj_t amiga_exists(mp_obj_t path_obj) {
    const char *path = mp_obj_str_get_str(path_obj);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (lock) {
        UnLock(lock);
        return mp_const_true;
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_exists_obj, amiga_exists);

// ---------- Phase 21 / 22: dos.library introspection and pattern matching ----------

static int amiga_dos_errno_from(LONG err) {
    switch (err) {
        case ERROR_OBJECT_NOT_FOUND:    return MP_ENOENT;
        case ERROR_OBJECT_EXISTS:       return MP_EEXIST;
        case ERROR_DISK_FULL:           return MP_ENOSPC;
        case ERROR_OBJECT_IN_USE:       return MP_EBUSY;
        case ERROR_READ_PROTECTED:
        case ERROR_WRITE_PROTECTED:     return MP_EACCES;
        case ERROR_OBJECT_WRONG_TYPE:   return MP_EISDIR;
        case ERROR_NO_FREE_STORE:       return MP_ENOMEM;
        case ERROR_BAD_TEMPLATE:        return MP_EINVAL;
        case ERROR_DEVICE_NOT_MOUNTED:
        case ERROR_NOT_A_DOS_DISK:
        case ERROR_NO_DISK:             return MP_ENODEV;
        case ERROR_INVALID_COMPONENT_NAME: return MP_EINVAL;
        default:                        return MP_EIO;
    }
}

// Copy a BSTR (BCPL string, length byte then chars) into a C buffer and
// append ':' so the result matches user-visible AmigaDOS conventions
// ("Work:", "LIBS:"). Buffer must be at least 34 bytes.
static size_t amiga_bstr_to_volname(BSTR bstr_bptr, char *out, size_t cap) {
    UBYTE *bstr = (UBYTE *)BADDR(bstr_bptr);
    UBYTE len = bstr[0];
    if (len > cap - 2) {
        len = cap - 2;
    }
    memcpy(out, bstr + 1, len);
    out[len] = ':';
    out[len + 1] = '\0';
    return (size_t)(len + 1);
}

// amiga.volumes() -> list[str] of mounted volume names with trailing ':'.
// Walks the dos.library DosList with LDF_VOLUMES + LDF_READ. Devices
// (DH0:, DF0:, etc.) and assigns are reported separately by amiga.assigns();
// only actually-mounted volumes show up here.
static mp_obj_t amiga_volumes(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    struct DosList *dl = LockDosList(LDF_VOLUMES | LDF_READ);
    if (dl == NULL) {
        return list;
    }
    char name[34];
    while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_READ)) != NULL) {
        size_t len = amiga_bstr_to_volname(dl->dol_Name, name, sizeof(name));
        mp_obj_list_append(list, mp_obj_new_str(name, len));
    }
    UnLockDosList(LDF_VOLUMES | LDF_READ);
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_volumes_obj, amiga_volumes);

// amiga.assigns() -> dict[str, str] mapping assign name ("LIBS:") to the
// resolved target path. Standard assigns (DLT_DIRECTORY) hold an open
// Lock — NameFromLock recovers the full path. Late- / non-binding
// assigns (DLT_LATE / DLT_NONBINDING) store the target as a plain
// string and have no Lock. Multi-directory assigns are reported by
// their first directory only; callers needing the full chain can fall
// back to the `Assign` shell command.
static mp_obj_t amiga_assigns(void) {
    mp_obj_t dict = mp_obj_new_dict(0);
    struct DosList *dl = LockDosList(LDF_ASSIGNS | LDF_READ);
    if (dl == NULL) {
        return dict;
    }
    char name[34];
    char target[256];
    while ((dl = NextDosEntry(dl, LDF_ASSIGNS | LDF_READ)) != NULL) {
        size_t nlen = amiga_bstr_to_volname(dl->dol_Name, name, sizeof(name));
        target[0] = '\0';
        size_t tlen = 0;
        if (dl->dol_Type == DLT_DIRECTORY && dl->dol_Lock != 0) {
            if (NameFromLock(dl->dol_Lock, (STRPTR)target, sizeof(target))) {
                tlen = strlen(target);
            }
        } else if (dl->dol_Type == DLT_LATE || dl->dol_Type == DLT_NONBINDING) {
            const char *t = (const char *)dl->dol_misc.dol_assign.dol_AssignName;
            if (t != NULL) {
                tlen = strlen(t);
                if (tlen >= sizeof(target)) {
                    tlen = sizeof(target) - 1;
                }
                memcpy(target, t, tlen);
                target[tlen] = '\0';
            }
        }
        mp_obj_dict_store(dict,
            mp_obj_new_str(name, nlen),
            mp_obj_new_str(target, tlen));
    }
    UnLockDosList(LDF_ASSIGNS | LDF_READ);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_assigns_obj, amiga_assigns);

// amiga.disk_info(path) -> (free_bytes, total_bytes, block_size).
// `path` can be a volume name ("Work:"), an assign ("LIBS:"), or any
// existing file/dir on the target volume. Auto-requesters are suppressed
// so an unmounted volume raises OSError instead of popping a system
// dialog. Results are 64-bit so >4 GB volumes (typical under Amiberry)
// report correctly.
static mp_obj_t amiga_disk_info(mp_obj_t path_obj) {
    const char *path = mp_obj_str_get_str(path_obj);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (lock == 0) {
        mp_raise_OSError(amiga_dos_errno_from(IoErr()));
    }
    struct InfoData info;
    LONG ok = Info(lock, &info);
    LONG err = ok ? 0 : IoErr();
    UnLock(lock);
    if (!ok) {
        mp_raise_OSError(amiga_dos_errno_from(err));
    }
    uint64_t bps = (uint64_t)(ULONG)info.id_BytesPerBlock;
    uint64_t total = (uint64_t)(ULONG)info.id_NumBlocks * bps;
    uint64_t used = (uint64_t)(ULONG)info.id_NumBlocksUsed * bps;
    uint64_t freebytes = (total > used) ? (total - used) : 0;
    mp_obj_t items[3] = {
        mp_obj_new_int_from_ull(freebytes),
        mp_obj_new_int_from_ull(total),
        mp_obj_new_int_from_uint((mp_uint_t)info.id_BytesPerBlock),
    };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_disk_info_obj, amiga_disk_info);

// Pattern-match buffer for the full path returned by MatchFirst/Next.
// Sized via AMIGA_PATH_MAX (mpconfigport.h) so the policy is shared
// with modasl.c and any future surface that builds full paths.
#define AMIGA_MATCH_BUFSIZE AMIGA_PATH_MAX

// Allocate a zeroed AnchorPath with `ap_Buf` space for the full path,
// pre-filling ap_Strlen so MatchFirst/Next will write into ap_Buf.
static struct AnchorPath *amiga_match_alloc_anchor(void) {
    struct AnchorPath *ap = AllocVec(
        sizeof(struct AnchorPath) + AMIGA_MATCH_BUFSIZE,
        MEMF_ANY | MEMF_CLEAR);
    if (ap != NULL) {
        ap->ap_Strlen = AMIGA_MATCH_BUFSIZE;
    }
    return ap;
}

// amiga.match(pattern) -> list[str] of full paths matching the AmigaDOS
// pattern. Patterns follow the shell's syntax (`#?` = any sequence,
// `?` = single char, `~(...)` = negate, `[a-z]` = class, etc.).
// Eager — materialises the full list before returning. Use amiga.imatch
// for memory-efficient iteration over large match sets.
static mp_obj_t amiga_match(mp_obj_t pattern_obj) {
    const char *pattern = mp_obj_str_get_str(pattern_obj);
    struct AnchorPath *ap = amiga_match_alloc_anchor();
    if (ap == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("AllocVec failed"));
    }
    mp_obj_t list = mp_obj_new_list(0, NULL);
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    LONG rc = MatchFirst((STRPTR)pattern, ap);
    while (rc == 0) {
        mp_obj_list_append(list,
            mp_obj_new_str((const char *)ap->ap_Buf, strlen((const char *)ap->ap_Buf)));
        rc = MatchNext(ap);
    }
    MatchEnd(ap);
    me->pr_WindowPtr = saved_wp;
    FreeVec(ap);
    if (rc != 0 && rc != ERROR_NO_MORE_ENTRIES && rc != ERROR_OBJECT_NOT_FOUND) {
        mp_raise_OSError(amiga_dos_errno_from(rc));
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_match_obj, amiga_match);

// amiga.imatch(pattern) -> iterator yielding full paths one at a time.
// Backed by a single AnchorPath that lives for the lifetime of the
// iterator; MatchEnd + FreeVec run when the iterator is exhausted, when
// the for-loop is exited (via a normal break / exception), or — as a
// safety net — when the iterator is GC'd. Holding many imatch iterators
// open simultaneously pins one AnchorPath + 512-byte buffer each.
typedef struct _amiga_match_it_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    mp_fun_1_t finaliser;
    struct AnchorPath *anchor;
    bool first;
    bool done;
} amiga_match_it_t;

static void amiga_match_cleanup(amiga_match_it_t *it) {
    if (it->anchor != NULL) {
        MatchEnd(it->anchor);
        FreeVec(it->anchor);
        it->anchor = NULL;
    }
}

static mp_obj_t amiga_match_iternext(mp_obj_t self_in) {
    amiga_match_it_t *it = MP_OBJ_TO_PTR(self_in);
    if (it->done || it->anchor == NULL) {
        return MP_OBJ_STOP_ITERATION;
    }
    if (it->first) {
        it->first = false;
    } else {
        struct Process *me = (struct Process *)FindTask(NULL);
        APTR saved_wp = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;
        LONG rc = MatchNext(it->anchor);
        me->pr_WindowPtr = saved_wp;
        if (rc != 0) {
            it->done = true;
            amiga_match_cleanup(it);
            return MP_OBJ_STOP_ITERATION;
        }
    }
    return mp_obj_new_str((const char *)it->anchor->ap_Buf,
        strlen((const char *)it->anchor->ap_Buf));
}

static mp_obj_t amiga_match_del(mp_obj_t self_in) {
    amiga_match_cleanup(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}

static mp_obj_t amiga_imatch(mp_obj_t pattern_obj) {
    const char *pattern = mp_obj_str_get_str(pattern_obj);
    struct AnchorPath *ap = amiga_match_alloc_anchor();
    if (ap == NULL) {
        mp_raise_msg(&mp_type_MemoryError, MP_ERROR_TEXT("AllocVec failed"));
    }
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    LONG rc = MatchFirst((STRPTR)pattern, ap);
    me->pr_WindowPtr = saved_wp;
    amiga_match_it_t *it = mp_obj_malloc_with_finaliser(
        amiga_match_it_t, &mp_type_polymorph_iter_with_finaliser);
    it->iternext = amiga_match_iternext;
    it->finaliser = amiga_match_del;
    it->anchor = ap;
    it->first = (rc == 0);
    it->done = (rc != 0);
    if (rc != 0) {
        MatchEnd(ap);
        FreeVec(ap);
        it->anchor = NULL;
        if (rc != ERROR_NO_MORE_ENTRIES && rc != ERROR_OBJECT_NOT_FOUND) {
            mp_raise_OSError(amiga_dos_errno_from(rc));
        }
    }
    return MP_OBJ_FROM_PTR(it);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_imatch_obj, amiga_imatch);

// ---------- Phase 17: generic AmigaOS library-call trampoline ----------

// Implemented in amiga_lib_call.S.  Loads A6 with `base`, D0-D7 / A0-A5
// from the explicit per-register slots, then JSRs to base+offset.  D0
// at return is the library function's result.
extern uint32_t amiga_lib_call_asm(
    uint32_t base, int32_t offset,
    uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
    uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7,
    uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
    uint32_t a4, uint32_t a5);

// amiga.lib_open(name, version=0) -> int base.
// Wraps `OpenLibrary("name.library", version)`.  Returns the library
// base as a Python int; raises OSError(ENOENT) if the library isn't
// available at the requested (or higher) version.  Callers must pair
// every successful open with amiga.lib_close.
static mp_obj_t amiga_lib_open(size_t n_args, const mp_obj_t *args) {
    const char *name = mp_obj_str_get_str(args[0]);
    ULONG version = (n_args >= 2) ? (ULONG)mp_obj_get_int(args[1]) : 0;
    struct Library *base = OpenLibrary((STRPTR)name, version);
    if (base == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    return mp_obj_new_int_from_uint((uintptr_t)base);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_lib_open_obj, 1, 2, amiga_lib_open);

// amiga.lib_close(base) -> None.  Tolerates a zero base (no-op) so
// callers can `lib_close(base)` unconditionally in a try/finally without
// having to remember whether the open succeeded.
static mp_obj_t amiga_lib_close(mp_obj_t base_obj) {
    struct Library *base = (struct Library *)(uintptr_t)mp_obj_get_int(base_obj);
    if (base != NULL) {
        CloseLibrary(base);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_lib_close_obj, amiga_lib_close);

// Table of (kwarg-name → register-slot-index) for the 14 user-loadable
// registers.  Matches the order of the regs[] array passed to the asm
// trampoline.
static const struct {
    qstr key;
    uint8_t idx;
} amiga_lib_call_regmap[] = {
    { MP_QSTR_d0, 0 },  { MP_QSTR_d1, 1 },  { MP_QSTR_d2, 2 },  { MP_QSTR_d3, 3 },
    { MP_QSTR_d4, 4 },  { MP_QSTR_d5, 5 },  { MP_QSTR_d6, 6 },  { MP_QSTR_d7, 7 },
    { MP_QSTR_a0, 8 },  { MP_QSTR_a1, 9 },  { MP_QSTR_a2, 10 }, { MP_QSTR_a3, 11 },
    { MP_QSTR_a4, 12 }, { MP_QSTR_a5, 13 },
};

// amiga.lib_call(base, offset, *, d0=0, ..., a5=0, ret="d0") -> value.
// Calls the library function at the given signed LVO offset (e.g. -96
// for DisplayBeep).  Registers not passed default to 0.  Return value
// interpretation:
//   ret="d0"   (default) — signed 32-bit int
//   ret="d0u"            — unsigned 32-bit int
//   ret="void"           — return None
// The trampoline (in amiga_lib_call.S) preserves the m68k SysV
// callee-saved register set, so the library is free to clobber any
// register per the AmigaOS ABI.
static mp_obj_t amiga_lib_call(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    (void)n_args;
    uint32_t base = (uint32_t)(uintptr_t)mp_obj_get_int(args[0]);
    int32_t offset = (int32_t)mp_obj_get_int(args[1]);
    uint32_t regs[14] = {0};
    const char *ret_mode = "d0";

    for (size_t i = 0; i < kw_args->alloc; i++) {
        mp_map_elem_t *e = &kw_args->table[i];
        if (e->key == MP_OBJ_NULL) {
            continue;
        }
        bool matched = false;
        for (size_t r = 0; r < MP_ARRAY_SIZE(amiga_lib_call_regmap); r++) {
            if (e->key == MP_OBJ_NEW_QSTR(amiga_lib_call_regmap[r].key)) {
                regs[amiga_lib_call_regmap[r].idx] =
                    (uint32_t)mp_obj_get_int_truncated(e->value);
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }
        if (e->key == MP_OBJ_NEW_QSTR(MP_QSTR_ret)) {
            ret_mode = mp_obj_str_get_str(e->value);
            continue;
        }
        mp_raise_TypeError(MP_ERROR_TEXT("lib_call: unknown keyword"));
    }

    uint32_t result = amiga_lib_call_asm(
        base, offset,
        regs[0], regs[1], regs[2], regs[3],
        regs[4], regs[5], regs[6], regs[7],
        regs[8], regs[9], regs[10], regs[11],
        regs[12], regs[13]);

    if (strcmp(ret_mode, "void") == 0) {
        return mp_const_none;
    }
    if (strcmp(ret_mode, "d0u") == 0) {
        return mp_obj_new_int_from_uint(result);
    }
    return mp_obj_new_int((mp_int_t)(int32_t)result);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(amiga_lib_call_obj, 2, amiga_lib_call);

// ---------- Phase 17 step 4: memory peek/poke primitives ----------
//
// These are the minimum surface needed for the Python-side TagList
// helper (which assembles a `struct TagItem[]` from kwargs) and for
// users who want to inspect or fill structs by hand.  All operations
// are big-endian 32-/16-bit and 8-bit — matching the m68k native
// representation; no endian swapping is needed.  The user is
// responsible for the address validity.

static mp_obj_t amiga_peek_b(mp_obj_t addr_obj) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    return MP_OBJ_NEW_SMALL_INT(*p);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_peek_b_obj, amiga_peek_b);

static mp_obj_t amiga_peek_w(mp_obj_t addr_obj) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    return MP_OBJ_NEW_SMALL_INT(*p);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_peek_w_obj, amiga_peek_w);

static mp_obj_t amiga_peek_l(mp_obj_t addr_obj) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    return mp_obj_new_int_from_uint(*p);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_peek_l_obj, amiga_peek_l);

// amiga.peek_bytes(addr, length) -> bytes
static mp_obj_t amiga_peek_bytes(mp_obj_t addr_obj, mp_obj_t len_obj) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    mp_int_t n = mp_obj_get_int(len_obj);
    if (n < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("negative length"));
    }
    return mp_obj_new_bytes(p, (size_t)n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_peek_bytes_obj, amiga_peek_bytes);

static mp_obj_t amiga_poke_b(mp_obj_t addr_obj, mp_obj_t val_obj) {
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    *p = (uint8_t)mp_obj_get_int_truncated(val_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_poke_b_obj, amiga_poke_b);

static mp_obj_t amiga_poke_w(mp_obj_t addr_obj, mp_obj_t val_obj) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    *p = (uint16_t)mp_obj_get_int_truncated(val_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_poke_w_obj, amiga_poke_w);

static mp_obj_t amiga_poke_l(mp_obj_t addr_obj, mp_obj_t val_obj) {
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    *p = (uint32_t)mp_obj_get_int_truncated(val_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_poke_l_obj, amiga_poke_l);

// amiga.poke_bytes(addr, data) — memcpy data (bytes/str/buffer) at addr.
// No NUL is appended automatically; for C strings the caller must
// include a trailing 0 byte.
static mp_obj_t amiga_poke_bytes(mp_obj_t addr_obj, mp_obj_t data_obj) {
    uint8_t *p = (uint8_t *)(uintptr_t)mp_obj_get_int(addr_obj);
    mp_buffer_info_t bi;
    mp_get_buffer_raise(data_obj, &bi, MP_BUFFER_READ);
    memcpy(p, bi.buf, bi.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_poke_bytes_obj, amiga_poke_bytes);

// ---------- Phase 25: break-signal IPC ----------
//
// AmigaOS gives every task four user-defined break signals
// (SIGBREAKF_CTRL_C/D/E/F).  Ctrl+C is already handled by the port to
// raise KeyboardInterrupt; the rest are exposed here as a cheap
// cooperative-IPC primitive on top of exec.library Signal() / Wait().
//
// amiga.signal(task_addr, sigmask) — wakes another task with the given
//   signal bits (or even the current task, for a self-test).
// amiga.wait_signal(sigmask, timeout_ms=None) — blocks until any of
//   `sigmask` arrives.  SIGBREAKF_CTRL_C is always implicitly ORed in
//   so the user can break out of an otherwise-infinite wait; if it
//   fires, KeyboardInterrupt is raised instead of being returned.  A
//   non-None `timeout_ms` arms an async timer.device request alongside
//   the Wait(); returns 0 if the timeout expires before any user
//   signal arrives.

extern ULONG amiga_async_timer_send(ULONG ms);
extern void  amiga_async_timer_abort(void);

static mp_obj_t amiga_signal(mp_obj_t task_obj, mp_obj_t mask_obj) {
    struct Task *task = (struct Task *)(uintptr_t)mp_obj_get_int(task_obj);
    ULONG mask = (ULONG)mp_obj_get_int_truncated(mask_obj);
    if (task == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("task pointer is NULL"));
    }
    Signal(task, mask);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_signal_obj, amiga_signal);

static mp_obj_t amiga_wait_signal(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mask,       MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_timeout_ms, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, arg_vals);
    ULONG user_mask = (ULONG)arg_vals[0].u_int;
    mp_obj_t timeout_obj = arg_vals[1].u_obj;
    bool has_timeout = (timeout_obj != mp_const_none);

    // SIGBREAKF_CTRL_C is reserved — the caller can't suppress it.  Mask
    // it out of the user request so the returned value (`got & user_mask`)
    // never includes it; we still listen for it via `full_mask` below
    // and raise KeyboardInterrupt if it fires.
    user_mask &= ~SIGBREAKF_CTRL_C;
    ULONG full_mask = user_mask | SIGBREAKF_CTRL_C;
    ULONG timer_sig = 0;

    if (has_timeout) {
        mp_int_t ms = mp_obj_get_int(timeout_obj);
        if (ms < 0) {
            ms = 0;
        }
        timer_sig = amiga_async_timer_send((ULONG)ms);
        if (timer_sig != 0) {
            full_mask |= timer_sig;
        }
        // If timer setup failed (timer_sig == 0) we fall through to an
        // untimed wait; documented best-effort behaviour.
    }

    ULONG got = Wait(full_mask);

    if (timer_sig != 0) {
        amiga_async_timer_abort();
    }
    if (got & SIGBREAKF_CTRL_C) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    return mp_obj_new_int_from_uint(got & user_mask);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(amiga_wait_signal_obj, 1, amiga_wait_signal);

// ---------- Phase 18 (inbound): public ARexx port ----------
//
// AmigaOS apps expose a public MsgPort named "<APPNAME>.<N>" for ARexx
// scripts to drive.  This block sets up one for the MicroPython VM so
// external `rx "address MICROPYTHON.1; <python expr>"` invocations can
// reach us.  Outbound (the `amiga.rexx()` client) is deferred.
//
// `amiga.rexx_open(stem="MICROPYTHON")` finds the lowest free `.N`
// suffix (1-16), creates a public MsgPort with that name, and returns
// the assigned name.  `amiga.rexx_recv(timeout_ms=None)` waits for an
// incoming RexxMsg (with Ctrl+C and an optional timer.device timeout
// ORed in) and returns its address as a Python int.  The Python-side
// `RexxMessage` wrapper (in `amiga.py`) reads `rm_Args[0]` for the
// command and calls `amiga.rexx_reply(msg, rc, result, secondary)` to
// fill `rm_Result1/2` and ReplyMsg.  Result strings (non-NULL with
// rc=0) are wrapped via `rexxsyslib.library` `CreateArgstring`; the
// receiver is expected to `DeleteArgstring` after consuming.

// Bebbo's proto/rexxsyslib.h declares this extern; we own the
// definition.  Opened lazily on first reply with a result string and
// closed in amiga_rexx_close().
struct RxsLib *RexxSysBase = NULL;

static struct MsgPort *amiga_rexx_port = NULL;
static char            amiga_rexx_port_name[40];

// Phase 32 persistent ARexx clients (definitions below near the
// client primitives); kept here so amiga_rexx_shutdown() -- which
// walks the array on process exit -- sees them.
#define AMIGA_REXX_CLIENT_MAX 16
static struct MsgPort *amiga_rexx_client_ports[AMIGA_REXX_CLIENT_MAX];

void amiga_rexx_shutdown(void);  // forward-declared for main.c cleanup

// amiga.rexx_open(stem="MICROPYTHON") -> str of the assigned port name.
static mp_obj_t amiga_rexx_open_fn(size_t n_args, const mp_obj_t *args) {
    if (amiga_rexx_port != NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("rexx port already open"));
    }
    const char *stem = (n_args >= 1)
        ? mp_obj_str_get_str(args[0]) : "MICROPYTHON";

    // Forbid()/Permit() so a concurrent task can't sneak in between our
    // FindPort and AddPort and claim the same name.
    int chosen = 0;
    Forbid();
    for (int i = 1; i <= 16; i++) {
        int n = snprintf(amiga_rexx_port_name,
            sizeof(amiga_rexx_port_name), "%s.%d", stem, i);
        if (n <= 0 || (size_t)n >= sizeof(amiga_rexx_port_name)) {
            continue;
        }
        if (FindPort((CONST_STRPTR)amiga_rexx_port_name) == NULL) {
            amiga_rexx_port = CreateMsgPort();
            if (amiga_rexx_port == NULL) {
                amiga_rexx_port_name[0] = '\0';
                Permit();
                mp_raise_msg(&mp_type_OSError,
                    MP_ERROR_TEXT("CreateMsgPort failed"));
            }
            amiga_rexx_port->mp_Node.ln_Name = (STRPTR)amiga_rexx_port_name;
            amiga_rexx_port->mp_Node.ln_Pri  = 0;
            AddPort(amiga_rexx_port);
            chosen = i;
            break;
        }
    }
    Permit();
    if (chosen == 0) {
        amiga_rexx_port_name[0] = '\0';
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("no free MICROPYTHON.N port slot"));
    }
    return mp_obj_new_str(amiga_rexx_port_name,
        strlen(amiga_rexx_port_name));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_rexx_open_obj, 0, 1, amiga_rexx_open_fn);

// Drain pending messages and tear down the port.  Safe to call any
// number of times.  Replies anything still in-flight with rc=20 ("FAIL")
// so the sender doesn't hang.
static mp_obj_t amiga_rexx_close_fn(void) {
    amiga_rexx_shutdown();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_rexx_close_obj, amiga_rexx_close_fn);

void amiga_rexx_shutdown(void) {
    // Tear down any RexxClient reply ports the user forgot to close.
    // The MsgPort is private to the client (no inbound messages
    // queue there) so DeleteMsgPort is sufficient -- no need to
    // drain/reply.
    for (int i = 0; i < AMIGA_REXX_CLIENT_MAX; i++) {
        if (amiga_rexx_client_ports[i] != NULL) {
            DeleteMsgPort(amiga_rexx_client_ports[i]);
            amiga_rexx_client_ports[i] = NULL;
        }
    }
    if (amiga_rexx_port == NULL) {
        if (RexxSysBase != NULL) {
            CloseLibrary((struct Library *)RexxSysBase);
            RexxSysBase = NULL;
        }
        return;
    }
    RemPort(amiga_rexx_port);
    struct RexxMsg *msg;
    while ((msg = (struct RexxMsg *)GetMsg(amiga_rexx_port)) != NULL) {
        msg->rm_Result1 = 20;
        msg->rm_Result2 = 0;
        ReplyMsg(&msg->rm_Node);
    }
    DeleteMsgPort(amiga_rexx_port);
    amiga_rexx_port = NULL;
    amiga_rexx_port_name[0] = '\0';
    if (RexxSysBase != NULL) {
        CloseLibrary((struct Library *)RexxSysBase);
        RexxSysBase = NULL;
    }
}

// amiga.rexx_port_name() -> str or None
static mp_obj_t amiga_rexx_port_name_fn(void) {
    if (amiga_rexx_port == NULL || amiga_rexx_port_name[0] == '\0') {
        return mp_const_none;
    }
    return mp_obj_new_str(amiga_rexx_port_name,
        strlen(amiga_rexx_port_name));
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_rexx_port_name_obj, amiga_rexx_port_name_fn);

// amiga.rexx_recv(timeout_ms=None) -> int (msg ptr) or None on timeout.
// Raises KeyboardInterrupt if Ctrl+C fires during the wait.
static mp_obj_t amiga_rexx_recv_fn(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout_ms, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, arg_vals);

    if (amiga_rexx_port == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("rexx port not open"));
    }

    struct RexxMsg *msg = (struct RexxMsg *)GetMsg(amiga_rexx_port);
    if (msg != NULL) {
        return mp_obj_new_int_from_uint((uintptr_t)msg);
    }

    mp_obj_t timeout_obj = arg_vals[0].u_obj;
    bool has_timeout = (timeout_obj != mp_const_none);
    ULONG port_sig  = 1UL << amiga_rexx_port->mp_SigBit;
    ULONG full_mask = port_sig | SIGBREAKF_CTRL_C;
    ULONG timer_sig = 0;

    if (has_timeout) {
        mp_int_t ms = mp_obj_get_int(timeout_obj);
        if (ms < 0) {
            ms = 0;
        }
        timer_sig = amiga_async_timer_send((ULONG)ms);
        if (timer_sig != 0) {
            full_mask |= timer_sig;
        }
    }
    ULONG got = Wait(full_mask);
    if (timer_sig != 0) {
        amiga_async_timer_abort();
    }
    if (got & SIGBREAKF_CTRL_C) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    msg = (struct RexxMsg *)GetMsg(amiga_rexx_port);
    if (msg == NULL) {
        return mp_const_none;
    }
    return mp_obj_new_int_from_uint((uintptr_t)msg);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(amiga_rexx_recv_obj, 0, amiga_rexx_recv_fn);

// amiga.rexx_command(msg_ptr) -> bytes of the ARG0 command string.
static mp_obj_t amiga_rexx_command_fn(mp_obj_t msg_obj) {
    struct RexxMsg *msg = (struct RexxMsg *)(uintptr_t)mp_obj_get_int(msg_obj);
    const char *cmd = (const char *)msg->rm_Args[0];
    if (cmd == NULL) {
        return mp_obj_new_bytes((const byte *)"", 0);
    }
    return mp_obj_new_bytes((const byte *)cmd, strlen(cmd));
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_rexx_command_obj, amiga_rexx_command_fn);

// amiga.rexx_reply(msg_ptr, rc, result_or_None, secondary=0) -> None.
// rc=0 with a non-None result wraps the result via CreateArgstring (the
// receiver is expected to DeleteArgstring after consuming).  rc!=0 puts
// `secondary` in rm_Result2 as the conventional error-code slot.
static mp_obj_t amiga_rexx_reply_fn(size_t n_args, const mp_obj_t *args) {
    struct RexxMsg *msg = (struct RexxMsg *)(uintptr_t)mp_obj_get_int(args[0]);
    LONG rc = (LONG)mp_obj_get_int(args[1]);
    mp_obj_t result_obj = args[2];
    LONG secondary = (n_args >= 4) ? (LONG)mp_obj_get_int(args[3]) : 0;

    msg->rm_Result1 = rc;
    msg->rm_Result2 = secondary;

    if (rc == 0 && result_obj != mp_const_none) {
        if (RexxSysBase == NULL) {
            RexxSysBase = (struct RxsLib *)OpenLibrary(
                (CONST_STRPTR)"rexxsyslib.library", 0);
        }
        if (RexxSysBase == NULL) {
            // We're past the point of no return — the message was
            // delivered to us and the sender is blocked on a reply.
            // Reply with a meaningful error rather than silently
            // dropping the result string.
            msg->rm_Result1 = 10;
            msg->rm_Result2 = 0;
            ReplyMsg(&msg->rm_Node);
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("rexxsyslib.library unavailable"));
        }
        mp_buffer_info_t bi;
        mp_get_buffer_raise(result_obj, &bi, MP_BUFFER_READ);
        UBYTE *argstr = CreateArgstring(
            (CONST_STRPTR)bi.buf, (ULONG)bi.len);
        if (argstr != NULL) {
            msg->rm_Result2 = (LONG)(uintptr_t)argstr;
        }
    }
    ReplyMsg(&msg->rm_Node);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_rexx_reply_obj, 3, 4, amiga_rexx_reply_fn);

// ---------- Phase 18 outbound: drive another app's ARexx port ----------
//
// FindPort(host) → CreateRexxMsg referencing a reply port →
// CreateArgstring for the command → PutMsg to the target → Wait for
// the reply on the reply port (with Ctrl+C latched but deferred so
// we don't orphan the message in the host's queue) → unpack
// rm_Result1/2 and clean up.
//
// Returns a `(rc, result_or_None)` tuple.  `result` is the bytes of
// the argstring at rm_Result2 when rc==0 and the host returned a
// string; otherwise None (rc!=0 leaves rm_Result2 as the host's
// secondary error code, which the Python wrapper ignores for now).
//
// Ctrl+C handling: we never abort the wait once PutMsg has happened —
// abandoning the reply port mid-flight would crash the host as it
// PutMsg's into freed memory.  Instead we latch the Ctrl+C bit and
// raise KeyboardInterrupt only after the reply is in hand and the
// cleanup has run.  Most ARexx hosts reply within milliseconds, so
// the user-visible delay is minimal.

static bool amiga_rexx_ensure_rexxsys(void) {
    if (RexxSysBase == NULL) {
        RexxSysBase = (struct RxsLib *)OpenLibrary(
            (CONST_STRPTR)"rexxsyslib.library", 0);
        if (RexxSysBase == NULL) {
            mp_raise_msg(&mp_type_OSError,
                MP_ERROR_TEXT("rexxsyslib.library unavailable"));
            return false;
        }
    }
    return true;
}

// Core send: takes an already-open reply MsgPort and runs the full
// CreateRexxMsg → CreateArgstring → PutMsg → Wait → cleanup dance
// against it.  Shared by `amiga_rexx_send_fn` (which owns its reply
// port for the duration of the call) and Phase 32's persistent
// RexxClient C primitives (which reuse the same reply port across
// many sends).
static mp_obj_t amiga_rexx_send_via_port(struct MsgPort *reply,
                                         const char *host,
                                         const mp_buffer_info_t *cmd_bi) {
    struct MsgPort *target = FindPort((CONST_STRPTR)host);
    if (target == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    if (!amiga_rexx_ensure_rexxsys()) {
        return mp_const_none;  // unreachable; ensure_rexxsys raised
    }

    struct RexxMsg *msg = CreateRexxMsg(reply, NULL, NULL);
    if (msg == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("CreateRexxMsg failed"));
    }
    msg->rm_Args[0] = (STRPTR)CreateArgstring(
        (CONST_STRPTR)cmd_bi->buf, (ULONG)cmd_bi->len);
    if (msg->rm_Args[0] == NULL) {
        DeleteRexxMsg(msg);
        mp_raise_msg(&mp_type_MemoryError,
            MP_ERROR_TEXT("CreateArgstring failed"));
    }
    // RXCOMM = a command-level invocation; RXFF_RESULT = ask the host
    // to put a result string in rm_Result2.  RXFF_STRING is for
    // function-style calls, not relevant here.
    msg->rm_Action = RXCOMM | RXFF_RESULT;

    PutMsg(target, &msg->rm_Node);

    // Wait for the reply.  Ctrl+C latched but deferred.
    bool ctrl_c_pending = false;
    ULONG reply_sig = 1UL << reply->mp_SigBit;
    struct RexxMsg *got_msg = NULL;
    while (got_msg == NULL) {
        ULONG got = Wait(reply_sig | SIGBREAKF_CTRL_C);
        if (got & SIGBREAKF_CTRL_C) {
            ctrl_c_pending = true;
        }
        if (got & reply_sig) {
            got_msg = (struct RexxMsg *)GetMsg(reply);
        }
    }

    LONG rc = msg->rm_Result1;
    mp_obj_t result_obj = mp_const_none;
    if (rc == 0 && msg->rm_Result2 != 0) {
        UBYTE *argstr = (UBYTE *)(uintptr_t)msg->rm_Result2;
        ULONG len = LengthArgstring(argstr);
        result_obj = mp_obj_new_bytes((const byte *)argstr, (size_t)len);
        DeleteArgstring(argstr);
    }
    DeleteArgstring((UBYTE *)msg->rm_Args[0]);
    msg->rm_Args[0] = NULL;
    DeleteRexxMsg(msg);

    if (ctrl_c_pending) {
        mp_raise_type(&mp_type_KeyboardInterrupt);
    }
    mp_obj_t pair[2] = {
        mp_obj_new_int(rc),
        result_obj,
    };
    return mp_obj_new_tuple(2, pair);
}

static mp_obj_t amiga_rexx_send_fn(mp_obj_t port_obj, mp_obj_t cmd_obj) {
    const char *port_name = mp_obj_str_get_str(port_obj);
    mp_buffer_info_t cmd_bi;
    mp_get_buffer_raise(cmd_obj, &cmd_bi, MP_BUFFER_READ);

    struct MsgPort *reply = CreateMsgPort();
    if (reply == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("CreateMsgPort failed"));
    }

    mp_obj_t result;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        result = amiga_rexx_send_via_port(reply, port_name, &cmd_bi);
        nlr_pop();
    } else {
        // Make sure the reply port is freed before re-raising; the
        // helper has already cleaned up its own RexxMsg/argstring
        // before raising in any of its error paths.
        DeleteMsgPort(reply);
        nlr_jump(nlr.ret_val);
    }
    DeleteMsgPort(reply);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_2(amiga_rexx_send_obj, amiga_rexx_send_fn);

// ---------- Phase 32: persistent ARexx client ----------
//
// `RexxClient` (Python) holds an open reply MsgPort across many
// sends. Driving a host (DOpus, IBrowse, YAM, ...) in a tight loop
// thus avoids paying CreateMsgPort/DeleteMsgPort per call.
//
// Bookkeeping: every open client port is registered in a small
// static array (declared near amiga_rexx_shutdown above; 16 slots,
// far more than any realistic use would open simultaneously).
// amiga_rexx_shutdown() walks the array on process exit and
// DeleteMsgPort's anything still live, so a script that forgot
// to call .close() doesn't leak the port.

static void amiga_rexx_client_register(struct MsgPort *port) {
    for (int i = 0; i < AMIGA_REXX_CLIENT_MAX; i++) {
        if (amiga_rexx_client_ports[i] == NULL) {
            amiga_rexx_client_ports[i] = port;
            return;
        }
    }
    // Array exhausted -- shouldn't happen in practice; drop the
    // registration silently. The MsgPort itself is still usable;
    // the only consequence is that a forgotten close on this
    // particular instance would leak it on process exit.
}

static void amiga_rexx_client_unregister(struct MsgPort *port) {
    for (int i = 0; i < AMIGA_REXX_CLIENT_MAX; i++) {
        if (amiga_rexx_client_ports[i] == port) {
            amiga_rexx_client_ports[i] = NULL;
            return;
        }
    }
}

// amiga.rexx_client_open() -> int (handle = MsgPort *)
static mp_obj_t amiga_rexx_client_open_fn(void) {
    if (!amiga_rexx_ensure_rexxsys()) {
        return mp_const_none;  // unreachable
    }
    struct MsgPort *reply = CreateMsgPort();
    if (reply == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("CreateMsgPort failed"));
    }
    amiga_rexx_client_register(reply);
    return mp_obj_new_int_from_uint((uintptr_t)reply);
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_rexx_client_open_obj,
    amiga_rexx_client_open_fn);

// amiga.rexx_client_close(handle) -> None. Tolerates a zero handle
// so Python __del__ paths can be unconditional.
static mp_obj_t amiga_rexx_client_close_fn(mp_obj_t handle_obj) {
    struct MsgPort *reply = (struct MsgPort *)(uintptr_t)
        mp_obj_get_int(handle_obj);
    if (reply == NULL) {
        return mp_const_none;
    }
    amiga_rexx_client_unregister(reply);
    DeleteMsgPort(reply);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_rexx_client_close_obj,
    amiga_rexx_client_close_fn);

// amiga.rexx_client_send(handle, host, cmd) -> (rc, result_or_None).
// Reuses the reply port at `handle`; otherwise identical to
// amiga.rexx_send.
static mp_obj_t amiga_rexx_client_send_fn(size_t n_args,
                                          const mp_obj_t *args) {
    (void)n_args;
    struct MsgPort *reply = (struct MsgPort *)(uintptr_t)
        mp_obj_get_int(args[0]);
    if (reply == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("RexxClient: closed"));
    }
    const char *host = mp_obj_str_get_str(args[1]);
    mp_buffer_info_t cmd_bi;
    mp_get_buffer_raise(args[2], &cmd_bi, MP_BUFFER_READ);
    return amiga_rexx_send_via_port(reply, host, &cmd_bi);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_rexx_client_send_obj,
    3, 3, amiga_rexx_client_send_fn);

// ---------- Phase 32: ARexx polish (rexx_exists, rexx_list) ----------

// amiga.rexx_exists(name) -> bool. Light-weight wrapper around
// FindPort, fenced with Forbid/Permit so the port list can't
// mutate between FindPort returning a stale entry and the caller
// acting on the result.
static mp_obj_t amiga_rexx_exists_fn(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    Forbid();
    bool ok = (FindPort((CONST_STRPTR)name) != NULL);
    Permit();
    return mp_obj_new_bool(ok);
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_rexx_exists_obj, amiga_rexx_exists_fn);

// amiga.rexx_list() -> list[str] of every public MsgPort's
// ln_Name. Walks SysBase->PortList under Forbid so the list can't
// mutate mid-walk. Nodes with NULL ln_Name (rare; not strictly
// required to have one) are skipped silently. Allocating Python
// objects inside a Forbid window matches the precedent in
// amiga_rexx_open_fn (Phase 18) -- mp_obj_new_str / list_append
// don't issue Forbid-unsafe calls.
static mp_obj_t amiga_rexx_list_fn(void) {
    mp_obj_t list = mp_obj_new_list(0, NULL);
    Forbid();
    struct List *plist = &SysBase->PortList;
    struct Node *n;
    for (n = plist->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ) {
        if (n->ln_Name != NULL) {
            mp_obj_list_append(list,
                mp_obj_new_str(n->ln_Name, strlen(n->ln_Name)));
        }
    }
    Permit();
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_rexx_list_obj, amiga_rexx_list_fn);

// ---------- Phase 24: REPL history accessors ----------
//
// The readline history ring lives in `MP_STATE_PORT(readline_hist)`
// and is normally only touched by `shared/readline/readline.c`.
// Exposing two thin accessors makes it scriptable: a startup script
// can pre-seed entries, and inspection is useful for "what did I
// just type?" recovery.

#include "shared/readline/readline.h"

// amiga.readline_history() -> tuple[str, ...]  (most recent first)
static mp_obj_t amiga_readline_history(void) {
    size_t n = 0;
    for (size_t i = 0; i < MICROPY_READLINE_HISTORY_SIZE; i++) {
        if (MP_STATE_PORT(readline_hist)[i] != NULL) {
            n++;
        }
    }
    mp_obj_t tup = mp_obj_new_tuple(n, NULL);
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(tup);
    size_t w = 0;
    for (size_t i = 0; i < MICROPY_READLINE_HISTORY_SIZE && w < n; i++) {
        const char *entry = MP_STATE_PORT(readline_hist)[i];
        if (entry != NULL) {
            t->items[w++] = mp_obj_new_str(entry, strlen(entry));
        }
    }
    return tup;
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_readline_history_obj, amiga_readline_history);

// amiga.readline_push_history(line)
static mp_obj_t amiga_readline_push_history_fn(mp_obj_t line_obj) {
    const char *line = mp_obj_str_get_str(line_obj);
    readline_push_history(line);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_readline_push_history_obj, amiga_readline_push_history_fn);

static const mp_rom_map_elem_t amiga_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR__amiga) },
    { MP_ROM_QSTR(MP_QSTR_os_version),  MP_ROM_PTR(&amiga_os_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_find_task),   MP_ROM_PTR(&amiga_find_task_obj) },
    { MP_ROM_QSTR(MP_QSTR_alloc_vec),   MP_ROM_PTR(&amiga_alloc_vec_obj) },
    { MP_ROM_QSTR(MP_QSTR_free_vec),    MP_ROM_PTR(&amiga_free_vec_obj) },
    { MP_ROM_QSTR(MP_QSTR_execute),     MP_ROM_PTR(&amiga_execute_obj) },
    { MP_ROM_QSTR(MP_QSTR_exists),      MP_ROM_PTR(&amiga_exists_obj) },
    { MP_ROM_QSTR(MP_QSTR_heap_info),   MP_ROM_PTR(&amiga_heap_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_launched_from_workbench),
      MP_ROM_PTR(&amiga_launched_from_workbench_obj) },
    { MP_ROM_QSTR(MP_QSTR_wb_selected_files),
      MP_ROM_PTR(&amiga_wb_selected_files_obj) },
    { MP_ROM_QSTR(MP_QSTR_tooltype),    MP_ROM_PTR(&amiga_tooltype_obj) },
    { MP_ROM_QSTR(MP_QSTR_volumes),     MP_ROM_PTR(&amiga_volumes_obj) },
    { MP_ROM_QSTR(MP_QSTR_assigns),     MP_ROM_PTR(&amiga_assigns_obj) },
    { MP_ROM_QSTR(MP_QSTR_disk_info),   MP_ROM_PTR(&amiga_disk_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_match),       MP_ROM_PTR(&amiga_match_obj) },
    { MP_ROM_QSTR(MP_QSTR_imatch),      MP_ROM_PTR(&amiga_imatch_obj) },
    { MP_ROM_QSTR(MP_QSTR_lib_open),    MP_ROM_PTR(&amiga_lib_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_lib_close),   MP_ROM_PTR(&amiga_lib_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_lib_call),    MP_ROM_PTR(&amiga_lib_call_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek_b),      MP_ROM_PTR(&amiga_peek_b_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek_w),      MP_ROM_PTR(&amiga_peek_w_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek_l),      MP_ROM_PTR(&amiga_peek_l_obj) },
    { MP_ROM_QSTR(MP_QSTR_peek_bytes),  MP_ROM_PTR(&amiga_peek_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke_b),      MP_ROM_PTR(&amiga_poke_b_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke_w),      MP_ROM_PTR(&amiga_poke_w_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke_l),      MP_ROM_PTR(&amiga_poke_l_obj) },
    { MP_ROM_QSTR(MP_QSTR_poke_bytes),  MP_ROM_PTR(&amiga_poke_bytes_obj) },
    { MP_ROM_QSTR(MP_QSTR_signal),      MP_ROM_PTR(&amiga_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_signal), MP_ROM_PTR(&amiga_wait_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_open),       MP_ROM_PTR(&amiga_rexx_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_close),      MP_ROM_PTR(&amiga_rexx_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_port_name),  MP_ROM_PTR(&amiga_rexx_port_name_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_recv),       MP_ROM_PTR(&amiga_rexx_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_command),    MP_ROM_PTR(&amiga_rexx_command_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_reply),      MP_ROM_PTR(&amiga_rexx_reply_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_send),       MP_ROM_PTR(&amiga_rexx_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_exists),       MP_ROM_PTR(&amiga_rexx_exists_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_list),         MP_ROM_PTR(&amiga_rexx_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_client_open),  MP_ROM_PTR(&amiga_rexx_client_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_client_close), MP_ROM_PTR(&amiga_rexx_client_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_rexx_client_send),  MP_ROM_PTR(&amiga_rexx_client_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline_history),      MP_ROM_PTR(&amiga_readline_history_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline_push_history), MP_ROM_PTR(&amiga_readline_push_history_obj) },
    // Break-signal bits (Phase 25)
    { MP_ROM_QSTR(MP_QSTR_SIGBREAKF_CTRL_C), MP_ROM_INT(SIGBREAKF_CTRL_C) },
    { MP_ROM_QSTR(MP_QSTR_SIGBREAKF_CTRL_D), MP_ROM_INT(SIGBREAKF_CTRL_D) },
    { MP_ROM_QSTR(MP_QSTR_SIGBREAKF_CTRL_E), MP_ROM_INT(SIGBREAKF_CTRL_E) },
    { MP_ROM_QSTR(MP_QSTR_SIGBREAKF_CTRL_F), MP_ROM_INT(SIGBREAKF_CTRL_F) },
    // Memory flags
    { MP_ROM_QSTR(MP_QSTR_MEMF_ANY),    MP_ROM_INT(MEMF_ANY) },
    { MP_ROM_QSTR(MP_QSTR_MEMF_PUBLIC), MP_ROM_INT(MEMF_PUBLIC) },
    { MP_ROM_QSTR(MP_QSTR_MEMF_CHIP),   MP_ROM_INT(MEMF_CHIP) },
    { MP_ROM_QSTR(MP_QSTR_MEMF_FAST),   MP_ROM_INT(MEMF_FAST) },
    { MP_ROM_QSTR(MP_QSTR_MEMF_CLEAR),  MP_ROM_INT(MEMF_CLEAR) },
};
static MP_DEFINE_CONST_DICT(amiga_module_globals, amiga_module_globals_table);

const mp_obj_module_t amiga_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&amiga_module_globals,
};

// Registered as `_amiga` (underscore prefix) so the frozen `amiga.py`
// in ports/amiga/modules/ can do `from _amiga import *` and add the
// Python-side Library proxy on top (Phase 17 step 3).
MP_REGISTER_MODULE(MP_QSTR__amiga, amiga_module);

#endif // MICROPY_PY_AMIGA
