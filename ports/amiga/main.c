#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>

#include "genhdr/mpversion.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/mphal.h"
#include "py/mpprint.h"
#include "py/objlist.h"
#include "py/repl.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/readline/readline.h"
#include "shared/runtime/pyexec.h"
#include "extmod/vfs.h"

// Tracked AllocVec chunks backing the GC heap. The first slot is the
// initial heap; further slots fill as the GC grows via gc_try_add_heap.
// 16 slots * 4 MiB each comfortably covers any realistic Amiga; the cap
// is here mainly to put a bound on the tracking array. AmigaOS doesn't
// reclaim AllocVec'd memory when the task exits, so we walk this list at
// shutdown and FreeVec each chunk.
#define AMIGA_HEAP_MAX_CHUNKS 16
static struct {
    void *ptr;
    size_t bytes;
} amiga_heap_chunks[AMIGA_HEAP_MAX_CHUNKS];
static size_t amiga_heap_total_bytes;        // sum of all chunk sizes
static size_t amiga_heap_max_bytes;          // -X maxheap=... cap (0 = unlimited)

void *amiga_alloc_heap(size_t n) {
    // Honour the -X maxheap cap. gc_get_max_new_split() also clamps to
    // this, but the GC may still call us at the exact limit; double-check.
    if (amiga_heap_max_bytes != 0 &&
        amiga_heap_total_bytes + n > amiga_heap_max_bytes) {
        return NULL;
    }
    void *p = AllocVec((ULONG)n, MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
    if (!p) {
        return NULL;
    }
    for (size_t i = 0; i < AMIGA_HEAP_MAX_CHUNKS; i++) {
        if (amiga_heap_chunks[i].ptr == NULL) {
            amiga_heap_chunks[i].ptr = p;
            amiga_heap_chunks[i].bytes = n;
            amiga_heap_total_bytes += n;
            return p;
        }
    }
    // Tracking array full -- shouldn't happen in practice. Leak rather
    // than refuse the allocation: the GC will still work; we just won't
    // FreeVec this chunk at exit. The next chunk will fit again.
    return p;
}

void amiga_free_heap(void *p) {
    for (size_t i = 0; i < AMIGA_HEAP_MAX_CHUNKS; i++) {
        if (amiga_heap_chunks[i].ptr == p) {
            amiga_heap_total_bytes -= amiga_heap_chunks[i].bytes;
            amiga_heap_chunks[i].ptr = NULL;
            amiga_heap_chunks[i].bytes = 0;
            break;
        }
    }
    FreeVec(p);
}

#if MICROPY_GC_SPLIT_HEAP_AUTO
size_t gc_get_max_new_split(void) {
    // Largest contiguous AllocVec'able block, less a small headroom for
    // exec.library's per-allocation bookkeeping. Clamped to -X maxheap=
    // if the user set one.
    ULONG largest = AvailMem(MEMF_ANY | MEMF_PUBLIC | MEMF_LARGEST);
    if (largest < 256) {
        return 0;
    }
    largest -= 64;
    if (amiga_heap_max_bytes != 0) {
        if (amiga_heap_total_bytes >= amiga_heap_max_bytes) {
            return 0;
        }
        size_t remaining = amiga_heap_max_bytes - amiga_heap_total_bytes;
        if ((size_t)largest > remaining) {
            largest = (ULONG)remaining;
        }
    }
    return (size_t)largest;
}
#endif

// amiga.heap_info() needs to know how many distinct AllocVec chunks the
// GC is currently using. Walking the slot array is the canonical source.
size_t amiga_heap_chunk_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < AMIGA_HEAP_MAX_CHUNKS; i++) {
        if (amiga_heap_chunks[i].ptr != NULL) n++;
    }
    return n;
}

// Parse a heap-size string like "256K" / "2M" / "524288" into bytes.
// Returns 0 on parse failure or non-positive input.
static size_t parse_heap_size(const char *s) {
    size_t v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (size_t)(*p - '0');
        p++;
    }
    if (p == s) return 0;
    if (*p == 'K' || *p == 'k') {
        v *= 1024;
        p++;
    } else if (*p == 'M' || *p == 'm') {
        v *= 1024 * 1024;
        p++;
    }
    if (*p != '\0') return 0;
    return v;
}

