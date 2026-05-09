#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>

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

static const mp_rom_map_elem_t amiga_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),    MP_ROM_QSTR(MP_QSTR_amiga) },
    { MP_ROM_QSTR(MP_QSTR_os_version),  MP_ROM_PTR(&amiga_os_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_find_task),   MP_ROM_PTR(&amiga_find_task_obj) },
    { MP_ROM_QSTR(MP_QSTR_alloc_vec),   MP_ROM_PTR(&amiga_alloc_vec_obj) },
    { MP_ROM_QSTR(MP_QSTR_free_vec),    MP_ROM_PTR(&amiga_free_vec_obj) },
    { MP_ROM_QSTR(MP_QSTR_execute),     MP_ROM_PTR(&amiga_execute_obj) },
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
