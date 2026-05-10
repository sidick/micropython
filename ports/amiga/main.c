#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <dos/dos.h>
#include <proto/dos.h>

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

static void *heap_ptr;

#if MICROPY_ENABLE_GC
void gc_collect(void) {
    // Scan the live portion of the task stack: from the current SP (approximated
    // by a local variable) up to tc_SPUpper (the top of the stack allocation).
    void *dummy;
    struct Task *task = FindTask(NULL);
    gc_collect_start();
    gc_collect_root(&dummy,
        ((char *)task->tc_SPUpper - (char *)&dummy) / sizeof(void *));
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

int main(int argc, char **argv) {
    // Allocate heap from Fast RAM; fall back to any available RAM.
    heap_ptr = AllocVec(MICROPY_HEAP_SIZE, MEMF_FAST | MEMF_PUBLIC);
    if (!heap_ptr) {
        heap_ptr = AllocVec(MICROPY_HEAP_SIZE, MEMF_ANY | MEMF_PUBLIC);
    }
    if (!heap_ptr) {
        return 1;
    }

    #if MICROPY_STACK_CHECK
    mp_stack_ctrl_init();
    mp_stack_set_limit(40 * 1024);
    #endif

    #if MICROPY_ENABLE_GC
    gc_init(heap_ptr, (char *)heap_ptr + MICROPY_HEAP_SIZE);
    #endif

    mp_init();

    // sys.path: "" means the current directory on AmigaOS.
    mp_sys_path = mp_obj_new_list(0, NULL);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    // sys.argv starts empty; populated below based on what is being run.
    mp_obj_list_init(MP_OBJ_TO_PTR(mp_sys_argv), 0);

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
                    mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
                    exit_code = 1;
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
            // Add the script's directory to the front of sys.path.
            char dir[256];
            path_dir(argv[a], dir, sizeof(dir));
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

    #if MICROPY_PY_AMIGA_SOCKET
    extern void amiga_socket_close(void);
    amiga_socket_close();
    #endif

    mp_deinit();
    FreeVec(heap_ptr);
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
