// Phase 36: thin wrapper around locale.library's catalog lookups.
//
// Exposes three Python entry points:
//
//   _catalog.open(name, version=0, language=None,
//                 built_in_language=None) -> Catalog
//     OpenCatalogA(NULL, name, [OC_Version, OC_BuiltInLanguage?,
//                               OC_Language?, TAG_DONE]).
//     `version` is the minimum required catalog version (0 = any).
//     `language` overrides the system default if passed; otherwise
//     locale.library picks the best match from the user's preferred
//     language list.
//     `built_in_language` tells locale.library what language is
//     compiled into the binary's default strings -- when the
//     requested `language` matches, locale.library refuses to open
//     (there's nothing to load).  Pass a different code (e.g.
//     "german") to force a translation file lookup even when asking
//     for "english".
//     Raises OSError(ENOENT) if the catalog isn't found,
//     OSError(EIO) if locale.library itself can't open.
//
//   _catalog.language() -> str
//     The user's first preferred language from Locale->loc_PrefLanguages[0],
//     or "english" if no preference is set.
//
//   _catalog.Catalog
//     The Python type itself, re-exported so isinstance() works.
//
// Catalog objects support:
//   .lookup(id, default)    -- GetCatalogStr(cat, id, default); returns
//                              the catalog string or the default.
//   .close()                -- CloseCatalog; idempotent
//   __enter__ / __exit__    -- with-statement support
//   __del__                 -- forwards to close()

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/mperrno.h"

#include <string.h>

#include <exec/types.h>
#include <libraries/locale.h>
#include <proto/exec.h>
#include <proto/locale.h>

#if MICROPY_PY_AMIGA

// proto/locale.h declares LocaleBase as extern; we own the definition.
// Opened lazily on first call.  No explicit close: locale.library is a
// system-wide library that AmigaOS reaps at process exit, matching the
// intuition / icon / asl lazy-open pattern.
struct LocaleBase *LocaleBase = NULL;

static bool catalog_ensure_open(void) {
    if (LocaleBase == NULL) {
        LocaleBase = (struct LocaleBase *)OpenLibrary(
            (CONST_STRPTR)"locale.library", 38);
    }
    return LocaleBase != NULL;
}

// ---------- Catalog Python type ----------

typedef struct _amiga_catalog_obj_t {
    mp_obj_base_t base;
    struct Catalog *cat;   // NULL after .close()
} amiga_catalog_obj_t;

static const mp_obj_type_t amiga_catalog_type;

static mp_obj_t amiga_catalog_make(struct Catalog *cat) {
    amiga_catalog_obj_t *self = mp_obj_malloc(
        amiga_catalog_obj_t, &amiga_catalog_type);
    self->cat = cat;
    return MP_OBJ_FROM_PTR(self);
}

// .lookup(id, default) -- GetCatalogStr handles a NULL catalog and a
// missing string id by returning the default pointer, so we don't
// need separate guards.  Returns a str copy so the lifetime tracks
// the caller, not the underlying locale.library buffer.
static mp_obj_t amiga_catalog_lookup(mp_obj_t self_in,
    mp_obj_t id_in,
    mp_obj_t default_in) {
    amiga_catalog_obj_t *self = MP_OBJ_TO_PTR(self_in);
    LONG id = mp_obj_get_int(id_in);
    const char *def = mp_obj_str_get_str(default_in);
    STRPTR result = GetCatalogStr(self->cat, id, (CONST_STRPTR)def);
    return mp_obj_new_str((const char *)result, strlen((const char *)result));
}
static MP_DEFINE_CONST_FUN_OBJ_3(amiga_catalog_lookup_obj,
    amiga_catalog_lookup);

static mp_obj_t amiga_catalog_close(mp_obj_t self_in) {
    amiga_catalog_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->cat != NULL) {
        CloseCatalog(self->cat);
        self->cat = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_catalog_close_obj,
    amiga_catalog_close);

