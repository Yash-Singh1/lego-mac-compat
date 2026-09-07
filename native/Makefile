BUILD_DIR := build

# One loader serves every supported title; the bundle targets pick the title
# with GAME=pirates (default), GAME=clonewars, GAME=marvel or GAME=saga.
GAME ?= pirates
ifeq ($(GAME),clonewars)
BUNDLE := $(BUILD_DIR)/LEGOCloneWars-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/lsw3/LEGOCloneWars.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
# Plain i386 Mach-O (no SecuROM wrapper), so the shipped binary is the image.
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGOCloneWars.macbin
# The shipped path may contain spaces, which make cannot take as a
# prerequisite; the recipes test for it instead.
IMAGE_PREREQ :=
IMAGE_NAME := LEGOCloneWars.image
LOADER_NAME := LEGOCloneWarsCompat
INFO_PLIST := Info-CloneWars.plist
# Clone Wars keeps GAME*.DAT in Resources; they arrive with the Resources ditto.
GAME_DATA :=
ICON := icons/LEGOCloneWars.icns
ICON_NAME := LEGOStarWars3.icns
else ifeq ($(GAME),marvel)
BUNDLE := $(BUILD_DIR)/LEGOMarvel-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/marvel/LEGO Marvel Super Heroes.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGOMarvel.macbin
IMAGE_PREREQ :=
IMAGE_NAME := LEGOMarvel.image
LOADER_NAME := LEGOMarvelCompat
INFO_PLIST := Info-Marvel.plist
GAME_DATA := steam_appid.txt
CG_SOURCE := $(SOURCE_CONTENTS)/Frameworks/Cg.framework
ICON := $(SOURCE_CONTENTS)/Resources/GameIcon.icns
ICON_NAME := GameIcon.icns
else ifeq ($(GAME),saga)
SAGA_EDITION ?= steam
ifeq ($(SAGA_EDITION),steam)
BUNDLE := $(BUILD_DIR)/LEGOCompleteSaga-Steam-Compat.app
# The Steam library location must be supplied by the caller.
SOURCE_APP ?=
ifneq ($(filter bundle run-game,$(MAKECMDGOALS)),)
ifeq ($(strip $(SOURCE_APP)),)
$(error Steam Saga requires SOURCE_APP="/path/to/LEGO Star Wars Saga.app")
endif
endif
SAGA_DATA_SOURCE ?= $(SOURCE_APP)/../LEGOStarWarsSagaData
IMAGE_PREREQ :=
ICON := $(SOURCE_APP)/Contents/Resources/GameIcon.icns
ICON_NAME := GameIcon.icns
else ifeq ($(SAGA_EDITION),retail)
BUNDLE := $(BUILD_DIR)/LEGOCompleteSaga-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/lswc/LEGO Star Wars Saga.app
SAGA_UPDATE_ZIP ?= /Volumes/Storage/games/mac/lswc/legostarwarssaga_111_update.zip
ifneq ($(strip $(SAGA_UPDATE_ZIP)),)
# Stage the publisher's update without changing the user's original copy.
# SOURCE_APP on the command line still selects an already updated app.
SOURCE_APP := $(abspath $(BUILD_DIR)/saga-update)/LEGO Star Wars Saga.app
IMAGE_PREREQ := saga-update
else
IMAGE_PREREQ :=
endif
SAGA_DATA_SOURCE ?= $(abspath $(BUILD_DIR))/Data
RESOURCE_FORKS := 1
ICON := $(SOURCE_APP)/Contents/Resources/Saga.icns
ICON_NAME := Saga.icns
else
$(error Unknown SAGA_EDITION "$(SAGA_EDITION)"; choose steam or retail)
endif
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
IMAGE_SOURCE := $(SOURCE_CONTENTS)/MacOS/LEGO Star Wars Saga
IMAGE_NAME := LEGOCompleteSaga.image
LOADER_NAME := LEGOCompleteSagaCompat
INFO_PLIST := Info-Saga.plist
GAME_DATA :=
CG_SOURCE :=
else ifeq ($(GAME),pirates)
BUNDLE := $(BUILD_DIR)/LEGOPirates-Compat.app
SOURCE_APP := /Volumes/Storage/games/mac/LEGOPirates.app
SOURCE_CONTENTS := $(SOURCE_APP)/Contents
# The shipped LEGOPirates.macbin is a SecuROM-packed stub; tools/unpack_securom.py
# recovers the plain Mach-O from it (see the rule below).  Pass IMAGE_SOURCE=
# to use an already unpacked image instead.
PACKED_IMAGE := $(SOURCE_CONTENTS)/MacOS/LEGOPirates.macbin
IMAGE_SOURCE := $(BUILD_DIR)/LEGOPirates.unpacked.macbin
IMAGE_PREREQ := $(IMAGE_SOURCE)
IMAGE_NAME := LEGOPirates.image
LOADER_NAME := LEGOPiratesCompat
INFO_PLIST := Info.plist
GAME_DATA := GAME.DAT GAME0.DAT GAME1.DAT GAME2.DAT GAME3.DAT GAME4.DAT
ICON := icons/LEGOPirates.icns
ICON_NAME := icon.icns
else
$(error Unknown GAME "$(GAME)"; choose pirates, clonewars, marvel or saga)
endif
CG_SOURCE ?= $(SOURCE_CONTENTS)/Library/Frameworks/Cg.framework
# Python for the Pirates unpacker.  By default a venv under $(BUILD_DIR) with
# `unicorn` installed is created on first use (system and Homebrew Pythons
# refuse plain `pip install`s); PYTHON=/path/to/python3 names an interpreter
# that already has unicorn instead.
PYTHON ?=
ifeq ($(PYTHON),)
UNPACK_PYTHON := $(BUILD_DIR)/venv/bin/python
UNPACK_PYTHON_PREREQ := $(UNPACK_PYTHON)
else
UNPACK_PYTHON := $(PYTHON)
UNPACK_PYTHON_PREREQ :=
endif
CLANG := $(shell xcrun --find clang)
SDK := $(shell xcrun --sdk macosx --show-sdk-path)
CFLAGS := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -Wall -Wextra -Werror
LDFLAGS := -Wl,-no_pie,-pagezero_size,0x1000,-segaddr,__TEXT,0x100000000,-segaddr,__LEGACY,0x1000,-segprot,__LEGACY,rwx,rwx

