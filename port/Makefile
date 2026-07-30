# Dead Space loader.
#
# Always cross-compiles to arm-linux-gnueabihf: the R36S runs an aarch64
# kernel, but the only native library the game ships is armeabi-v7a, and the
# bionic ELF loader has to map it into a process of the same word size.
#
# Sources are picked up by wildcard on purpose — the loader, the JNI shim and
# the libc thunks all land as new directories in later milestones and must not
# each require a Makefile edit.

ARCH       ?= arm-linux-gnueabihf
CROSS      ?= $(ARCH)-
CXX        := $(CROSS)g++
PKG_CONFIG ?= $(CROSS)pkg-config

TARGET  := build/deadspace
OBJDIR  := build/obj

PKGS := sdl2 zlib libzip

OPT  ?= -O2 -g
WARN := -Wall -Wextra -Wno-unused-parameter -Werror=return-type

# thunks/libc/generated holds the bionic export tables produced by
# tools/generate_libc.sh. They are checked in because the build image has no
# clang; see that script for why.
INCLUDES := -I. -Isrc -Iandroid -Iloader -Ithunks -Ithunks/libc \
            -Ithunks/libc/generated -Ijni
CPPFLAGS := $(INCLUDES) $(shell $(PKG_CONFIG) --cflags $(PKGS)) -MMD -MP
# gnu++20: the vendored ELF loader uses std::string::starts_with.
CXXFLAGS := -std=gnu++20 $(OPT) $(WARN) -fno-strict-aliasing -fuse-cxa-atexit
LDFLAGS  := $(OPT)
LDLIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS)) -pthread -lm -ldl -lrt -lbsd

SRCDIRS := src android loader thunks/libc thunks/khronos jni jni/classes
SRCS    := $(foreach d,$(SRCDIRS),$(wildcard $(d)/*.cpp))
OBJS    := $(patsubst %.cpp,$(OBJDIR)/%.cpp.o,$(SRCS))
DEPS    := $(OBJS:.o=.d)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OBJDIR)/%.cpp.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

# The .so files the zip has to carry. Generated, never checked in: they are
# Debian binaries and tools/collect_libs.sh reproduces them exactly.
libs: $(TARGET)
	tools/collect_libs.sh $(TARGET) build/libs.armhf

clean:
	rm -rf build

.PHONY: all clean libs

-include $(DEPS)