// Set by `-X compile-only`. shared/runtime/pyexec.c checks this before
// invoking module_fun, so the script gets parsed and compiled but not
// executed. Provided by every port that defines MICROPY_PYEXEC_COMPILE_ONLY.
bool mp_compile_only = false;

// Set by bebbo crt0 when our process is launched from Workbench (pr_CLI == 0):
// the runtime does WaitPort + GetMsg on the process MsgPort and stores the
// WBStartup pointer here, then Forbid()+ReplyMsg's it on exit. We only have
// to read it — null means CLI launch. amiga.launched_from_workbench() and
// amiga.wb_selected_files() in modamiga.c go through this same pointer.
extern struct WBStartup *_WBenchMsg;

// icon.library state. Opened on demand when the script (or our startup
// code) first looks at a tooltype; closed at shutdown. icon.library may
// genuinely be absent on stripped-down systems and is absent under vamos,
// so callers treat NULL IconBase as "no tooltypes available". The Workbench
// disk object is GetDiskObject'd from sm_ArgList[0] on WB launch so the
// "SCRIPT=" tooltype is available before mp_init().
struct Library *IconBase = NULL;
static struct DiskObject *amiga_wb_diskobject = NULL;

bool amiga_icon_open(void) {
    if (IconBase == NULL) {
        IconBase = OpenLibrary((STRPTR)"icon.library", 33);
    }
    return IconBase != NULL;
}

struct DiskObject *amiga_wb_get_diskobject(void) {
    return amiga_wb_diskobject;
}

// CON: stream opened for stdout/stderr when launched from Workbench (where
// pr_CIS / pr_COS are NULL). Kept here so we can Close() it at shutdown.
static BPTR amiga_wb_console = 0;

// Stack ceiling captured at main() entry. Used by gc_collect when
// task->tc_SPUpper is unavailable (vamos leaves it zero) to bound the
// stack scan. The address of a local in main() is below the start of
// the stack, but main()'s caller frames are all below us too, so this
// is a safe upper bound: anything we want to keep alive is at or below
// gc_stack_top.
static void *gc_stack_top;

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // Scan from the current SP (approximated by a local variable) up to
    // gc_stack_top (captured at main() entry). On real AmigaOS we could
    // use task->tc_SPUpper, but vamos leaves that field zero on the
    // initial process, which would compute a multi-gigabyte scan length
    // and walk off into unmapped memory. Bounding the scan to main()'s
    // frame and below is sufficient: nothing above main() (C runtime,
    // AmigaOS startup) holds MicroPython object references.
    void *dummy;
    if (gc_stack_top == NULL || (char *)&dummy >= (char *)gc_stack_top) {
        // Defensive: should not happen, but skip the scan rather than
        // walk an enormous range.
        gc_collect_start();
        gc_collect_end();
        return;
    }
    gc_collect_start();
    gc_collect_root(&dummy,
        ((char *)gc_stack_top - (char *)&dummy) / sizeof(void *));
    gc_collect_end();
}
#endif

static void print_help(const char *prog) {
    mp_hal_stdout_tx_str("Usage: ");
    mp_hal_stdout_tx_str(prog);
    mp_hal_stdout_tx_str(" [options] [<script> [args]]\r\n"
        "\r\n"
        "Options:\r\n"
        "  -h, --help     Show this help message and exit\r\n"
        "  --version      Show version info and exit\r\n"
        "  -c <cmd>       Run statement as Python code and exit\r\n"
        "  -m <module>    Run named module as a script and exit\r\n"
        "\r\n"
        "Without any options, start the interactive REPL.\r\n");
}

static void print_version(void) {
    mp_hal_stdout_tx_str(MICROPY_BANNER_NAME_AND_VERSION "\r\n");
}

// Extract the directory portion of an AmigaOS path.
//   "Work:scripts/foo.py" -> "Work:scripts"
//   "Work:foo.py"         -> "Work:"
//   "foo.py"              -> ""  (current directory)
static void path_dir(const char *path, char *buf, size_t buflen) {
    const char *last = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == ':' || *p == '/') {
            last = p;
        }
    }
    if (!last) {
        buf[0] = '\0';
        return;
    }
    // Include ':' (e.g. "Work:") but exclude '/' (e.g. "Work:scripts" not "Work:scripts/")
    size_t len = (*last == ':') ? (size_t)(last - path + 1) : (size_t)(last - path);
    if (len >= buflen) {
        len = buflen - 1;
    }
    memcpy(buf, path, len);
    buf[len] = '\0';
}

