/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Phase 30: thin wrapper around intuition.library's EasyRequestArgs.
//
// Exposes three Python entry points (the latter two are tiny wrappers
// around the first):
//
//   _intuition.easy_request(title, body, buttons) -> int
//       0-based-leftmost index of the clicked button. The user-visible
//       AmigaOS convention is "rightmost == 0"; we translate that here
//       so Python users see the natural [0,N) leftmost-first ordering.
//
//   _intuition.auto_request(body, yes="Yes", no="No") -> bool
//       True if the first (Yes) button was picked, False otherwise.
//
//   _intuition.message(body, button="OK") -> None
//       Single-button informational requester. Return value is
//       discarded — there's only one outcome.
//
// All three are fully modal: the Python call blocks until the user
// clicks a button. The default public screen (typically Workbench) is
// the parent; if Workbench is closed the EasyRequestArgs call will
// fail with rc == -1 and we raise OSError.
//
// Printf-format injection safety: es_TextFormat is hard-coded to "%s"
// and the body is passed as the single varargs arg. The body can
// contain raw '%' characters with no consequences.

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"
#include "py/misc.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#if MICROPY_PY_AMIGA

// proto/intuition.h declares this extern; we own the definition. Opened
// lazily on first call. No explicit close: intuition.library is a
// system-wide library that AmigaOS reaps when the process exits, and
// every other task on the machine is already holding it open.
struct IntuitionBase *IntuitionBase = NULL;

static void intuition_ensure_open(void) {
    if (IntuitionBase == NULL) {
        IntuitionBase = (struct IntuitionBase *)OpenLibrary(
            (CONST_STRPTR)"intuition.library", 36);
        if (IntuitionBase == NULL) {
            mp_raise_OSError(MP_ENOENT);
        }
    }
}

// Run the EasyRequestArgs call. `title_ptr` may be NULL (no title bar
// caption). `body_ptr` must be a NUL-terminated C string. `gadgets_ptr`
// is the bar-separated label string (e.g. "Yes|No|Cancel"). Returns the
// raw AmigaOS rightmost-is-0 code; the caller translates if needed.
// Raises OSError(EIO) if EasyRequestArgs fails (return value -1) —
// typically because no public screen is available.
static LONG intuition_run(const char *title_ptr, const char *body_ptr,
    const char *gadgets_ptr) {
    intuition_ensure_open();
    struct EasyStruct es;
    es.es_StructSize = sizeof(struct EasyStruct);
    es.es_Flags = 0;
    es.es_Title = (CONST_STRPTR)(title_ptr ? title_ptr : "");
    es.es_TextFormat = (CONST_STRPTR)"%s";
    es.es_GadgetFormat = (CONST_STRPTR)gadgets_ptr;
    // args is a pointer to an array of values consumed by es_TextFormat.
    // Our format takes one %s, so args points at a single STRPTR.
    STRPTR body_arg = (STRPTR)body_ptr;
    LONG r = EasyRequestArgs(NULL, &es, NULL, &body_arg);
    if (r == -1) {
        mp_raise_OSError(MP_EIO);
    }
    return r;
}

// Walk `buttons_in` building "label1|label2|label3" in `out`. Returns
// the number of labels. Raises TypeError if the iterable is empty.
static size_t intuition_join_labels(mp_obj_t buttons_in, vstr_t *out) {
    mp_obj_iter_buf_t iter_buf;
    mp_obj_t iter = mp_getiter(buttons_in, &iter_buf);
    mp_obj_t item;
    size_t n = 0;
    while ((item = mp_iternext(iter)) != MP_OBJ_STOP_ITERATION) {
        if (n > 0) {
            vstr_add_char(out, '|');
        }
        size_t l;
        const char *s = mp_obj_str_get_data(item, &l);
        vstr_add_strn(out, s, l);
        n++;
    }
    if (n == 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("at least one button required"));
    }
    return n;
}

// Translate AmigaOS rightmost-is-0 to Python 0-based-leftmost.
//   AmigaOS:  N buttons indexed (left-to-right) 1, 2, ..., N-1, 0
//             so the rightmost gadget is 0 and the leftmost is 1.
//   Python:   left-to-right 0, 1, ..., N-1.
// r==0 → (N-1).  r in [1, N-1] → (r-1).
static int intuition_translate(LONG r, size_t n) {
    if (r == 0) {
        return (int)n - 1;
    }
    return (int)r - 1;
}

// _intuition.easy_request(title, body, buttons) -> int
static mp_obj_t mod_intuition_easy_request(mp_obj_t title_in,
    mp_obj_t body_in,
    mp_obj_t buttons_in) {
    const char *title = mp_obj_str_get_str(title_in);
    const char *body = mp_obj_str_get_str(body_in);
    vstr_t labels;
    vstr_init(&labels, 32);
    size_t n = intuition_join_labels(buttons_in, &labels);
    vstr_null_terminated_str(&labels);
    LONG r = intuition_run(title, body, labels.buf);
    vstr_clear(&labels);
    return MP_OBJ_NEW_SMALL_INT(intuition_translate(r, n));
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_intuition_easy_request_obj,
    mod_intuition_easy_request);

// _intuition.auto_request(body, yes="Yes", no="No") -> bool
static mp_obj_t mod_intuition_auto_request(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_body, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_yes,  MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_Yes)} },
        { MP_QSTR_no,   MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_No)} },
    };
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, arg_vals);
    const char *body = mp_obj_str_get_str(arg_vals[0].u_obj);
    const char *yes = mp_obj_str_get_str(arg_vals[1].u_obj);
    const char *no = mp_obj_str_get_str(arg_vals[2].u_obj);
    vstr_t labels;
    vstr_init(&labels, 32);
    vstr_add_str(&labels, yes);
    vstr_add_char(&labels, '|');
    vstr_add_str(&labels, no);
    vstr_null_terminated_str(&labels);
    LONG r = intuition_run(NULL, body, labels.buf);
    vstr_clear(&labels);
    // AmigaOS: 1 == leftmost (Yes), 0 == rightmost (No).
    return mp_obj_new_bool(r == 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_auto_request_obj, 1,
    mod_intuition_auto_request);

// _intuition.message(body, button="OK") -> None
static mp_obj_t mod_intuition_message(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_body,   MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_button, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_OK)} },
    };
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, arg_vals);
    const char *body = mp_obj_str_get_str(arg_vals[0].u_obj);
    const char *button = mp_obj_str_get_str(arg_vals[1].u_obj);
    (void)intuition_run(NULL, body, button);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_intuition_message_obj, 1,
    mod_intuition_message);

void amiga_intuition_close(void) {
    if (IntuitionBase != NULL) {
        CloseLibrary((struct Library *)IntuitionBase);
        IntuitionBase = NULL;
    }
}

static const mp_rom_map_elem_t mod_intuition_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR__intuition) },
    { MP_ROM_QSTR(MP_QSTR_easy_request), MP_ROM_PTR(&mod_intuition_easy_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_auto_request), MP_ROM_PTR(&mod_intuition_auto_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_message),      MP_ROM_PTR(&mod_intuition_message_obj) },
};
static MP_DEFINE_CONST_DICT(mod_intuition_globals, mod_intuition_globals_table);

const mp_obj_module_t mod_intuition_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_intuition_globals,
};

MP_REGISTER_MODULE(MP_QSTR__intuition, mod_intuition_module);

#endif // MICROPY_PY_AMIGA
