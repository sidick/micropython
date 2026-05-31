// Phase 31: thin wrapper around asl.library's file requester.
//
// Step 1 exposes the single-pick form:
//
//   _asl.file_request(title="", initial_drawer="", initial_file="",
//                     pattern="") -> str | None
//
// Returns the absolute path of the chosen file as a Python string,
// or None if the user cancelled.
//
// Future steps (per docs/phase31-asl-plan.md) add save=, drawers_only=
// and multi= kwargs.
//
// asl.library is system-wide on AmigaOS 2.0+; the module opens it
// lazily on first call. The dialog is fully modal and renders on the
// default public screen (Workbench, or asl.library opens its own).

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <proto/exec.h>
#include <proto/asl.h>
#include <proto/dos.h>

#if MICROPY_PY_AMIGA

// proto/asl.h declares this extern; we own the definition. Lazy open,
// no explicit close (system-wide library, AmigaOS reaps on exit).
struct Library *AslBase = NULL;

static void asl_ensure_open(void) {
    if (AslBase == NULL) {
        AslBase = OpenLibrary((CONST_STRPTR)"asl.library", 36);
        if (AslBase == NULL) {
            mp_raise_OSError(MP_ENOENT);
        }
    }
}

void amiga_asl_close(void) {
    if (AslBase != NULL) {
        CloseLibrary(AslBase);
        AslBase = NULL;
    }
}

// Path buffer policy: see AMIGA_PATH_MAX in mpconfigport.h. Shared
// across modasl.c / modamiga.c so a future bump is a single edit.

// asl.library is notoriously stack-hungry: the file requester loads
// directory listings, font code, etc. and can easily blow a default
// 4 KB shell stack. 32 KB is the documented safe size for
// AllocAslRequest + AslRequest combined.
#define ASL_SWAP_STACK_BYTES (32 * 1024)

// Context passed across the StackSwap boundary. The struct itself
// lives on the original (caller's) stack but is accessed from the
// scratch stack while the ASL work is running -- both stacks remain
// valid memory throughout, only the SP register changes.
struct asl_swap_ctx {
    struct TagItem        *tags;
    struct FileRequester  *req;
    BOOL                   ok;
};

// Runs entirely on the scratch stack. Keeps to AllocAslRequest +
// AslRequest -- no MicroPython API calls -- so the GC's stack-scan
// range (anchored to the original stack via main.c's gc_stack_top)
// stays correct.
static void asl_run_on_scratch_stack(struct asl_swap_ctx *ctx) {
    ctx->req = (struct FileRequester *)AllocAslRequest(
        ASL_FileRequest, ctx->tags);
    if (ctx->req == NULL) {
        ctx->ok = FALSE;
        return;
    }
    ctx->ok = AslRequest(ctx->req, NULL);
}