static void set_sys_argv(char **argv, int argc, int start) {
    for (int i = start; i < argc; i++) {
        mp_obj_list_append(mp_sys_argv, mp_obj_new_str(argv[i], strlen(argv[i])));
    }
}

// Parse the AmigaOS CLI argument string (pr_Arguments) into argc/argv.
//
// The bebbo C runtime's __nocommandline parser produces broken argv
// pointers under vamos, so we parse the raw arg string ourselves.
// pr_Arguments is a NUL-terminated string with a trailing newline.
//
// Tokenisation rules (matching AmigaOS shell):
//   - whitespace (space, tab, LF) separates tokens
//   - "..." groups quote a single token; '*' is the escape character:
//     *N = LF, *E = ESC, *" = ", ** = *
//   - the trailing newline ends parsing
//
// Returns the number of tokens parsed (argv[0] is always the program name).
// argv array and string buffer are allocated with AllocVec; the buffer
// pointer is returned via *buf_out so main() can free it at exit.
static int amiga_parse_args(char ***argv_out, char **buf_out) {
    struct Process *me = (struct Process *)FindTask(NULL);
    const char *src = (me->pr_Arguments) ? (const char *)me->pr_Arguments : "";

    // Get program name from dos.library; fall back to "micropython".
    char prog_buf[64];
    prog_buf[0] = 0;
    if (DOSBase->dl_lib.lib_Version >= 36) {
        GetProgramName((STRPTR)prog_buf, sizeof(prog_buf));
    }
    if (!prog_buf[0]) {
        strcpy(prog_buf, "micropython");
    }

    // Worst-case: every char becomes a separate token, plus argv[0].
    size_t srclen = strlen(src);
    size_t plen = strlen(prog_buf);
    size_t bufsz = srclen + plen + 16;
    char *buf = AllocVec(bufsz, MEMF_ANY | MEMF_CLEAR);
    char **argv = AllocVec((srclen / 2 + 4) * sizeof(char *), MEMF_ANY | MEMF_CLEAR);
    if (!buf || !argv) {
        if (buf) FreeVec(buf);
        if (argv) FreeVec(argv);
        return 0;
    }

    // argv[0] = program name.
    memcpy(buf, prog_buf, plen + 1);
    argv[0] = buf;
    int argc = 1;
    char *out = buf + plen + 1;

    const char *p = src;
    while (*p) {
        // Skip leading whitespace.
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (!*p) break;

        argv[argc++] = out;

        if (*p == '"') {
            // Quoted token; '*' is the AmigaOS escape character.
            p++;
            while (*p && *p != '"') {
                if (*p == '*' && p[1]) {
                    p++;
                    if (*p == 'N' || *p == 'n') {
                        *out++ = '\n';
                    } else if (*p == 'E' || *p == 'e') {
                        *out++ = 0x1b;
                    } else {
                        *out++ = *p;
                    }
                    p++;
                } else {
                    *out++ = *p++;
                }
            }
            if (*p == '"') p++;
        } else {
            // Unquoted token: read until whitespace.
            while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
                *out++ = *p++;
            }
        }
        *out++ = '\0';
    }

    *argv_out = argv;
    *buf_out = buf;
    return argc;
}

static int run_str(const char *str) {
    vstr_t vstr;
    size_t len = strlen(str);
    vstr_init(&vstr, len + 1);
    vstr_add_strn(&vstr, str, len);
    vstr_add_byte(&vstr, '\n');
    int ret = pyexec_vstr(&vstr, true);
    vstr_clear(&vstr);
    return ret;
}

