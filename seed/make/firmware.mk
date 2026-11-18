TARGET = passthrough
CPP_SOURCES = src/main.cpp src/audio/nam_processor.cpp src/audio/nam_audio.cpp src/audio/cab_filter.cpp src/audio/output_controls.cpp src/models/amp_models.cpp src/models/a2_lite.cpp
C_INCLUDES = -Isrc -Ibuild/generated
CPP_STANDARD = -std=gnu++20
# Optimize application and model code for speed; libDaisy builds with -Os.
OPT = -O3
LIBDAISY_DIR = libs/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
APP_TYPE = BOOT_NONE
# make link: adds the touchscreen link (SEED_LINK). Its protocol code is the touchscreen repo's own
# (firmware/main/seed_link_proto), compiled unchanged, so both ends always match.
ifdef SEED_LINK
SLP_DIR = ../firmware/main/seed_link_proto
C_SOURCES += $(SLP_DIR)/seed_link_proto.c $(SLP_DIR)/slp_receiver.c
C_INCLUDES += -I$(SLP_DIR)
# make link-fx: adds the six effects (SEED_FX), the touchscreen repo's seed-dsp/effects. They
# don't fit the 128 KB internal flash, so that build runs under the Daisy bootloader.
ifdef SEED_FX
FX_DIR = ../seed-dsp
CPP_SOURCES += $(FX_DIR)/effects.cpp $(FX_DIR)/tuner.cpp
C_INCLUDES += -I$(FX_DIR)
# The tuner too: the receiver's SET_TUNER hook (C and C++ must agree on its config struct).
C_DEFS += -DSLP_RX_TUNER
endif
endif
include $(SYSTEM_FILES_DIR)/Makefile

# LoadAmpModel catches a failed load so audio still starts in bypass. On the
# board that never covers running out of heap: newlib-nano's libstdc++ is
# built without exceptions, so operator new (main.cpp) aborts instead of
# throwing bad_alloc. Keep IEEE floating-point behavior until model accuracy
# and target performance have been measured.
CPPFLAGS += -fexceptions
include make/warnings.mk
CPPFLAGS += $(FIRMWARE_WARNINGS)
# nano.specs memcpy and memset copy one byte per iteration. Keep GCC from
# turning A2-Lite's per-call weight copies and per-block fills into calls
# to them; plain loops compile to word loads and stores.
$(BUILD_DIR)/a2_lite.o: CPPFLAGS += -fno-tree-loop-distribute-patterns

# make upload PROFILE=1 adds A2-Lite per-layer cycle counters, printed every 5 s.
# Objects don't track flags; run make clean when toggling it.
ifdef PROFILE
CPPFLAGS += -DA2_LITE_PROFILE
endif
ifdef SEED_LINK
CPPFLAGS += -DSEED_LINK
ifdef SEED_FX
CPPFLAGS += -DSEED_FX
endif
# The link's protocol code runs in the main loop, never per sample: built small, so the firmware
# still fits the 128 KB internal flash.
$(BUILD_DIR)/seed_link_proto.o $(BUILD_DIR)/slp_receiver.o: OPT = -Os
endif
include make/models.mk

include make/flash_dfu.mk

# Used in dry-run mode to generate clangd commands without requiring a link.
.PHONY: compile-objects
compile-objects: $(OBJECTS)

$(OBJECTS): make/firmware.mk
$(BUILD_DIR)/amp_models.o: $(A2_HEADER)
$(BUILD_DIR)/$(TARGET).elf: make/firmware.mk $(LIBDAISY_DIR)/build/libdaisy.a
