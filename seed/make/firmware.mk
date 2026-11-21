TARGET = passthrough
CPP_SOURCES = src/main.cpp src/audio/nam_processor.cpp src/audio/nam_audio.cpp src/audio/cab_filter.cpp src/audio/output_controls.cpp src/models/amp_models.cpp src/models/a2_lite.cpp
C_INCLUDES = -Isrc -Ibuild/generated
CPP_STANDARD = -std=gnu++20
# Optimize application and model code for speed; libDaisy builds with -Os.
OPT = -O3
LIBDAISY_DIR = libs/libDaisy
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
# The firmware is too big for the 128 KB internal flash, so it runs under the Daisy bootloader: the
# app lives in the QSPI flash and the bootloader copies it into the AXI SRAM at power-up
# (scripts/flash_boot.sh installs the bootloader and flashes the app).
APP_TYPE = BOOT_SRAM
LDSCRIPT = make/STM32H750IB_sram_app.lds
# The touchscreen link: its protocol code is the touchscreen's own
# (../touchscreen/firmware/main/seed_link_proto), compiled unchanged, so both ends always match.
SLP_DIR = ../touchscreen/firmware/main/seed_link_proto
C_SOURCES += $(SLP_DIR)/seed_link_proto.c $(SLP_DIR)/slp_receiver.c
C_INCLUDES += -I$(SLP_DIR)
# The six effects and the tuner, the touchscreen's seed-dsp code, also compiled unchanged.
FX_DIR = ../touchscreen/seed-dsp
CPP_SOURCES += $(FX_DIR)/effects.cpp $(FX_DIR)/tuner.cpp
C_INCLUDES += -I$(FX_DIR)
# The receiver's SET_TUNER hook (C and C++ must agree on its config struct).
C_DEFS += -DSLP_RX_TUNER
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
include make/models.mk


# Used in dry-run mode to generate clangd commands without requiring a link.
.PHONY: compile-objects
compile-objects: $(OBJECTS)

$(OBJECTS): make/firmware.mk
$(BUILD_DIR)/amp_models.o: $(A2_HEADER)
$(BUILD_DIR)/$(TARGET).elf: make/firmware.mk $(LIBDAISY_DIR)/build/libdaisy.a
