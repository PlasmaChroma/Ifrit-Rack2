# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../Rack-SDK

# FLAGS will be passed to both the C and C++ compiler
FLAGS += -I dep/vst3sdk -I src
CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS += -ldl -lX11

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard src/host/*.cpp)
SOURCES += $(wildcard src/host/vst3/*.cpp)
SOURCES += $(wildcard src/modules/*.cpp)
SOURCES += $(wildcard src/widgets/*.cpp)
SOURCES += dep/vst3sdk/pluginterfaces/base/funknown.cpp
SOURCES += dep/vst3sdk/pluginterfaces/base/coreiids.cpp
SOURCES += dep/vst3sdk/public.sdk/source/vst/vstinitiids.cpp
SOURCES += dep/vst3sdk/public.sdk/source/common/commoniids.cpp
SOURCES += $(wildcard src/doom/*.c)


# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

# Force C++17 to support std::filesystem
CXXFLAGS += -std=c++17

# Rack SDK 2.5 adds this Clang-only option globally. GCC emits a note for it
# on every translation unit, so remove it after the SDK has assembled FLAGS.
FLAGS := $(filter-out -Wno-vla-extension,$(FLAGS))

# Chocolate Doom is vendored C89-era code. Keep the normal warning policy for
# Ifrit, while suppressing only its documented legacy warning classes.
DOOM_LEGACY_WARN_FLAGS := \
	-Wno-sign-compare \
	-Wno-implicit-fallthrough \
	-Wno-unused-but-set-parameter \
	-Wno-missing-field-initializers \
	-Wno-dangling-pointer \
	-Wno-stringop-truncation \
	-Wno-enum-conversion \
	-Wno-absolute-value

build/src/doom/%.c.o: CFLAGS += $(DOOM_LEGACY_WARN_FLAGS)