int main(int argc_unused, char **argv_unused) {
    (void)argc_unused;
    (void)argv_unused;

    // Record the stack address at main() entry as the GC's stack ceiling.
    // See gc_collect() above for why we don't trust task->tc_SPUpper.
    int stack_top_marker;
    gc_stack_top = &stack_top_marker;

    // Workbench launch: bebbo crt0 has already done WaitPort+GetMsg and put
    // the WBStartup pointer in _WBenchMsg. There's no shell, so pr_CIS /
    // pr_COS are NULL and any later Write(Output(), ...) would silently
    // drop output. Open a CON: window and point pr_CIS/pr_COS at it so
    // print(), input() and traceback printing all work normally. The CON:
    // spec uses /AUTO so the window only pops when something is written,
    // /CLOSE so it closes on the close gadget, /WAIT so the window stays
    // open after main() returns until the user clicks close (otherwise any
    // output from a short script would flash by and vanish).
    //
    // Also load the executable's .info icon now so tooltypes (SCRIPT=,
    // HEAP=, MAXHEAP=) are available before the heap is sized below.
    // sm_ArgList[0] is the tool itself (the icon the user double-clicked);
    // its wa_Lock is already our pr_CurrentDir thanks to Workbench's
    // pre-launch CurrentDir setup, so wa_Name resolves correctly.
    const char *wb_tt_script = NULL;
    if (_WBenchMsg != NULL) {
        amiga_wb_console = Open(
            (STRPTR)"CON:0/30/640/200/MicroPython/AUTO/CLOSE/WAIT",
            MODE_NEWFILE);
        if (amiga_wb_console) {
            struct Process *me = (struct Process *)FindTask(NULL);
            me->pr_CIS = amiga_wb_console;
            me->pr_COS = amiga_wb_console;
            // pr_ConsoleTask drives Open("*", ...) and the readline raw-mode
            // SetMode() path; point it at the new console's underlying task.
            struct FileHandle *fh = (struct FileHandle *)BADDR(amiga_wb_console);
            me->pr_ConsoleTask = (APTR)fh->fh_Type;
        }
        if (_WBenchMsg->sm_NumArgs > 0 && amiga_icon_open()) {
            // GetDiskObject finds <wa_Name>.info relative to the current
            // directory. wa_Lock points at the directory holding the icon
            // and Workbench has already CurrentDir'd us there.
            amiga_wb_diskobject = GetDiskObject(_WBenchMsg->sm_ArgList[0].wa_Name);
            if (amiga_wb_diskobject != NULL && amiga_wb_diskobject->do_ToolTypes != NULL) {
                wb_tt_script = (const char *)FindToolType(
                    (CONST_STRPTR *)amiga_wb_diskobject->do_ToolTypes,
                    (CONST_STRPTR)"SCRIPT");
            }
        }
    }

    // Parse arguments ourselves from pr_Arguments; the bebbo C runtime
    // produces broken argv pointers under vamos. WB launch has no
    // pr_Arguments (sm_ArgList carries selected files instead, exposed
    // via amiga.wb_selected_files()), so skip the parse there.
    char **argv = NULL;
    char *argv_buf = NULL;
    static char *wb_fallback_argv[] = {"micropython", NULL};
    int argc;
    if (_WBenchMsg != NULL) {
        argv = wb_fallback_argv;
        argc = 1;
    } else {
        argc = amiga_parse_args(&argv, &argv_buf);
        if (argc == 0) {
            // Parse failed (out of memory); fall back to a single dummy argv.
            static char *fallback_argv[] = {"micropython", NULL};
            argv = fallback_argv;
            argc = 1;
        }
    }

    // Resolve initial heap size and growth cap before the first alloc.
    // Priority: command-line -X heap=<N> > MICROPYHEAP env var > default.
    // Same priority for -X maxheap=<N> > MICROPYHEAPMAX env var > unlimited.
    // Pre-scan argv for the -X heap=/-X maxheap= forms so the heap is
    // sized correctly *before* gc_init().
    size_t initial_heap = MICROPY_HEAP_SIZE;
    char envbuf[32];
    if (GetVar((STRPTR)"MICROPYHEAP", (STRPTR)envbuf, sizeof(envbuf), 0) > 0) {
        size_t v = parse_heap_size(envbuf);
        if (v != 0) initial_heap = v;
    }
    if (GetVar((STRPTR)"MICROPYHEAPMAX", (STRPTR)envbuf, sizeof(envbuf), 0) > 0) {
        size_t v = parse_heap_size(envbuf);
        if (v != 0) amiga_heap_max_bytes = v;
    }
    for (int a = 1; a + 1 < argc; a++) {
        if (strcmp(argv[a], "-X") != 0) continue;
        if (strncmp(argv[a + 1], "heap=", 5) == 0) {
            size_t v = parse_heap_size(argv[a + 1] + 5);
            if (v != 0) initial_heap = v;
        } else if (strncmp(argv[a + 1], "maxheap=", 8) == 0) {
            size_t v = parse_heap_size(argv[a + 1] + 8);
            if (v != 0) amiga_heap_max_bytes = v;
        }
    }
    // Workbench tooltypes can also drive heap sizing; this is the only way
    // for a Workbench-launched user to override the default since there's
    // no command line. Tooltype priority is below env vars but above the
    // compiled default, matching the documented "edit tooltypes to tune"
    // expectation. FindToolType returns a pointer just past "HEAP=" so it
    // can be passed straight to parse_heap_size.
    if (amiga_wb_diskobject != NULL && amiga_wb_diskobject->do_ToolTypes != NULL) {
        UBYTE *tt_heap = FindToolType(
            (CONST_STRPTR *)amiga_wb_diskobject->do_ToolTypes, (CONST_STRPTR)"HEAP");
        if (tt_heap != NULL) {
            size_t v = parse_heap_size((const char *)tt_heap);
            if (v != 0) initial_heap = v;
        }
        UBYTE *tt_max = FindToolType(
            (CONST_STRPTR *)amiga_wb_diskobject->do_ToolTypes, (CONST_STRPTR)"MAXHEAP");
        if (tt_max != NULL) {
            size_t v = parse_heap_size((const char *)tt_max);
            if (v != 0) amiga_heap_max_bytes = v;
        }
    }
    // If maxheap is set and smaller than initial, raise it to fit the
    // initial heap rather than refusing the allocation outright. The
    // user's intent for maxheap is "cap on growth"; honouring it strictly
    // against the initial alloc would just fail confusingly.
    if (amiga_heap_max_bytes != 0 && amiga_heap_max_bytes < initial_heap) {
        amiga_heap_max_bytes = initial_heap;
    }

    // Allocate the initial GC heap. AmigaOS hands out the fastest
    // available memory first when MEMF_ANY is set, so we don't need a
    // separate MEMF_FAST-then-fallback dance; on chip-only machines
    // (stock A500/A1000/A2000) MEMF_FAST would never succeed anyway.
    // MEMF_CLEAR zeroes the region before gc_init() sets up its
    // bookkeeping so there's no chance of reading stale bits from a
    // previous task. Goes through amiga_alloc_heap so it's recorded in
    // the chunk tracking array; gc_init takes care of teaching the GC
    // about it. Subsequent growth via MICROPY_GC_SPLIT_HEAP_AUTO routes
    // through the same wrapper.
    void *initial_ptr = amiga_alloc_heap(initial_heap);
    if (!initial_ptr) {
        return 1;
    }

    #if MICROPY_STACK_CHECK
    mp_stack_ctrl_init();
    // AmigaOS CLIs run with whatever stack the user (or the Stack
    // command) gave them; the AmigaDOS 4 KiB default is too tight for
    // anything beyond trivial recursion, so the documented expectation
    // for this port is `Stack 32768` or similar before launching, and
    // tools/amiga-vamos-run.sh asks vamos for `-s 32`.
    //
    // Use task->tc_SPLower/tc_SPUpper when populated to size the limit
    // dynamically with a 2 KiB safety margin. Vamos zeros both fields
    // on the initial process, in which case we assume the documented
    // 32 KiB-ish stack and use a 24 KiB limit -- conservative against
    // a smaller stack but big enough that the test suite's nested
    // imports and stress-test recursion paths run cleanly.
    {
        struct Task *task = FindTask(NULL);
        size_t stack_size = (size_t)((char *)task->tc_SPUpper - (char *)task->tc_SPLower);
        size_t limit;
        if (stack_size > 4 * 1024) {
            limit = stack_size - 2 * 1024;
        } else {
            limit = 24 * 1024;
        }
        mp_stack_set_limit(limit);
    }
    #endif

    #if MICROPY_ENABLE_GC
    gc_init(initial_ptr, (char *)initial_ptr + initial_heap);
    #endif

    #if MICROPY_ENABLE_PYSTACK
    // 4 KB pystack, matching the unix port; aligned(8) to satisfy
    // MICROPY_PYSTACK_ALIGN since bebbo's default 16-bit struct alignment
    // would otherwise leave a static char[] only 2-byte aligned.
    static char pystack[4096] __attribute__((aligned(8)));
    mp_pystack_init(pystack, pystack + sizeof(pystack));
    #endif

    // Open timer.device for mp_hal_ticks_* / mp_hal_delay_* (Phase 23).
    // Must happen before any code that might call into these HAL hooks; in
    // practice mp_init() and below all do. A failure here is non-fatal —
    // amiga_timer.c degrades to dos.library Delay() and ticks_* return 0.
    amiga_timer_open();

    mp_init();
    // mp_init() already initializes sys.path = [""] and sys.argv = [].

    // Phase 26: append `PROGDIR:` to sys.path so users can drop modules
    // next to the binary and `import` them without extra setup.
    // AmigaDOS auto-assigns PROGDIR: to the directory of the running
    // executable, so the assign-resolution machinery takes care of the
    // path lookup.  Appended (not prepended) so the cwd-or-script-dir
    // entry that lives at index 0 keeps its expected import priority.
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(qstr_from_str("PROGDIR:")));

    #if MICROPY_VFS
    // Mount the VfsAmiga at "/" so os.chdir/os.listdir/os.stat/open() etc.
    // all flow through dos.library via vfs_amiga.c. AmigaOS paths don't
    // start with "/" (they look like "volume:dir/file" or are relative);
    // the VFS layer's lookup_path treats anything without a leading "/"
    // as a relative-path-on-current-VFS, which routes straight to us.
    {
        extern const mp_obj_type_t mp_type_vfs_amiga;
        mp_obj_t args[2] = {
            MP_OBJ_TYPE_GET_SLOT(&mp_type_vfs_amiga, make_new)(&mp_type_vfs_amiga, 0, 0, NULL),
            MP_OBJ_NEW_QSTR(MP_QSTR__slash_),
        };
        mp_vfs_mount(2, args, (mp_map_t *)&mp_const_empty_map);
        MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_mount_table);
        while (MP_STATE_VM(vfs_cur)->next != NULL) {
            MP_STATE_VM(vfs_cur) = MP_STATE_VM(vfs_cur)->next;
        }
    }
    #endif

    #if MICROPY_PY_AMIGA_SOCKET
    extern bool amiga_socket_open(void);
    amiga_socket_open();
    #endif

    int exit_code = 0;
    bool ran_something = false;

    for (int a = 1; a < argc; a++) {
        if (argv[a][0] == '-') {
            if (strcmp(argv[a], "-h") == 0 || strcmp(argv[a], "--help") == 0) {
                print_help(argv[0]);
                ran_something = true;
                break;

            } else if (strcmp(argv[a], "--version") == 0) {
                print_version();
                ran_something = true;
                break;

            } else if (strcmp(argv[a], "-c") == 0) {
                if (a + 1 >= argc) {
                    mp_hal_stdout_tx_str("micropython: -c requires an argument\r\n");
                    exit_code = 2;
                    ran_something = true;
                    break;
                }
                // CPython convention: sys.argv = ["-c", remaining_args...]
                set_sys_argv(argv, argc, a);
                exit_code = run_str(argv[++a]);
                ran_something = true;
                break;

            } else if (strcmp(argv[a], "-X") == 0) {
                // -X <impl-option>. tests/run-tests.py emits -X emit=bytecode
                // (and on macOS -X realtime); we accept those silently. Honour
                // -X compile-only so the cmd_compile_only test can verify
                // parse-but-don't-execute behaviour. -X heap= / -X maxheap=
                // were already consumed by the pre-scan above the alloc and
                // are accepted silently here.
                if (a + 1 < argc) {
                    if (strcmp(argv[a + 1], "compile-only") == 0) {
                        mp_compile_only = true;
                    }
                    a++;
                }

            } else if (argv[a][1] == 'O' && (argv[a][2] == '\0' || (argv[a][2] >= '0' && argv[a][2] <= '9'))) {
                // -O / -OO / -O<digit> -- raise the optimisation level so
                // assert is dropped and __debug__ becomes False.
                if (argv[a][2] >= '0' && argv[a][2] <= '9') {
                    MP_STATE_VM(mp_optimise_value) = argv[a][2] - '0';
                } else {
                    for (const char *p = argv[a] + 1; *p == 'O'; p++) {
                        MP_STATE_VM(mp_optimise_value)++;
                    }
                }

            } else if (strcmp(argv[a], "-m") == 0) {
                if (a + 1 >= argc) {
                    mp_hal_stdout_tx_str("micropython: -m requires an argument\r\n");
                    exit_code = 2;
                    ran_something = true;
                    break;
                }
                a++;
                set_sys_argv(argv, argc, a);
                mp_hal_set_interrupt_char(CHAR_CTRL_C);
                nlr_buf_t nlr;
                if (nlr_push(&nlr) == 0) {
                    mp_obj_t import_args[4];
                    import_args[0] = mp_obj_new_str(argv[a], strlen(argv[a]));
                    import_args[1] = import_args[2] = mp_const_none;
                    // mp_const_false as fromlist: return leaf module, mark as __main__
                    import_args[3] = mp_const_false;
                    mp_builtin___import__(4, import_args);
                    mp_hal_set_interrupt_char(-1);
                    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
                    nlr_pop();
                } else {
                    mp_hal_set_interrupt_char(-1);
                    mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_CLEAR_EXCEPTIONS);
                    // SystemExit exits with the value as the exit code; everything
                    // else prints the traceback and exits 1. Mirrors the path in
                    // shared/runtime/pyexec.c that handles -c / script files.
                    mp_obj_t exc = MP_OBJ_FROM_PTR(nlr.ret_val);
                    if (mp_obj_is_subclass_fast(MP_OBJ_FROM_PTR(((mp_obj_base_t *)nlr.ret_val)->type),
                            MP_OBJ_FROM_PTR(&mp_type_SystemExit))) {
                        mp_obj_t val = mp_obj_exception_get_value(exc);
                        if (val == mp_const_none) {
                            exit_code = 0;
                        } else if (mp_obj_is_int(val)) {
                            exit_code = (int)mp_obj_int_get_truncated(val);
                        } else {
                            mp_obj_print_helper(&mp_plat_print, val, PRINT_STR);
                            mp_print_str(&mp_plat_print, "\n");
                            exit_code = 1;
                        }
                    } else {
                        mp_obj_print_exception(&mp_plat_print, exc);
                        exit_code = 1;
                    }
                }
                ran_something = true;
                break;

            } else {
                mp_hal_stdout_tx_str("micropython: unknown option '");
                mp_hal_stdout_tx_str(argv[a]);
                mp_hal_stdout_tx_str("'\r\nUse -h for help.\r\n");
                exit_code = 2;
                ran_something = true;
                break;
            }

        } else {
            // Positional argument: treat as a script file.
            // Add the script's directory to the front of sys.path so
            // `from sibling_module import ...` works. If the script
            // was passed by basename, the directory component is
            // empty — fall back to the cwd (matches CPython, which
            // sets sys.path[0] to the script's resolved absolute dir).
            char dir[256];
            path_dir(argv[a], dir, sizeof(dir));
            if (!dir[0]) {
                struct Process *me_p = (struct Process *)FindTask(NULL);
                if (!NameFromLock(me_p->pr_CurrentDir, (STRPTR)dir, sizeof(dir))) {
                    dir[0] = '\0';
                }
            }
            if (dir[0]) {
                mp_obj_list_store(mp_sys_path, MP_OBJ_NEW_SMALL_INT(0),
                    mp_obj_new_str(dir, strlen(dir)));
            }
            set_sys_argv(argv, argc, a);
            exit_code = pyexec_file(argv[a]);
            ran_something = true;
            break;
        }
    }

    if (!ran_something && wb_tt_script != NULL && wb_tt_script[0] != '\0') {
        // Workbench launch with SCRIPT=<path> tooltype. Run that script as
        // if it had been given on the command line. The argv loop above
        // doesn't see it because we never inject a positional, so we mirror
        // its behaviour here. sys.argv ends up as ["micropython"] (no
        // user-supplied script args — selected files come back via
        // amiga.wb_selected_files()).
        char dir[256];
        path_dir(wb_tt_script, dir, sizeof(dir));
        if (!dir[0]) {
            struct Process *me_p = (struct Process *)FindTask(NULL);
            if (!NameFromLock(me_p->pr_CurrentDir, (STRPTR)dir, sizeof(dir))) {
                dir[0] = '\0';
            }
        }
        if (dir[0]) {
            mp_obj_list_store(mp_sys_path, MP_OBJ_NEW_SMALL_INT(0),
                mp_obj_new_str(dir, strlen(dir)));
        }
        exit_code = pyexec_file(wb_tt_script);
        ran_something = true;
    }

    if (!ran_something) {
        // No script or command: start the interactive REPL.
        // Switch console to raw mode so readline gets characters immediately
        // rather than waiting for a full line. readline.c handles echo itself.
        BPTR stdin_fh = Input();
        SetMode(stdin_fh, 1);
        #if MICROPY_ENABLE_COMPILER
        pyexec_friendly_repl();
        #endif
        // Restore cooked mode before returning to the CLI.
        SetMode(stdin_fh, 0);
    }

    #if MICROPY_PY_SYS_ATEXIT
    if (mp_obj_is_callable(MP_STATE_VM(sys_exitfunc))) {
        mp_call_function_0(MP_STATE_VM(sys_exitfunc));
    }
    #endif

    #if MICROPY_PY_AMIGA_SOCKET
    extern void amiga_socket_close(void);
    amiga_socket_close();
    #endif

    #if MICROPY_VFS
    extern void vfs_amiga_cleanup(void);
    vfs_amiga_cleanup();
    #endif

    mp_deinit();

    // Tear down timer.device. Done after mp_deinit so any Python code run
    // up to this point (sys.exitfunc, VFS/socket close hooks) still sees a
    // working time module.
    amiga_timer_close();

    // Free every still-tracked heap chunk. The GC's sweep auto-frees
    // empty grown areas during normal operation (see gc_sweep_free_blocks
    // under MICROPY_GC_SPLIT_HEAP_AUTO), so the only chunks left here are
    // the initial one plus any grown chunks still holding live data at
    // shutdown. amiga_free_heap() goes through FreeVec().
    for (size_t i = 0; i < AMIGA_HEAP_MAX_CHUNKS; i++) {
        if (amiga_heap_chunks[i].ptr != NULL) {
            FreeVec(amiga_heap_chunks[i].ptr);
            amiga_heap_chunks[i].ptr = NULL;
            amiga_heap_chunks[i].bytes = 0;
        }
    }
    amiga_heap_total_bytes = 0;
    if (argv_buf) {
        FreeVec(argv_buf);
        FreeVec(argv);
    }

    // Tear down Workbench-launch resources. The WBStartup message itself
    // is Forbid()+ReplyMsg'd by bebbo crt0 just before our task exits, so
    // we don't touch it here; only the icon-library bits and CON: are ours.
    if (amiga_wb_diskobject != NULL) {
        FreeDiskObject(amiga_wb_diskobject);
        amiga_wb_diskobject = NULL;
    }
    if (IconBase != NULL) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }
    if (amiga_wb_console != 0) {
        // Detach from the process before Close() so the trailing FreeVec
        // / library-close path doesn't try to write into a freed handle.
        struct Process *me = (struct Process *)FindTask(NULL);
        me->pr_CIS = 0;
        me->pr_COS = 0;
        Close(amiga_wb_console);
        amiga_wb_console = 0;
    }
    return exit_code & 0xff;
}

void nlr_jump_fail(void *val) {
    (void)val;
    for (;;) {
    }
}

void MP_NORETURN __fatal_error(const char *msg) {
    (void)msg;
    for (;;) {
    }
}

#ifndef NDEBUG
void MP_WEAK __assert_func(const char *file, int line, const char *func, const char *expr) {
    mp_hal_stdout_tx_strn("Assertion '", 11);
    mp_hal_stdout_tx_strn(expr, strlen(expr));
    mp_hal_stdout_tx_strn("' failed\r\n", 10);
    __fatal_error("Assertion failed");
}
#endif
