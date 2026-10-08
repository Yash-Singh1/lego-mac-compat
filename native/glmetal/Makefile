# GLMetal and its test suite. Everything is built universal (x86_64 for
# Rosetta processes such as the 32-bit loader, arm64 for native games).
#
#   make -C native/glmetal glcompare     build the comparison suite
#   make -C native/glmetal check         run it: Apple's GL vs GLMetal

SDK := $(shell xcrun --sdk macosx --show-sdk-path)
ARCHS ?= -arch x86_64 -arch arm64
CC := xcrun clang
CFLAGS := $(ARCHS) -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -Wall -Wextra \
	-Wno-unused-parameter -DGL_SILENCE_DEPRECATION
OUT := build

GLC := tests/glcompare
GLC_CASES := $(wildcard $(GLC)/cases_*.c)

APPLE_DATA := data
GEN := $(OUT)/gen
SRC_C := $(wildcard src/*.c)
SRC_M := $(wildcard src/*.m)
SRC_CXX := $(wildcard src/*.cpp)
DEPS := third_party/install
DEP_LIBS := $(addprefix $(DEPS)/lib/lib,glslang.a MachineIndependent.a GenericCodeGen.a OSDependent.a SPIRV.a \
	glslang-default-resource-limits.a spirv-cross-msl.a spirv-cross-glsl.a spirv-cross-core.a)
# glslang / SPIRV-Cross stay private: their symbols must not clash with an
# application's own copies.
DEP_LINK := $(foreach lib,$(DEP_LIBS),-Wl,-load_hidden,$(lib))
CXX := xcrun clang++
# glslang's source tree (tools/build_deps.sh) for its AST headers.
GLSLANG_SRC := third_party/glslang
CXXFLAGS := $(ARCHS) -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -g -std=c++17 -fvisibility=hidden \
	-Wall -Wno-unused-parameter -I$(DEPS)/include -I$(GLSLANG_SRC) -Isrc
GENERATED := $(GEN)/glm_core_enums.h $(GEN)/glm_apple_profile.h $(GEN)/glm_apple_gets.h $(GEN)/attrib_setters.c $(GEN)/stubs.c \
	$(GEN)/glm_impl_rename.h $(GEN)/marshal.c
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -Isrc -I$(GEN) -Wno-unused-function -Wno-sign-compare \
	-Wno-missing-field-initializers

.PHONY: all lib inject glcompare check check-reference clean
all: lib inject glcompare
lib: $(OUT)/libGLMetal.dylib $(OUT)/glmetal-compiler
inject: $(OUT)/libGLMetalInject.dylib

# The injector: interposes Apple's OpenGL.framework onto GLMetal in an
# unmodified process (see src/inject/inject.c).
$(GEN)/inject_tables.c $(GEN)/inject_trampolines.S: tools/gen_inject.py $(APPLE_DATA)/apple-gl-exports.txt \
		$(APPLE_DATA)/apple-cgl-exports.txt
	python3 tools/gen_inject.py $(GEN) $(APPLE_DATA)/apple-gl-exports.txt $(APPLE_DATA)/apple-cgl-exports.txt

$(OUT)/libGLMetalInject.dylib: src/inject/inject.c $(GEN)/inject_tables.c $(GEN)/inject_trampolines.S
	$(CC) $(CFLAGS) -dynamiclib $^ -install_name @rpath/libGLMetalInject.dylib -framework OpenGL \
		-framework CoreFoundation -framework AppKit -o $@

$(GEN)/glm_core_enums.h: tools/gen_core_enums.py
	@mkdir -p $(GEN)
	python3 $< $(SDK) $@
$(GEN)/glm_apple_gets.h: tools/gen_get_reference.py $(APPLE_DATA)/apple-get-reference.txt
	@mkdir -p $(GEN)
	python3 $^ $@
$(GEN)/glm_apple_profile.h: tools/gen_profile.py $(APPLE_DATA)/apple-gl-reference.txt
	@mkdir -p $(GEN)
	python3 $^ $@
$(GEN)/attrib_setters.c: tools/gen_attribs.py
	@mkdir -p $(GEN)
	python3 $< $@
# The threaded entry points (tools/gen_marshal.py): marshal.c defines the
# exported gl* functions; the implementation is compiled with
# glm_impl_rename.h so its definitions become glm_impl_*.
$(GEN)/stubs.c $(GEN)/glm_impl_rename.h $(GEN)/marshal.c: tools/gen_marshal.py $(APPLE_DATA)/apple-gl-exports.txt \
		$(APPLE_DATA)/apple-cgl-exports.txt $(APPLE_DATA)/apple-profile-rejections.txt
	@mkdir -p $(GEN)
	python3 tools/gen_marshal.py $(SDK) $(GEN) $(APPLE_DATA)/apple-gl-exports.txt $(APPLE_DATA)/apple-cgl-exports.txt

# glslang and SPIRV-Cross, universal and static: tools/build_deps.sh.
$(DEPS)/lib/libglslang.a:
	tools/build_deps.sh

# The shader compiler as a native helper (src/compile_remote.cpp starts it
# from processes running under Rosetta): arm64 only.
$(OUT)/glmetal-compiler: tools/glmetal_compiler.cpp src/shader_compiler.cpp src/compile_cache.cpp src/compile_remote.cpp \
		src/shader_compiler.h $(GEN)/compiler_stamp.h $(DEPS)/lib/libglslang.a
	xcrun clang++ -arch arm64 -isysroot $(SDK) -mmacosx-version-min=11.0 -O2 -std=c++17 -I$(DEPS)/include -I$(GLSLANG_SRC) -Isrc -I$(GEN) \
		tools/glmetal_compiler.cpp src/shader_compiler.cpp src/compile_cache.cpp src/compile_remote.cpp $(DEP_LIBS) -o $@

# The shader compiler's identity for the disk caches: a hash of its source
# (and glslang/SPIRV-Cross), so rebuilds that do not change it keep caches.
$(GEN)/compiler_stamp.h: src/shader_compiler.cpp src/shader_compiler.h src/shader_texture_bias.h src/shader_fp64.h src/shader_fp64_arithmetic.h src/shader_fp64_matrix.h src/shader_cube_shadow.h src/shader_clip.h src/compile_cache.cpp $(DEPS)/lib/libglslang.a
	@mkdir -p $(GEN)
	printf '#define GLM_COMPILER_STAMP "%s"\n' "$$(cat $^ | shasum -a 256 | cut -c1-32)" > $@

$(OUT)/obj/%.o: src/%.cpp src/shader_compiler.h $(DEPS)/lib/libglslang.a $(GEN)/compiler_stamp.h
	@mkdir -p $(OUT)/obj
	$(CXX) $(CXXFLAGS) -I$(GEN) -c $< -o $@

$(OUT)/obj/shader_depth.o: src/shader_depth.h

# AppKit subclasses that must skip AppKit's -dealloc: manual retain/release.
SRC_MRC := $(wildcard src/mrc/*.m)
$(OUT)/obj/mrc_%.o: src/mrc/%.m $(GEN)/glm_impl_rename.h
	@mkdir -p $(OUT)/obj
	$(CC) $(LIB_CFLAGS) -include $(GEN)/glm_impl_rename.h -fno-objc-arc -c $< -o $@

LIB_OBJS := $(SRC_CXX:src/%.cpp=$(OUT)/obj/%.o) $(SRC_MRC:src/mrc/%.m=$(OUT)/obj/mrc_%.o)

# Compiled without the rename: the exported entry points.
$(OUT)/obj/marshal.o: $(GEN)/marshal.c src/marshal_support.h src/thread.h
	@mkdir -p $(OUT)/obj
	$(CC) $(LIB_CFLAGS) -Wno-unused-parameter -c $< -o $@
$(OUT)/obj/marshal_custom.o: src/marshal/marshal_custom.c src/marshal_support.h src/thread.h $(wildcard src/*.h) $(GENERATED)
	@mkdir -p $(OUT)/obj
	$(CC) $(LIB_CFLAGS) -c $< -o $@

LIB_OBJS += $(OUT)/obj/marshal.o $(OUT)/obj/marshal_custom.o

$(OUT)/libGLMetal.dylib: $(SRC_C) $(SRC_M) $(wildcard src/*.h) $(GENERATED) $(LIB_OBJS)
	$(CC) $(LIB_CFLAGS) -include $(GEN)/glm_impl_rename.h -dynamiclib $(SRC_C) $(GEN)/attrib_setters.c $(GEN)/stubs.c \
		$(SRC_M) -fobjc-arc $(LIB_OBJS) $(DEP_LINK) -lc++ \
		-install_name @rpath/libGLMetal.dylib -framework Metal -framework Foundation -framework QuartzCore \
		-framework CoreGraphics -framework AppKit -o $@

glcompare: $(OUT)/glcompare $(OUT)/libglcases.dylib

.PHONY: compile-budget-probe
compile-budget-probe: $(OUT)/glm_compile_budget

$(OUT)/glm_compile_budget: tests/window/glm_compile_budget.m
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Wno-deprecated-declarations $< -framework Cocoa -framework OpenGL -o $@

$(OUT)/glcompare: $(GLC)/runner.c $(GLC)/glcompare.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $< -framework ApplicationServices -framework ImageIO \
		-framework CoreFoundation -o $@

$(OUT)/gen/glc_gl_legacy.h: $(GLC)/gen_gl_header.py
	python3 $< $(SDK) $(OUT)/gen

# GL calls go through glc_resolve to the implementation the runner loaded;
# only the runner's own symbols (glc_fail, glc_lookup...) bind dynamically.
$(OUT)/libglcases.dylib: $(GLC_CASES) $(GLC)/glc_util.c $(GLC)/glcompare.h $(OUT)/gen/glc_gl_legacy.h tests/probes/border_reconstruction.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(GLC) -I$(OUT)/gen -dynamiclib $(GLC_CASES) $(GLC)/glc_util.c \
		-Wl,-undefined,dynamic_lookup -o $@

check: glcompare check-reference
	python3 $(GLC)/compare.py

# GLMetal against Apple's recorded API behaviour (data/apple-get-reference.txt:
# getters, capabilities, renderbuffer and texture formats), direct and on the
# command stream. No windows: surfaceless contexts only.
$(OUT)/apple_get_sweep: tools/apple_get_sweep.c
	@mkdir -p $(OUT)
	$(CC) -arch arm64 -w $< -o $@
check-reference: lib $(OUT)/apple_get_sweep
	@grep -v '^#' $(APPLE_DATA)/apple-get-reference.txt > $(OUT)/apple-get-reference.expected
	@$(OUT)/apple_get_sweep $(OUT)/libGLMetal.dylib 2>/dev/null | grep -v '^#' | \
		diff $(OUT)/apple-get-reference.expected - > $(OUT)/reference-direct.diff && echo "reference: direct matches Apple" || \
		{ echo "reference: direct differs from Apple ($(OUT)/reference-direct.diff)"; exit 1; }
	@GLMETAL_THREAD_ALL=1 $(OUT)/apple_get_sweep $(OUT)/libGLMetal.dylib 2>/dev/null | grep -v '^#' | \
		diff $(OUT)/apple-get-reference.expected - > $(OUT)/reference-threaded.diff && echo "reference: threaded matches Apple" || \
		{ echo "reference: threaded differs from Apple ($(OUT)/reference-threaded.diff)"; exit 1; }

clean:
	rm -rf $(OUT)
