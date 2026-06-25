CURDIR   := $(shell pwd)
BASEDIR	 := $(abspath $(dir $(CURDIR)))
CXX      ?= g++
PRIV_DIR ?= $(if $(REBAR_BARE_COMPILER_OUTPUT_DIR),$(REBAR_BARE_COMPILER_OUTPUT_DIR),$(BASEDIR))/priv
OBJ_DIR  ?= $(BASEDIR)/obj
DEBUG    ?= 0

SRC      := glazer_nif.cpp
OBJ      := $(OBJ_DIR)/glazer_nif.o

# erlang:load_nif/2 appends the platform-specific extension itself
# (.dll on Windows, .so elsewhere), so the build output must match.
ifneq (,$(findstring MINGW,$(shell uname -s))$(findstring MSYS,$(shell uname -s)))
  EXT := .dll
else
  EXT := .so
endif

TARGET           := $(PRIV_DIR)/glazer$(EXT)
ERL_ERTS_INCLUDE := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts/erts-~ts/include\",[code:root_dir(),erlang:system_info(version)]),halt(0).")
ERL_EI_INCLUDE   := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts\",[code:lib_dir(erl_interface,include)]),halt(0).")
ERL_EI_LIB       := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts\",[code:lib_dir(erl_interface,lib)]),halt(0).")

CXXFLAGS := -std=c++2b -fPIC -Wall -finline-functions -fvisibility=hidden \
            -I$(ERL_ERTS_INCLUDE) -I$(ERL_EI_INCLUDE) -I.

# Detect compiler family for LTO flags.
# AppleClang uses ld64 which handles thin-LTO natively — no -fuse-ld=lld.
# Upstream Clang on Linux emits LLVM bitcode that requires lld.
# GCC uses -flto=auto with GNU ld/gold.
CXX_ID := $(shell $(CXX) --version 2>&1 | head -1)
ifneq ($(findstring Apple,$(CXX_ID)),)
  LTO_FLAGS  := -flto=thin
  LINK_FLAGS :=
else ifneq ($(findstring clang,$(CXX_ID)),)
  LTO_FLAGS  := -flto=thin
  LINK_FLAGS := -fuse-ld=lld
else
  LTO_FLAGS  := -flto=auto -fno-fat-lto-objects
  LINK_FLAGS :=
endif

# Optional jq support (glazer:json_query/2) — detected via a compile+link
# probe since most distros don't ship a jq.pc pkg-config file. Falls back
# to pkg-config first in case a future libjq release adds one.
HAVE_JQ := $(shell pkg-config --exists jq 2>/dev/null && echo 1 || \
  (printf '\#include <jq.h>\nint main(){jq_init();return 0;}\n' | \
   $(CXX) -xc++ - -ljq -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0))
ifeq ($(HAVE_JQ),1)
  CXXFLAGS += -DGLAZER_HAVE_JQ
  LDFLAGS  += -ljq
  $(info "==> Building glazer with jq support")
endif

# Profile-Guided Optimisation (PGO) — lets the compiler use real branch
# frequencies and call counts to lay out hot paths, inline more aggressively,
# and reorder basic blocks for better instruction-cache usage.  Typical gain
# on a JSON-heavy workload is 5–15 % over plain -O3.
#
# Three-step workflow (or just run `make pgo` from the project root):
#
#   1. make PGO=generate   — build an instrumented .so that writes profile
#                            data (*.gcda) into obj/pgo/ at runtime
#   2. run a representative workload (e.g. `rebar3 eunit`, a benchmark)
#   3. make PGO=use        — rebuild optimised using the collected profile
#
# Profile data lives in $(OBJ_DIR)/pgo/ and is removed by `make clean`.
# -fprofile-correction tolerates minor counter mismatches (e.g. if the
# workload was interrupted) instead of aborting the build.
PGO      ?= 0
PGO_DIR  := $(OBJ_DIR)/pgo
ASAN     ?= 0

# Build metadata exposed at runtime via glazer:info/0 — see nif_info() in
# glazer_nif.cpp. GLAZER_VERSION is the git describe of the checkout used to
# build this .so; GLAZER_APP_VERSION is the `vsn` from glazer.app.src;
# GLAZER_OPT_LEVEL/GLAZER_PGO reflect the flags chosen below.
GLAZER_VERSION     := $(shell git -C $(BASEDIR) describe --tags --dirty --always --abbrev=6 2>/dev/null | sed 's/g//; s/-dirty/*/')
GLAZER_APP_VERSION := $(shell sed -nE 's/.*\{vsn, *"([^"]+)".*/\1/p' $(BASEDIR)/src/*.app.src | head -n1)
CXXFLAGS += -DGLAZER_VERSION='"$(GLAZER_VERSION)"' -DGLAZER_APP_VERSION='"$(GLAZER_APP_VERSION)"'

ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g -DGLAZER_OPT_LEVEL='"none"'
else ifeq ($(ASAN),1)
  # Compile with ASan instrumentation but do NOT link -fsanitize=address.
  # Linking it would embed or reference the runtime inside the NIF .so; when
  # the Erlang VM dlopen's that .so the runtime loads too late and ASan aborts.
  # Instead all __asan_* symbols stay undefined in the .so and are resolved
  # from the runtime preloaded via LD_PRELOAD (Linux only — see Makefile).
  CXXFLAGS += -O1 -g -fsanitize=address -fno-omit-frame-pointer -DGLAZER_OPT_LEVEL='"O1"'
else
  CXXFLAGS += -O3 -DNDEBUG -march=native -mtune=native $(LTO_FLAGS) -DGLAZER_OPT_LEVEL='"O3"'
  LDFLAGS  += $(LINK_FLAGS)
  ifeq ($(PGO),generate)
    CXXFLAGS += -fprofile-generate=$(PGO_DIR)
    LDFLAGS  += -fprofile-generate=$(PGO_DIR)
  else ifeq ($(PGO),use)
    CXXFLAGS += -fprofile-use=$(PGO_DIR) -fprofile-correction -Wno-missing-profile -DGLAZER_PGO=1
    LDFLAGS  += -fprofile-use=$(PGO_DIR)
  endif
endif

LDFLAGS += -shared -L$(ERL_EI_LIB) -lei
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -undefined dynamic_lookup
endif

.PHONY: all clean force

all: $(TARGET)

$(TARGET): $(OBJ) | $(PRIV_DIR)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@

# When PGO=use the .o is stale by timestamp (same source, same flags string)
# but must be recompiled against the newly collected profile data — force it.
# Note: GCC writes .gcda files under a path derived from the source file's
# absolute path, so the layout inside $(PGO_DIR) is nested.  This is harmless:
# GCC searches $(PGO_DIR) recursively on -fprofile-use, and clean wipes the tree.
$(OBJ): $(SRC) $(wildcard *.hpp) $(if $(filter use,$(PGO)),force) \
        | $(OBJ_DIR) $(if $(filter generate use,$(PGO)),$(PGO_DIR))
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PRIV_DIR) $(OBJ_DIR) $(PGO_DIR):
	@mkdir -p $@

clean:
	rm -f $(OBJ) $(TARGET)
	rm -rf $(PGO_DIR)
