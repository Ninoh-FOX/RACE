LOCAL_PATH := $(call my-dir)

DRZ80                    := 0
DEBUG                    := 0
FLAGS                    :=

CORE_DIR := $(LOCAL_PATH)/..

include $(CORE_DIR)/Makefile.common

COREFLAGS := $(INCFLAGS) $(FLAGS) -DWANT_CRC32
COREFLAGS += -D__LIBRETRO__ -D_MAX_PATH=2048

GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
  COREFLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

# Dual link: the hidden Neo Geo Pocket (instance B) is the core built again
# with -DRACE_INSTANCE_B (without the frontend side), linked into one object
# with every global it defines renamed ngpb_* (as the Makefile does).
include $(CLEAR_VARS)
LOCAL_MODULE       := retro_b
LOCAL_SRC_FILES    := $(RACE_B_SOURCES)
LOCAL_CFLAGS       := $(COREFLAGS) -DRACE_INSTANCE_B
include $(BUILD_STATIC_LIBRARY)
RACE_B_LIB := $(LOCAL_BUILT_MODULE)

RACE_HIDDEN_DIR := $(TARGET_OBJS)/retro_b/hidden
RACE_HIDDEN     := $(RACE_HIDDEN_DIR)/race_b.o

$(RACE_HIDDEN_DIR)/race_b_all.o: $(RACE_B_LIB)
	@mkdir -p $(dir $@)
	$(LLVM_TOOLCHAIN_PREFIX)ld.lld -r --whole-archive $< -o $@

$(RACE_HIDDEN): $(RACE_HIDDEN_DIR)/race_b_all.o
	$(LLVM_TOOLCHAIN_PREFIX)llvm-nm -g --defined-only $< | awk 'NF == 3 { print $$3 " ngpb_" $$3 }' > $@.syms
	$(LLVM_TOOLCHAIN_PREFIX)llvm-objcopy --redefine-syms=$@.syms $< $@

include $(CLEAR_VARS)
LOCAL_MODULE       := retro
LOCAL_SRC_FILES    := $(sort $(SOURCES_CXX)) $(SOURCES_C)
LOCAL_CFLAGS       := $(COREFLAGS)
LOCAL_CXXFLAGS     := $(COREFLAGS) -fno-rtti
LOCAL_LDFLAGS      := -Wl,-version-script=$(CORE_DIR)/libretro/link.T $(RACE_HIDDEN)
LOCAL_CPP_FEATURES := exceptions
include $(BUILD_SHARED_LIBRARY)
$(LOCAL_BUILT_MODULE): $(RACE_HIDDEN)
