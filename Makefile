# Ocean Drive, C23 + SDL3 port.
#   make            optimized build  -> build/release/oceandrive
#   make DEBUG=1    -O0, ASan + UBSan (clang) -> build/debug/oceandrive
#   make test       build and run the headless tests
#   make shaders    regenerate gen/shaders.{h,c} (needs glslangValidator)
#
# gen/ is committed, so a plain build only needs a C23 compiler and SDL3. The shader step
# runs only when a shader source is newer than the generated files.

CC      ?= cc
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS   := $(shell pkg-config --libs sdl3)

WARN   := -Wall -Wextra -Werror -Wshadow -Wvla -Wno-unused-parameter
CFLAGS_BASE := -std=c23 $(WARN) -Isrc -Igen $(SDL_CFLAGS) -MMD -MP
ifeq ($(DEBUG),1)
  # clang ships its own sanitizer runtimes (GCC's libasan/libubsan are separate packages)
  ifeq ($(origin CC),default)
    CC := clang
  endif
  OUT    := build/debug
  CFLAGS := $(CFLAGS_BASE) -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
  LDFLAGS := -fsanitize=address,undefined
else
  OUT    := build/release
  CFLAGS := $(CFLAGS_BASE) -O2 -g -ffp-contract=off
  LDFLAGS :=
endif
LDLIBS := $(SDL_LIBS) -lm

SRC      := $(shell find src -name '*.c' | sort)
GEN_SRC  := gen/shaders.c gen/programs.c
OBJ      := $(patsubst %.c,$(OUT)/%.o,$(SRC) $(GEN_SRC))
APP_OBJ  := $(filter-out $(OUT)/src/main.o,$(OBJ))

TEST_SRC := $(sort $(wildcard tests/*.c))
TEST_BIN := $(patsubst tests/%.c,$(OUT)/tests/%,$(TEST_SRC))

.PHONY: all test shaders clean
all: $(OUT)/oceandrive

$(OUT)/oceandrive: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# generated sources must exist before anything that includes them is compiled
$(OBJ): | gen/shaders.h

# --- shaders -----------------------------------------------------------------------------
SHADER_INPUTS := $(shell find shaders -type f | sort) tools/shadergen.c tools/shaderpack.c \
                 tools/build-shaders.sh tools/check-layout.sh

gen/shaders.h gen/shaders.c gen/programs.h gen/programs.c &: $(SHADER_INPUTS)
	tools/build-shaders.sh $(CC)

shaders: gen/shaders.h

# --- tests -------------------------------------------------------------------------------
$(OUT)/tests/%: tests/%.c $(APP_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(APP_OBJ) $(LDLIBS)

test: $(TEST_BIN)
	@set -e; for t in $(TEST_BIN); do echo "== $$t"; $$t; done

clean:
	rm -rf build

-include $(shell find build -name '*.d' 2>/dev/null)