static mp_obj_t amiga_catalog_enter(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(amiga_catalog_enter_obj,
    amiga_catalog_enter);

static mp_obj_t amiga_catalog_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return amiga_catalog_close(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(amiga_catalog_exit_obj,
    4, 4, amiga_catalog_exit);

static const mp_rom_map_elem_t amiga_catalog_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_lookup),    MP_ROM_PTR(&amiga_catalog_lookup_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&amiga_catalog_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),   MP_ROM_PTR(&amiga_catalog_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&amiga_catalog_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&amiga_catalog_exit_obj) },
};
static MP_DEFINE_CONST_DICT(amiga_catalog_locals_dict,
    amiga_catalog_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    amiga_catalog_type,
    MP_QSTR_Catalog,
    MP_TYPE_FLAG_NONE,
    locals_dict, &amiga_catalog_locals_dict);

// ---------- _catalog.open(name, version=0, language=None) ----------

static mp_obj_t mod_catalog_open(size_t n_args, const mp_obj_t *pos_args,
    mp_map_t *kw_args) {
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_name,              MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_version,           MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_language,          MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_built_in_language, MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t arg_vals[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, arg_vals);

    if (!catalog_ensure_open()) {
        mp_raise_OSError(MP_EIO);
    }

    const char *name = mp_obj_str_get_str(arg_vals[0].u_obj);
    LONG version = arg_vals[1].u_int;
    const char *language = NULL;
    if (arg_vals[2].u_obj != mp_const_none) {
        language = mp_obj_str_get_str(arg_vals[2].u_obj);
    }
    const char *built_in = NULL;
    if (arg_vals[3].u_obj != mp_const_none) {
        built_in = mp_obj_str_get_str(arg_vals[3].u_obj);
    }

    // Tag-list assembly: OC_Version is always present;
    // OC_BuiltInLanguage / OC_Language are optional; TAG_DONE ends.
    struct TagItem tags[4];
    int n = 0;
    tags[n].ti_Tag = OC_Version;
    tags[n].ti_Data = (ULONG)version;
    n++;
    if (built_in != NULL) {
        tags[n].ti_Tag = OC_BuiltInLanguage;
        tags[n].ti_Data = (ULONG)built_in;
        n++;
    }
    if (language != NULL) {
        tags[n].ti_Tag = OC_Language;
        tags[n].ti_Data = (ULONG)language;
        n++;
    }
    tags[n].ti_Tag = TAG_DONE;
    tags[n].ti_Data = 0;

    struct Catalog *cat = OpenCatalogA(NULL, (CONST_STRPTR)name, tags);
    if (cat == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    return amiga_catalog_make(cat);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_catalog_open_obj, 1, mod_catalog_open);

// ---------- _catalog.language() -> str ----------
//
// Returns the system's first preferred language (loc_PrefLanguages[0])
// or "english" if no preference is set / locale.library can't open.
// Calls OpenLocale(NULL) / CloseLocale so the system Locale isn't
// pinned by this call.
static mp_obj_t mod_catalog_language(void) {
    if (!catalog_ensure_open()) {
        return mp_obj_new_str("english", 7);
    }
    struct Locale *loc = OpenLocale(NULL);
    if (loc == NULL) {
        return mp_obj_new_str("english", 7);
    }
    const char *lang = "english";
    if (loc->loc_PrefLanguages[0] != NULL
        && loc->loc_PrefLanguages[0][0] != '\0') {
        lang = (const char *)loc->loc_PrefLanguages[0];
    }
    mp_obj_t result = mp_obj_new_str(lang, strlen(lang));
    CloseLocale(loc);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_0(mod_catalog_language_obj,
    mod_catalog_language);

// ---------- module globals ----------

static const mp_rom_map_elem_t mod_catalog_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR__catalog) },
    { MP_ROM_QSTR(MP_QSTR_open),     MP_ROM_PTR(&mod_catalog_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_language), MP_ROM_PTR(&mod_catalog_language_obj) },
    { MP_ROM_QSTR(MP_QSTR_Catalog),  MP_ROM_PTR(&amiga_catalog_type) },
};
static MP_DEFINE_CONST_DICT(mod_catalog_globals, mod_catalog_globals_table);

const mp_obj_module_t mod_catalog_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mod_catalog_globals,
};

MP_REGISTER_MODULE(MP_QSTR__catalog, mod_catalog_module);

#endif // MICROPY_PY_AMIGA