.PHONY: test-game-profile all bundle promote-loader icons clean run run-game controller-probe test-controller test-arb-program-guard test-arb-sampler-usage mode-switch-stress

all: $(BUILD_DIR)/mode32_probe $(BUILD_DIR)/game_loader

$(BUILD_DIR)/controller_probe: src/controller_probe.m | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $^ -framework Foundation -framework GameController -o $@
	codesign --force --sign - $@

controller-probe: $(BUILD_DIR)/controller_probe
	arch -x86_64 ./$(BUILD_DIR)/controller_probe

# Use the existing bundle's image without launching a window or loading saves.
CONTROLLER_TEST_IMAGE ?= $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
test-controller: $(BUILD_DIR)/game_loader
	test -f "$(CONTROLLER_TEST_IMAGE)"
	env LP32_CONTROLLER_SELFTEST=1 LP32_VIRTUAL_CONTROLLERS=2 LP32_VIRTUAL_CONTROLLER_TEST=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(CONTROLLER_TEST_IMAGE)"

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/mode32_probe: src/mode32_probe.c src/mode32_switch.S src/address_space_reserve.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $^ -o $@
	codesign --force --sign - $@

$(BUILD_DIR)/game_loader: src/game_loader.c src/hitch_recorder.c src/game_profile.c src/macho_loader.c src/compat_runtime.c src/audio_bridge.c src/sound_manager_bridge.c src/openal_bridge.c src/hid_bridge.c src/dispatch_bridge.m src/dlfcn_bridge.c src/gl_core_bridge.c src/curl_bridge.c src/steam_bridge.c src/steam_storage_fix.c src/resource_bridge.c src/stl_bridge.c src/libcpp_bridge.c src/libcpp_stream_bridge.cpp src/tlv_bridge.c src/blocks_bridge.c src/zlib_bridge.c src/regex_bridge.c src/rtti_bridge.c src/carbon_bridge.c src/carbon_text.m src/carbon_ui.m src/cfnetwork_bridge.c src/crypto_bridge.c src/quicktime_bridge.m src/controller_bridge.m src/objc_bridge.m src/objc_legacy_bridge.m src/arb_program_guard.c src/arb_sampler_usage.c src/mode32_switch.S src/import_gateway.S src/address_space_reserve.S $(wildcard src/*.h) | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) $(LDFLAGS) $(filter %.c %.m %.cpp %.S,$^) -lc++ -lffi -lcurl -lz -framework CoreText -framework Security -framework ForceFeedback -framework OpenAL -framework CFNetwork -framework SystemConfiguration -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework Cocoa -framework Carbon -framework OpenGL -framework AudioToolbox -framework AudioUnit -framework CoreAudio -framework GameController -framework IOKit -o $@
	codesign --force --sign - $@

$(BUILD_DIR)/test_game_profile: tests/test_game_profile.c src/game_profile.c src/game_profile.h src/macho_loader.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_game_profile.c src/game_profile.c -o $@

test-game-profile: $(BUILD_DIR)/test_game_profile
	./$(BUILD_DIR)/test_game_profile

$(BUILD_DIR)/test_macho_entry: tests/test_macho_entry.c src/macho_loader.c src/macho_loader.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_macho_entry.c -o $@

.PHONY: test-macho-entry
test-macho-entry: $(BUILD_DIR)/test_macho_entry
	./$(BUILD_DIR)/test_macho_entry

.PHONY: test-objc-proxy
test-objc-proxy: $(BUILD_DIR)/game_loader
	env LP32_OBJC_PROXY_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader

$(BUILD_DIR)/test_arb_sampler_usage: tests/test_arb_sampler_usage.c src/arb_sampler_usage.c src/arb_sampler_usage.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_sampler_usage.c src/arb_sampler_usage.c -o $@

$(BUILD_DIR)/test_arb_program_guard: tests/test_arb_program_guard.c src/arb_program_guard.c src/arb_program_guard.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Isrc tests/test_arb_program_guard.c src/arb_program_guard.c -o $@

test-arb-program-guard: $(BUILD_DIR)/test_arb_program_guard
	./$(BUILD_DIR)/test_arb_program_guard

test-arb-sampler-usage: $(BUILD_DIR)/test_arb_sampler_usage
	./$(BUILD_DIR)/test_arb_sampler_usage

$(BUILD_DIR)/mode_switch_stress: tests/mode_switch_stress.c tests/mode_switch_stress_asm.S | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 $^ -o $@
	codesign --force --sign - $@

# Reproduces Rosetta's wrong-mode landings after far jumps (guarded=0 crashes
# within seconds; guarded=1 must survive and report recoveries).
mode-switch-stress: $(BUILD_DIR)/mode_switch_stress
	arch -x86_64 ./$(BUILD_DIR)/mode_switch_stress guarded=1 threads=12 seconds=30 churn=1

run: $(BUILD_DIR)/mode32_probe
	arch -x86_64 ./$(BUILD_DIR)/mode32_probe

run-game: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ)
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	arch -x86_64 ./$(BUILD_DIR)/game_loader "$(IMAGE_SOURCE)"

# Recover the plain Pirates Mach-O by emulating the packer stub (needs the
# `unicorn` Python package, see the README).
$(BUILD_DIR)/venv/bin/python: | $(BUILD_DIR)
	python3 -m venv $(BUILD_DIR)/venv
	$(BUILD_DIR)/venv/bin/python -m pip install --quiet --disable-pip-version-check unicorn

$(BUILD_DIR)/LEGOPirates.unpacked.macbin: tools/unpack_securom.py $(UNPACK_PYTHON_PREREQ) | $(BUILD_DIR)
	test -f "$(PACKED_IMAGE)" || { echo "missing $(PACKED_IMAGE); pass SOURCE_APP=/path/to/LEGOPirates.app" >&2; exit 1; }
	$(UNPACK_PYTHON) tools/unpack_securom.py "$(PACKED_IMAGE)" "$@"

.PHONY: saga-update
saga-update:
	test -f "$(SAGA_UPDATE_ZIP)" || { echo "missing $(SAGA_UPDATE_ZIP); pass SAGA_UPDATE_ZIP=/path/to/legostarwarssaga_111_update.zip, or SAGA_UPDATE_ZIP= with SOURCE_APP=/path/to/an/updated.app" >&2; exit 1; }
	mkdir -p $(BUILD_DIR)/saga-update
	ditto -x -k "$(SAGA_UPDATE_ZIP)" $(BUILD_DIR)/saga-update

ifeq ($(GAME):$(SAGA_EDITION),saga:steam)
bundle: $(BUILD_DIR)/game_loader $(INFO_PLIST) tools/bundle_saga_steam.py
	python3 tools/bundle_saga_steam.py "$(SOURCE_APP)" "$(SAGA_DATA_SOURCE)" "$(BUILD_DIR)/game_loader" "$(BUNDLE)" "$(INFO_PLIST)"
else
bundle: $(BUILD_DIR)/game_loader $(IMAGE_PREREQ) $(INFO_PLIST)
	if test "$(GAME)" = saga; then python3 tools/stage_saga_data.py "$(SAGA_DATA_SOURCE)" "$(BUILD_DIR)/Data"; fi
	test -d "$(SOURCE_CONTENTS)" || { echo "missing $(SOURCE_APP); pass SOURCE_APP=/path/to/the/game.app" >&2; exit 1; }
	test -f "$(IMAGE_SOURCE)" || { echo "missing $(IMAGE_SOURCE)" >&2; exit 1; }
	mkdir -p $(BUNDLE)/Contents/MacOS $(BUNDLE)/Contents/Resources $(BUNDLE)/Contents/SharedSupport $(BUNDLE)/Contents/Library/Frameworks
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	cp "$(IMAGE_SOURCE)" $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	chmod 0644 $(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)
	for data in $(GAME_DATA); do cp "$(SOURCE_CONTENTS)/MacOS/$$data" $(BUNDLE)/Contents/MacOS/$$data; done
	if test -n "$(CG_SOURCE)"; then ditto --noextattr --noqtn "$(CG_SOURCE)" $(BUNDLE)/Contents/Library/Frameworks/Cg.framework; fi
	cp $(INFO_PLIST) $(BUNDLE)/Contents/Info.plist
	ditto --noextattr --noqtn "$(SOURCE_CONTENTS)/Resources" $(BUNDLE)/Contents/Resources
	cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME)
	if test "$(RESOURCE_FORKS)" = 1; then python3 tools/flatten_resource_forks.py $(BUNDLE); fi
	codesign --force --deep --sign - $(BUNDLE)
endif

# Update only the compatibility runtime when the external source-data volume
# is unavailable. The existing bundle's game data and resources are retained.
promote-loader: $(BUILD_DIR)/game_loader
	test -d $(BUNDLE)/Contents/MacOS
	cp $(BUILD_DIR)/game_loader $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new
	mv $(BUNDLE)/Contents/MacOS/$(LOADER_NAME).new $(BUNDLE)/Contents/MacOS/$(LOADER_NAME)
	if test -f "$(ICON)"; then cp "$(ICON)" $(BUNDLE)/Contents/Resources/$(ICON_NAME); fi
	codesign --force --deep --sign - $(BUNDLE)
	codesign --verify --deep --strict $(BUNDLE)

# Regenerate the Dock icons from the artwork in icons/ (the shipped icons are
# cut-outs, which macOS 26 draws on a white tile).
icons: $(BUILD_DIR)/mkicon
	./$(BUILD_DIR)/mkicon icons/LEGOPirates-art.png icons/LEGOPirates.icns
	./$(BUILD_DIR)/mkicon icons/LEGOCloneWars-art.png icons/LEGOCloneWars.icns 14,16,22

$(BUILD_DIR)/mkicon: icons/mkicon.swift | $(BUILD_DIR)
	swiftc -O $< -o $@

clean:
	rm -f $(BUILD_DIR)/mode32_probe $(BUILD_DIR)/game_loader $(BUILD_DIR)/controller_probe $(BUILD_DIR)/mkicon

.PHONY: test-hitch-recorder
$(BUILD_DIR)/test_hitch_recorder: tests/test_hitch_recorder.c src/hitch_recorder.c src/hitch_recorder.h src/host_diagnostics.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) tests/test_hitch_recorder.c src/hitch_recorder.c -o $@
	codesign --force --sign - $@

test-hitch-recorder: $(BUILD_DIR)/test_hitch_recorder
	arch -x86_64 ./$(BUILD_DIR)/test_hitch_recorder

.PHONY: test-steam-storage-fix
STEAM_STORAGE_LIBRARY ?= $(BUILD_DIR)/LEGOMarvel-Compat.app/Contents/Resources/libsteam_api.dylib
$(BUILD_DIR)/test_steam_storage_fix: tests/test_steam_storage_fix.c src/steam_storage_fix.c src/steam_storage_fix.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) tests/test_steam_storage_fix.c src/steam_storage_fix.c -o $@
	codesign --force --sign - $@

test-steam-storage-fix: $(BUILD_DIR)/test_steam_storage_fix
	python3 tests/test_steam_storage_fix.py $(BUILD_DIR)/test_steam_storage_fix "$(STEAM_STORAGE_LIBRARY)"

.PHONY: test-stl-bridge
$(BUILD_DIR)/test_stl_bridge: tests/test_stl_bridge.c src/stl_bridge.c src/stl_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_stl_bridge.c src/stl_bridge.c -o $@
	codesign --force --sign - $@

test-stl-bridge: $(BUILD_DIR)/test_stl_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_stl_bridge

$(BUILD_DIR)/test_resource_bridge: tests/test_resource_bridge.c src/resource_bridge.c src/resource_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_resource_bridge.c src/resource_bridge.c -o $@
	codesign --force --sign - $@

test-resource-bridge: $(BUILD_DIR)/test_resource_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_resource_bridge

.PHONY: test-rtti-bridge
$(BUILD_DIR)/test_rtti_bridge: tests/test_rtti_bridge.c src/rtti_bridge.c src/rtti_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_rtti_bridge.c -o $@

test-rtti-bridge: $(BUILD_DIR)/test_rtti_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_rtti_bridge

.PHONY: test-carbon-geometry
.PHONY: test-carbon-dispatch
test-carbon-dispatch: $(BUILD_DIR)/game_loader
	env LP32_CARBON_DISPATCH_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-import-return
test-import-return: $(BUILD_DIR)/game_loader
	env LP32_IMPORT_RETURN_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

test-carbon-geometry: $(BUILD_DIR)/game_loader
	env LP32_CARBON_GEOMETRY_SELFTEST=1 LP32_MUTE_AUDIO=1 LP32_BACKGROUND_TEST=1 LP32_NO_DIAGNOSTIC_LOG=1 arch -x86_64 ./$(BUILD_DIR)/game_loader "$(BUNDLE)/Contents/SharedSupport/$(IMAGE_NAME)"

.PHONY: test-cfnetwork-bridge
$(BUILD_DIR)/test_cfnetwork_bridge: tests/test_cfnetwork_bridge.c src/cfnetwork_bridge.c src/cfnetwork_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_cfnetwork_bridge.c src/cfnetwork_bridge.c -framework CFNetwork -framework SystemConfiguration -framework CoreFoundation -o $@
	codesign --force --sign - $@

test-cfnetwork-bridge: $(BUILD_DIR)/test_cfnetwork_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_cfnetwork_bridge

.PHONY: test-libcpp-bridge
$(BUILD_DIR)/test_libcpp_bridge: tests/test_libcpp_bridge.c src/libcpp_bridge.c src/libcpp_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_libcpp_bridge.c src/libcpp_bridge.c -o $@

test-libcpp-bridge: $(BUILD_DIR)/test_libcpp_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_libcpp_bridge

.PHONY: test-blocks-bridge
$(BUILD_DIR)/test_blocks_bridge: tests/test_blocks_bridge.c src/blocks_bridge.c src/blocks_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_blocks_bridge.c src/blocks_bridge.c -o $@

test-blocks-bridge: $(BUILD_DIR)/test_blocks_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_blocks_bridge

.PHONY: test-curl-bridge
$(BUILD_DIR)/test_curl_bridge: tests/test_curl_bridge.c src/curl_bridge.c src/curl_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_curl_bridge.c src/curl_bridge.c -lcurl -o $@

test-curl-bridge: $(BUILD_DIR)/test_curl_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_curl_bridge

.PHONY: test-regex-bridge
$(BUILD_DIR)/test_regex_bridge: tests/test_regex_bridge.c src/regex_bridge.c src/regex_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_regex_bridge.c -o $@

test-regex-bridge: $(BUILD_DIR)/test_regex_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_regex_bridge

.PHONY: test-gl-core-bridge
$(BUILD_DIR)/test_gl_core_bridge: tests/test_gl_core_bridge.c src/gl_core_bridge.c src/gl_core_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_gl_core_bridge.c src/gl_core_bridge.c -framework OpenGL -o $@

test-gl-core-bridge: $(BUILD_DIR)/test_gl_core_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_gl_core_bridge

.PHONY: test-runloop-bridge
$(BUILD_DIR)/test_runloop_bridge: tests/test_runloop_bridge.m src/cfnetwork_bridge.c src/cfnetwork_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_runloop_bridge.m src/cfnetwork_bridge.c -framework Foundation -framework CFNetwork -framework SystemConfiguration -o $@

test-runloop-bridge: $(BUILD_DIR)/test_runloop_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_runloop_bridge

.PHONY: test-steam-bridge
$(BUILD_DIR)/test_steam_bridge: tests/test_steam_bridge.c src/steam_bridge.c src/steam_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_steam_bridge.c -o $@

test-steam-bridge: $(BUILD_DIR)/test_steam_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_steam_bridge

.PHONY: test-dlfcn-bridge
$(BUILD_DIR)/test_dlfcn_bridge: tests/test_dlfcn_bridge.c src/dlfcn_bridge.c src/dlfcn_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_dlfcn_bridge.c src/dlfcn_bridge.c -o $@

test-dlfcn-bridge: $(BUILD_DIR)/test_dlfcn_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_dlfcn_bridge

$(BUILD_DIR)/test_zlib_bridge: tests/test_zlib_bridge.c src/zlib_bridge.c src/zlib_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-no_pie,-pagezero_size,0x1000 -Isrc tests/test_zlib_bridge.c -lz -o $@

test-zlib-bridge: $(BUILD_DIR)/test_zlib_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_zlib_bridge

$(BUILD_DIR)/test_libcpp_stream_bridge: tests/test_libcpp_stream_bridge.cpp src/libcpp_stream_bridge.cpp | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_libcpp_stream_bridge.cpp -lc++ -o $@

test-libcpp-stream-bridge: $(BUILD_DIR)/test_libcpp_stream_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_libcpp_stream_bridge

$(BUILD_DIR)/test_tlv_bridge: tests/test_tlv_bridge.c src/tlv_bridge.c src/tlv_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 tests/test_tlv_bridge.c -o $@

test-tlv-bridge: $(BUILD_DIR)/test_tlv_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_tlv_bridge

.PHONY: test-sound-manager-bridge
$(BUILD_DIR)/test_sound_manager_bridge: tests/test_sound_manager_bridge.c src/sound_manager_bridge.c src/sound_manager_bridge.h | $(BUILD_DIR)
	$(CLANG) $(CFLAGS) -Wl,-pagezero_size,0x1000 -Isrc tests/test_sound_manager_bridge.c src/sound_manager_bridge.c -framework AudioToolbox -o $@

test-sound-manager-bridge: $(BUILD_DIR)/test_sound_manager_bridge
	arch -x86_64 ./$(BUILD_DIR)/test_sound_manager_bridge

.PHONY: test-pointer-proxy
test-pointer-proxy: $(BUILD_DIR)/game_loader
	env LP32_POINTER_SELFTEST=1 LP32_NO_DIAGNOSTIC_LOG=1 LP32_MUTE_AUDIO=1 arch -x86_64 ./$(BUILD_DIR)/game_loader
