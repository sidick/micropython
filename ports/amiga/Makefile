include ../../py/mkenv.mk

# Cross-compile with bebbo GCC (https://franke.ms/git/bebbo/amiga-gcc).
# The toolchain must be installed with 'make all' or 'make clib2 ndk' —
# 'make min' alone does not include C library headers and will not compile.
CROSS_COMPILE = m68k-amigaos-

# qstr definitions (must come before including py.mk)
QSTR_DEFS = qstrdefsport.h

# include py core make definitions
include $(TOP)/py/py.mk
include $(TOP)/extmod/extmod.mk

BUILD ?= build-standard
PROG  = micropython

# Target 68020 minimum; soft-float (no FPU on most Amigas)
CFLAGS_AMIGA = -m68020 -msoft-float

INC += -I.
INC += -I$(TOP)
INC += -I$(BUILD)

CFLAGS += $(INC) $(CFLAGS_AMIGA) -Wall -Wextra -Wno-unused-parameter -std=gnu99

CFLAGS += -g
ifeq ($(DEBUG), 1)
CFLAGS += -O0
else
CFLAGS += -Os -DNDEBUG -fdata-sections -ffunction-sections
endif

LDFLAGS += -m68020

SRC_C = \
	main.c \
	mphalport.c \
	amigafile.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c \

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

OBJ = $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

all: $(BUILD)/$(PROG)

$(BUILD)/$(PROG): $(OBJ)
	$(ECHO) "LINK $@"
	$(Q)$(CC) $(LDFLAGS) -o $@ $^
	$(Q)$(SIZE) $@

include $(TOP)/py/mkrules.mk
