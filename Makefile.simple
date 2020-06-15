# Enables debug flags and asserts
DEBUG := 0

# Which version of OpenGL to support
# GL, GLES2, GLES3
#USE_GL :=

# Enable high quality texture features with GLideNHQ
USE_HQTEX := 1

# Custom optimisations
#OPT :=

# Specify target architecture
USE_ARCH := $(shell uname -m)

.PHONY: clean

TARGET_NAME := mini64
TARGET := $(TARGET_NAME)_libretro.so

ROOT_DIR := .
LIBRETRO_DIR := $(ROOT_DIR)/libretro
DEPSDIR	:=	$(CURDIR)/

AWK       ?= awk
STRINGS   ?= strings
TR        ?= tr

# Checks if the given library is available for linking. Works with GCC and
# Clang.
IS_LIB_AVAIL = $(shell $(CC) -l$(CHECK_LIB) 2>&1 >/dev/null | grep "cannot find" > /dev/null; echo $$?)

# Check for the availability of GL libs.
CHECK_LIB   := opengl32
ifeq ($(IS_LIB_AVAIL),1)
    USE_GL ?= GL
    platform := win
endif

CHECK_LIB   := GL
ifeq ($(IS_LIB_AVAIL),1)
    USE_GL ?= GL
endif

CHECK_LIB   := GLESv2
ifeq ($(IS_LIB_AVAIL),1)
    USE_GL ?= GLES2
endif

CHECK_LIB   := brcmGLESv2
ifeq ($(IS_LIB_AVAIL),1)
    USE_GL ?= brcmGLES2
endif

ifeq ($(USE_GL),)
    err := $(error A compatible version of OpenGL was not found)
endif

# Prioritise OpenGL before OpenGLES if both are available.
ifeq ($(USE_GL),GL)
    ifeq ($(platform),win)
        # Set to OpenGL, but link to Windows specific library
        LDLIBS += -lopengl32
    else
        LDLIBS += -lGL
    endif
else ifeq ($(USE_GL),GLES2)
    LDLIBS += -lGLESv2 -lEGL
else ifeq ($(USE_GL),brcmGLES2)
    LDLIBS += -lbrcmGLESv2 -lbrcmEGL -ldl
    CFLAGS += -I/opt/vc/include -I/opt/vc/include/interface/vcos \
        -I/opt/vc/include/interface/vcos/pthreads
else
    err := $(error Either OpenGL or OpenGLES2 is required, but neither could be found)
endif

ifneq ($(USE_HQTEX),1)
    CFLAGS += -DNODHQ
endif

# Assume platform is unix if not set to win previously, or forced by user.
platform ?= unix

ifeq ($(platform), unix)
	LDLIBS += -lpthread
endif

# Target Dynarec
ifeq ($(USE_ARCH), $(filter $(USE_ARCH), i386 i686))
	USE_DYNAREC := x86
else ifeq ($(USE_ARCH), $(filter $(USE_ARCH), x86_64 x64))
	USE_DYNAREC := x64
else ifeq ($(USE_ARCH), $(filter $(USE_ARCH), arm))
	USE_DYNAREC := arm
else ifeq ($(USE_ARCH), $(filter $(USE_ARCH), aarch64))
	USE_DYNAREC := arm64
endif

include Makefile.common

GIT_VERSION := $(shell git rev-parse --short HEAD 2>/dev/null)
ifneq ($(GIT_VERSION),)
   COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

ifeq ($(platform), win)
   ifeq ($(USE_ARCH), x86_64)
      ASFLAGS := -f win64 -d WIN64
   else
      ASFLAGS := -f win32 -f WIN32 -d LEADING_UNDERSCORE
   endif
else
   ifeq ($(USE_ARCH), x86_64)
      ASFLAGS := -f elf64 -d ELF_TYPE
   else
      ASFLAGS := -f elf -d ELF_TYPE
   endif
endif

ifeq ($(DEBUG),1)
	OPT := -Og
else
	OPT := -O3
endif

