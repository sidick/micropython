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
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dosasl.h>
#include <proto/exec.h>
#include <proto/dos.h>
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
    char path[512];
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
// 512 bytes is generous: AmigaDOS volume names are <=30 chars and
// typical nesting depth is shallow.
#define AMIGA_MATCH_BUFSIZE 512

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

static const mp_rom_map_elem_t amiga_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_amiga) },
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

MP_REGISTER_MODULE(MP_QSTR_amiga, amiga_module);

#endif // MICROPY_PY_AMIGA
