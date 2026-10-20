LOCAL_PATH := $(call my-dir)

CORE_DIR := $(LOCAL_PATH)/..

include $(CLEAR_VARS)
LOCAL_MODULE    := retro
LOCAL_SRC_FILES := \
	$(CORE_DIR)/src/flashem_libretro.c \
	$(CORE_DIR)/src/vflash.c \
	$(CORE_DIR)/src/hw.c \
	$(CORE_DIR)/src/zevio_dsp.c \
	$(CORE_DIR)/src/zsp400.c \
	$(CORE_DIR)/src/midi.c \
	$(CORE_DIR)/src/cdda_dma.c \
	$(CORE_DIR)/src/arm9.c \
	$(CORE_DIR)/src/cp15.c \
	$(CORE_DIR)/src/cdrom.c \
	$(CORE_DIR)/src/cdsp.c \
	$(CORE_DIR)/src/ge.c \
	$(CORE_DIR)/src/audio.c \
	$(CORE_DIR)/src/disasm.c
LOCAL_C_INCLUDES := $(CORE_DIR)/src
LOCAL_CFLAGS    := -O2 -DNDEBUG -DFLASHEM_NO_SDL
LOCAL_CONLYFLAGS := -std=gnu11
LOCAL_LDFLAGS   := -Wl,-version-script=$(CORE_DIR)/link.T -Wl,-Bsymbolic
LOCAL_LDLIBS    := -lm

# The JIT on the 64-bit ABIs dynarmic has backends for, when DYNARMIC_ANDROID
# holds a tools/build_dynarmic.sh build per ABI (<dir>/<abi>/libs.txt)
ifneq (,$(DYNARMIC_ANDROID))
ifneq (,$(filter arm64-v8a x86_64,$(TARGET_ARCH_ABI)))
LOCAL_SRC_FILES    += $(CORE_DIR)/src/jit.cpp
LOCAL_CFLAGS       += -DFLASHEM_JIT
LOCAL_CPPFLAGS     += -std=c++20
LOCAL_CPP_FEATURES := exceptions rtti
LOCAL_C_INCLUDES   += $(CORE_DIR)/external/dynarmic/src $(CORE_DIR)/external/dynarmic/externals/mcl/include
LOCAL_LDFLAGS      += -Wl,--start-group $(shell cat $(DYNARMIC_ANDROID)/$(TARGET_ARCH_ABI)/libs.txt) -Wl,--end-group
endif
endif
include $(BUILD_SHARED_LIBRARY)