ifneq ("$(wildcard $(SOURCES_NASM:.asm=.o))","")
    CFLAGS += -flto
else
    $(warning LTO is disabled on clean build.)
endif

LDLIBS += -shared -Wl,--version-script=$(LIBRETRO_DIR)/link.T -Wl,--no-undefined -lz
CFLAGS += -DOS_LINUX -fPIC -g2 $(OPT)

OBJECTS  := $(SOURCES_C:.c=.o) $(SOURCES_CXX:.cpp=.o) $(SOURCES_ASM:.S=.o) $(SOURCES_NASM:.asm=.o)
CXXFLAGS := $(CFLAGS)

# set C/C++ standard to use
CFLAGS += -std=gnu11
CXXFLAGS += -std=gnu++11

# Required defines
override CFLAGS += -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -D__LIBRETRO__ \
    -DM64P_PLUGIN_API -DM64P_CORE_PROTOTYPES -DTXFILTER_LIB -D__VEC4_OPT \
    -DMUPENPLUSAPI
override CXXFLAGS += -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS -D__LIBRETRO__ \
    -DM64P_PLUGIN_API -DM64P_CORE_PROTOTYPES -DTXFILTER_LIB -D__VEC4_OPT \
    -DMUPENPLUSAPI

all: $(TARGET)
$(TARGET): $(OBJECTS)
	$(CXX) -o $@ $^ $(LDLIBS)

# Script hackery fll or generating ASM include files for the new dynarec assembly code
$(AWK_DEST_DIR)/asm_defines_gas.h: $(AWK_DEST_DIR)/asm_defines_nasm.h
$(AWK_DEST_DIR)/asm_defines_nasm.h: $(ASM_DEFINES_OBJ)
	$(STRINGS) "$<" | $(TR) -d '\r' | $(AWK) -v dest_dir="$(AWK_DEST_DIR)" -f $(CORE_DIR)/tools/gen_asm_defines.awk

%.o: %.asm $(AWK_DEST_DIR)/asm_defines_gas.h
	nasm -i$(AWK_DEST_DIR)/ $(ASFLAGS) $< -o $@

%.o: %.S $(AWK_DEST_DIR)/asm_defines_gas.h
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	@echo "CC $@"
	@$(CC) -c $(CPPFLAGS) $(CFLAGS) $< -o $@

%.o: %.cpp
	@echo "CXX $@"
	@$(CXX) -c $(CPPFLAGS) $(CXXFLAGS) $< -o $@

clean:
	$(RM) $(OBJECTS)
	$(RM) $(OBJECTS:.o=.gcda)
	$(RM) $(TARGET)

# 80char      |-------------------------------------------------------------------------------|
help:
	@echo "Available options and their descriptions when enabled:"
	@echo "  DEBUG=$(DEBUG)"
	@echo "          Enable all asserts and reduces optimisation."
	@echo
	@echo "  USE_HQTEX=$(USE_HQTEX)"
	@echo "          Enable texture pack support and features of GLideNHQ."
	@echo
	@echo "  USE_GL=$(USE_GL)"
	@echo "          Specify a specific version of OpenGL to use."
	@echo "          Supported options are: GL GLES2"
	@echo "          An attempt is made to automatically detect the best version of GL"
	@echo "          for the target platform if this option is not manually specified."
	@echo
	@echo "  platform=$(platform)"
	@echo "          Selects the target platform."
	@echo "          Supported options are: unix win"
	@echo "          An attempt is made to automatically detect the target platform"
	@echo "          if this option is not manually specified."
	@echo
	@echo "  OPT=$(OPT)"
	@echo "          Set additional compiler flags, such as optimisations."
	@echo
	@echo "  Example: make DEBUG=1 OPT=\"-Ofast -march=native\""
	@echo
	@echo
	@echo "Mini64, Mupen64plus, and GLideN64 are all free software; see the LICENSE "
	@echo "file for copying conditions. There is NO warranty; not even for"
	@echo "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE."
	@echo ""
