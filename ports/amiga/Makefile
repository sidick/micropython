# Select the variant to build for.
ifdef VARIANT_DIR
VARIANT ?= $(notdir $(VARIANT_DIR:/=))
else
VARIANT ?= standard
VARIANT_DIR ?= variants/$(VARIANT)
endif

ifeq ($(wildcard $(VARIANT_DIR)/.),)
$(error Invalid VARIANT specified: $(VARIANT_DIR))
endif

# Build directory tracks the variant name unless overridden.
BUILD ?= build-$(VARIANT)
PROG  = micropython

include ../../py/mkenv.mk

# Cross-compile with bebbo GCC (https://franke.ms/git/bebbo/amiga-gcc).
# The toolchain must be installed with 'make all' or 'make clib2 ndk' —
# 'make min' alone does not include C library headers and will not compile.
CROSS_COMPILE = m68k-amigaos-

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h
QSTR_GLOBAL_DEPENDENCIES += $(VARIANT_DIR)/mpconfigvariant.h

# Frozen Python modules: ports/amiga/modules/amiga.py wraps the
# C-side _amiga module and exposes the Library proxy on top of
# the .fd-derived signature table in modules/_amiga_fd.py.
FROZEN_MANIFEST ?= manifest.py
# mpy-tool defaults to encoding integer literals as MPZ; this port
# uses MICROPY_LONGINT_IMPL_LONGLONG so the frozen content must
# match or the static_assert in frozen_content.c trips.
MPY_TOOL_FLAGS ?= -mlongint-impl=longlong

# Variant-supplied knobs. Each variant's mpconfigvariant.mk defines:
#   AMIGA_CPU_FLAGS    — CPU/FPU selection, e.g. '-m68020 -msoft-float'
#   AMIGA_FLOAT_WRAPS  — soft-float libgcc wraps (empty for FPU variants)
# The libm wraps below apply unconditionally — those are clib2/libnix
# math-lib bugs that aren't FPU-related.
include $(VARIANT_DIR)/mpconfigvariant.mk

# include py core make definitions
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

# Note: bebbo gcc 6.5 ICEs on -malign-int (its global 32-bit alignment
# flag), so struct alignment for mp_obj_t (which requires 4-byte alignment
# for the tagged-pointer encoding) is enforced via MICROPY_OBJ_BASE_ALIGNMENT
# in mpconfigport.h.

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)
INC += -I$(VARIANT_DIR)

CFLAGS += $(INC) $(AMIGA_CPU_FLAGS) -Wall -Wextra -Wno-unused-parameter -std=gnu99

CFLAGS += -g
ifeq ($(DEBUG), 1)
CFLAGS += -O0
else
CFLAGS += -Os -DNDEBUG -fdata-sections -ffunction-sections
endif

LDFLAGS += $(AMIGA_CPU_FLAGS) -lm

# clib2's __eqdf2.o packs every double-comparison helper together with
# __muldf3 / __floatsidf / etc, so we can't simply redefine the broken
# comparison functions in floatconv.c (multi-def at link time when
# clib2 pulls in __eqdf2.o for __muldf3). --wrap diverts the calls to
# our amiga_dbl_* implementations without us having to take over the
# rest of the object. See ports/amiga/floatconv.c for the why.
# FPU variants skip this (gcc emits FPU compare instructions directly).
LDFLAGS += $(AMIGA_FLOAT_WRAPS)
# libnix pow(-1, NaN) returns 1.0 (CPython expects NaN) and tgamma(-inf)
# returns +inf (CPython raises ValueError). See floatconv.c. These are
# math-library bugs, not soft-float bugs — applies to all variants.
LDFLAGS += -Wl,--wrap=pow,--wrap=tgamma

# AmiSSL v5 TLS (Phase 28). The minimal variant overrides this to 0;
# every other variant inherits the default and links -lamisslstubs.
# The SDK headers and the libamisslstubs.a archive come from the
# host cache populated by tools/amiga-fetch-amissl-sdk.sh and bind-
# mounted into the container at /amissl-cache. Override
# AMISSL_SDK_DEV when building natively against a different install.
MICROPY_PY_AMIGA_SSL ?= 1
AMISSL_SDK_DEV ?= /amissl-cache/latest/AmiSSL/Developer

ifeq ($(MICROPY_PY_AMIGA_SSL),1)
ifeq ($(wildcard $(AMISSL_SDK_DEV)/include/openssl/ssl.h),)
$(error MICROPY_PY_AMIGA_SSL=1 but AmiSSL SDK not found at \
  $(AMISSL_SDK_DEV)/include/openssl/ssl.h. Run tools/amiga-fetch-amissl-sdk.sh \
  first, override AMISSL_SDK_DEV, or pass MICROPY_PY_AMIGA_SSL=0)
endif
CFLAGS += -I$(AMISSL_SDK_DEV)/include -DMICROPY_PY_AMIGA_SSL=1
LDFLAGS += -L$(AMISSL_SDK_DEV)/lib/AmigaOS3 -lamisslstubs
else
CFLAGS += -DMICROPY_PY_AMIGA_SSL=0
endif

# Allow callers (CI, manual invocations) to inject extra include /
# library paths without editing the Makefile. Mirrors the convention
# in ports/unix.
CFLAGS  += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

# shared/libc/printf.c uses __attribute__((alias)) which bebbo GCC doesn't
# support on AmigaOS. clib2 provides vsnprintf natively so drop this file.
SRC_EXTMOD_C := $(filter-out shared/libc/printf.c, $(SRC_EXTMOD_C))

# extmod/modjson.c uses a stack-allocated mp_obj_stringio_t passed through
# the stream protocol; on 68k this fails because mp_obj_is_obj() requires
# 4-byte pointer alignment not guaranteed for all stack frames by the SysV
# ABI. Use a port-local replacement that reads directly from a buffer.
SRC_EXTMOD_C := $(filter-out extmod/modjson.c, $(SRC_EXTMOD_C))

SRC_C = \
	main.c \
	mphalport.c \
	amiga_history.c \
	amiga_timer.c \
	modjson.c \
	modamiga.c \
	vfs_amiga.c \
	modsocket.c \
	sysstdio.c \
	floatconv.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \
	shared/runtime/interrupt_char.c \

ifeq ($(MICROPY_PY_AMIGA_SSL),1)
SRC_C += amiga_ssl.c
endif

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c vfs_amiga.c modjson.c modamiga.c modsocket.c sysstdio.c

SRC_S = \
	amiga_lib_call.S \

OBJ = $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))
OBJ += $(addprefix $(BUILD)/, $(SRC_S:.S=.o))

all: $(BUILD)/$(PROG)

$(BUILD)/$(PROG): $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
