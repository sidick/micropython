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

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c vfs_amiga.c modjson.c modamiga.c modsocket.c sysstdio.c

OBJ = $(PY_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

all: $(BUILD)/$(PROG)

$(BUILD)/$(PROG): $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
