# LP32GL: OpenGL on Mesa (Zink + KosmicKrisp), built for the loader's x86_64
# process.  Included from native/Makefile.
#
#   make lp32gl-mesa   fetch, patch and build Mesa (slow; once per revision)
#   make lp32gl        build LP32GL into $(BUILD_DIR)/lp32gl
#
# promote-loader copies $(BUILD_DIR)/lp32gl into the bundle when it exists.
# The loader only uses it when asked (LP32_GL_BACKEND=mesa, or a title that
# opts in); Apple's OpenGL stays the default.

LP32GL_ROOT := lp32gl
LP32GL_ARCH ?= x86_64
LP32GL_MESA_WORK ?= third_party
LP32GL_MESA_SOURCE := $(LP32GL_MESA_WORK)/mesa
LP32GL_MESA_BUILD := $(LP32GL_MESA_WORK)/build-$(LP32GL_ARCH)
LP32GL_MESA_VERSION := $(shell cat $(LP32GL_MESA_SOURCE)/VERSION 2>/dev/null)
LP32GL_GALLIUM := $(LP32GL_MESA_BUILD)/src/gallium/targets/dri/libgallium-$(LP32GL_MESA_VERSION).dylib
LP32GL_KOSMICKRISP := $(LP32GL_MESA_BUILD)/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib
LP32GL_OUT := $(BUILD_DIR)/lp32gl
LP32GL_GEN := $(BUILD_DIR)/lp32gl-gen
LP32GL_CFLAGS := -arch $(LP32GL_ARCH) -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g \
	-Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fno-objc-arc -DGL_SILENCE_DEPRECATION=1 \
	-I$(LP32GL_ROOT)/include -I$(LP32GL_ROOT)/src -I$(LP32GL_GEN) \
	-I$(LP32GL_MESA_SOURCE)/include -I$(LP32GL_MESA_SOURCE)/src/gallium/include \
	-DMESA_VERSION_STRING='"$(LP32GL_MESA_VERSION)"'
LP32GL_SOURCES := $(addprefix $(LP32GL_ROOT)/src/,lp32gl_dri.c lp32gl_cgl.m lp32gl_surface.m \
	lp32gl_gl.c lp32gl_agl.c lp32gl_nsopengl.m lp32gl_exports.c)

.PHONY: lp32gl lp32gl-mesa
lp32gl-mesa:
	$(LP32GL_ROOT)/tools/build_mesa.sh $(abspath $(LP32GL_MESA_WORK)) $(LP32GL_ARCH)

$(LP32GL_GEN)/lp32gl_apple_profile.h: $(LP32GL_ROOT)/tools/gen_apple_profile.py $(LP32GL_ROOT)/data/apple-gl-reference.txt
	mkdir -p $(LP32GL_GEN)
	python3 $^ $@

$(LP32GL_GEN)/lp32gl_exports.S: $(LP32GL_ROOT)/tools/gen_exports.py $(LP32GL_ROOT)/data/apple-gl-exports.txt
	mkdir -p $(LP32GL_GEN)
	python3 $^ $@ $(LP32GL_GEN)/lp32gl_export_names.h
$(LP32GL_GEN)/lp32gl_export_names.h: $(LP32GL_GEN)/lp32gl_exports.S

$(LP32GL_OUT)/libLP32GL.dylib: $(LP32GL_SOURCES) $(wildcard $(LP32GL_ROOT)/src/*.h) $(LP32GL_ROOT)/include/lp32gl.h \
		$(LP32GL_GEN)/lp32gl_apple_profile.h $(LP32GL_GEN)/lp32gl_exports.S $(LP32GL_GEN)/lp32gl_export_names.h \
		$(LP32GL_GALLIUM) $(LP32GL_KOSMICKRISP)
	test -f "$(LP32GL_GALLIUM)" || { echo "missing Mesa build; run make lp32gl-mesa" >&2; exit 1; }
	mkdir -p $(LP32GL_OUT)
	cp "$(LP32GL_GALLIUM)" "$(LP32GL_KOSMICKRISP)" $(LP32GL_OUT)/
	$(CLANG) $(LP32GL_CFLAGS) -dynamiclib $(LP32GL_SOURCES) $(LP32GL_GEN)/lp32gl_exports.S \
		$(LP32GL_OUT)/$(notdir $(LP32GL_GALLIUM)) \
		-install_name @rpath/libLP32GL.dylib -Wl,-rpath,@loader_path \
		-framework Cocoa -framework QuartzCore -framework Metal -o $@

$(LP32GL_OUT)/libvulkan.1.dylib: $(LP32GL_ROOT)/src/vulkan_icd_shim.c
	mkdir -p $(LP32GL_OUT)
	$(CLANG) $(LP32GL_CFLAGS) -dynamiclib $< -install_name @rpath/libvulkan.1.dylib -o $@

lp32gl: $(LP32GL_OUT)/libLP32GL.dylib $(LP32GL_OUT)/libvulkan.1.dylib
	codesign --force --sign - $(LP32GL_OUT)/*.dylib