static const mp_arg_t mod_asl_file_request_args[] = {
    { MP_QSTR_title,          MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_initial_drawer, MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_initial_file,   MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_pattern,        MP_ARG_OBJ,  {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_save,           MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_drawers_only,   MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_multi,          MP_ARG_BOOL, {.u_bool = false} },
};

static mp_obj_t mod_asl_file_request(size_t n_args, const mp_obj_t *pos_args,
                                     mp_map_t *kw_args) {
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(mod_asl_file_request_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(mod_asl_file_request_args), mod_asl_file_request_args,
        arg_vals);

    const char *title    = mp_obj_str_get_str(arg_vals[0].u_obj);
    const char *drawer   = mp_obj_str_get_str(arg_vals[1].u_obj);
    const char *file     = mp_obj_str_get_str(arg_vals[2].u_obj);
    const char *pattern  = mp_obj_str_get_str(arg_vals[3].u_obj);
    bool save_mode       = arg_vals[4].u_bool;
    bool drawers_only    = arg_vals[5].u_bool;
    bool multi           = arg_vals[6].u_bool;

    // Save dialogs only make sense with a single target. ASL would
    // silently honour one or the other; we'd rather surface the
    // contradiction so callers fix the call site.
    if (multi && save_mode) {
        mp_raise_ValueError(MP_ERROR_TEXT("multi=True is incompatible with save=True"));
    }

    asl_ensure_open();

    // Build a TagItem array on the stack. Max slots = 8 user tags + TAG_DONE
    // (title, drawer, file, pattern + DoPatterns, save, drawers_only, multi).
    struct TagItem tags[10];
    int t = 0;
    if (title[0]) {
        tags[t].ti_Tag  = ASLFR_TitleText;
        tags[t].ti_Data = (ULONG)(uintptr_t)title;
        t++;
    }
    if (drawer[0]) {
        tags[t].ti_Tag  = ASLFR_InitialDrawer;
        tags[t].ti_Data = (ULONG)(uintptr_t)drawer;
        t++;
    }
    if (file[0]) {
        tags[t].ti_Tag  = ASLFR_InitialFile;
        tags[t].ti_Data = (ULONG)(uintptr_t)file;
        t++;
    }
    if (pattern[0]) {
        tags[t].ti_Tag  = ASLFR_InitialPattern;
        tags[t].ti_Data = (ULONG)(uintptr_t)pattern;
        t++;
        tags[t].ti_Tag  = ASLFR_DoPatterns;
        tags[t].ti_Data = TRUE;
        t++;
    }
    if (save_mode) {
        tags[t].ti_Tag  = ASLFR_DoSaveMode;
        tags[t].ti_Data = TRUE;
        t++;
    }
    if (drawers_only) {
        tags[t].ti_Tag  = ASLFR_DrawersOnly;
        tags[t].ti_Data = TRUE;
        t++;
    }
    if (multi) {
        tags[t].ti_Tag  = ASLFR_DoMultiSelect;
        tags[t].ti_Data = TRUE;
        t++;
    }
    tags[t].ti_Tag  = TAG_DONE;
    tags[t].ti_Data = 0;

    // Run the ASL call on a 32 KB scratch stack. The default AmigaShell
    // stack (often 4 KB) is too small for AslRequest's directory-listing
    // / font-loading code and trips an exception (CHK / 0x80000006)
    // somewhere after the user picks. Doing the swap inside file_request
    // means callers don't need to remember `Stack 32768` at the shell.
    APTR scratch = AllocVec(ASL_SWAP_STACK_BYTES, MEMF_ANY);
    if (scratch == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }
    struct asl_swap_ctx ctx;
    ctx.tags = tags;
    ctx.req  = NULL;
    ctx.ok   = FALSE;
    struct StackSwapStruct sss;
    sss.stk_Lower   = scratch;
    sss.stk_Upper   = (APTR)((char *)scratch + ASL_SWAP_STACK_BYTES);
    sss.stk_Pointer = sss.stk_Upper;
    StackSwap(&sss);
    asl_run_on_scratch_stack(&ctx);
    StackSwap(&sss);
    FreeVec(scratch);

    if (ctx.req == NULL) {
        mp_raise_OSError(MP_ENOMEM);
    }

    // AslRequest returned TRUE if the user clicked OK, FALSE on Cancel.
    // Build the return value back on the original stack so the GC's
    // stack-scan range stays correct.
    mp_obj_t result = mp_const_none;
    if (ctx.ok) {
        char buf[AMIGA_PATH_MAX];
        const char *drawer_str = (ctx.req->fr_Drawer != NULL)
            ? (const char *)ctx.req->fr_Drawer : "";
        if (multi) {
            // Multi-select: walk fr_NumArgs / fr_ArgList building a list.
            // Each WBArg's wa_Lock is 0 (ASL doesn't lock dirs) so we join
            // fr_Drawer with each entry's wa_Name into a fresh buffer.
            result = mp_obj_new_list(0, NULL);
            for (LONG i = 0; i < ctx.req->fr_NumArgs; i++) {
                struct WBArg *wa = &ctx.req->fr_ArgList[i];
                buf[0] = '\0';
                if (drawer_str[0] != '\0') {
                    AddPart((STRPTR)buf, (STRPTR)drawer_str, sizeof(buf));
                }
                if (wa->wa_Name != NULL && wa->wa_Name[0] != '\0') {
                    AddPart((STRPTR)buf, (STRPTR)wa->wa_Name, sizeof(buf));
                }
                mp_obj_list_append(result,
                    mp_obj_new_str(buf, strlen(buf)));
            }
        } else {
            // Single-pick. AddPart joins with the right separator ('/'
            // between dirs, ':' after a volume name). For drawers_only the
            // file component is empty -- AddPart copes either way.
            buf[0] = '\0';
            if (drawer_str[0] != '\0') {
                AddPart((STRPTR)buf, (STRPTR)drawer_str, sizeof(buf));
            }
            if (ctx.req->fr_File != NULL && ctx.req->fr_File[0] != '\0') {
                AddPart((STRPTR)buf, (STRPTR)ctx.req->fr_File, sizeof(buf));
            }
            result = mp_obj_new_str(buf, strlen(buf));
        }
    }
    FreeAslRequest(ctx.req);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_asl_file_request_obj, 0,
    mod_asl_file_request);

static const mp_rom_map_elem_t mod_asl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR__asl) },
    { MP_ROM_QSTR(MP_QSTR_file_request), MP_ROM_PTR(&mod_asl_file_request_obj) },
};
static MP_DEFINE_CONST_DICT(mod_asl_globals, mod_asl_globals_table);

const mp_obj_module_t mod_asl_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_asl_globals,
};

MP_REGISTER_MODULE(MP_QSTR__asl, mod_asl_module);

#endif // MICROPY_PY_AMIGA
