CURDIR   := $(shell pwd)
BASEDIR	 := $(abspath $(dir $(CURDIR)))
CXX      ?= g++
PRIV_DIR ?= $(if $(REBAR_BARE_COMPILER_OUTPUT_DIR),$(REBAR_BARE_COMPILER_OUTPUT_DIR),$(BASEDIR))/priv
OBJ_DIR  ?= $(BASEDIR)/obj
DEBUG    ?= 0
REBAR    := rebar3

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

HAS_PGO := $(if $(wildcard $(TARGET)),$(if $(shell strings $(TARGET) | grep -E "GLAZER_PGO_OPTIMIZED"),yes))

# Detect compiler family for LTO flags and PGO format.
# AppleClang uses ld64 which handles thin-LTO natively — no -fuse-ld=lld.
# Upstream Clang on Linux emits LLVM bitcode that requires lld.
# GCC uses -flto=auto with GNU ld/gold.
CXX_ID := $(shell $(CXX) --version 2>&1 | head -1)
IS_CLANG := $(findstring clang,$(CXX_ID))
IS_APPLE := $(findstring Apple,$(CXX_ID))
LLVM_VERSION := $(shell $(CXX) --version 2>&1 | grep -oE "[0-9]+\.[0-9]+\.[0-9]+" | head -1 | cut -d. -f1)
# For clang, determine llvm-profdata tool name
ifdef IS_CLANG
  ifdef IS_APPLE
    LLVM_PROFDATA := xcrun llvm-profdata
  else
    # On Linux, try versioned name first (e.g., llvm-profdata-19), then unversioned
    LLVM_PROFDATA := $(shell \
      if [ -n "$(LLVM_VERSION)" ] && command -v llvm-profdata-$(LLVM_VERSION) >/dev/null 2>&1; then \
        echo "llvm-profdata-$(LLVM_VERSION)"; \
      elif command -v llvm-profdata >/dev/null 2>&1; then \
        echo "llvm-profdata"; \
      else \
        echo "llvm-profdata"; \
      fi \
    )
  endif
endif

ifneq ($(IS_APPLE),)
  LTO_FLAGS  := -flto=thin
  LINK_FLAGS :=
else ifneq ($(IS_CLANG),)
  LTO_FLAGS  := -flto=thin
  LINK_FLAGS := -fuse-ld=lld
else
  LTO_FLAGS  := -flto=auto -fno-fat-lto-objects
  LINK_FLAGS :=
endif

# Optional jq support (glazer:json_query/2) — detected via a compile+link
# probe since most distros don't ship a jq.pc pkg-config file. Falls back
# to pkg-config first in case a future libjq release adds one.
# Note: the hash must be kept in a variable so Make doesn't treat it as a
# comment character when it expands the $(shell ...) line.
HASH     := \#
HAVE_JQ := $(shell pkg-config --exists jq 2>/dev/null && echo 1 || \
  (printf '$(HASH)include <jq.h>\nint main(){jq_init();return 0;}\n' | \
   $(CXX) -xc++ - -ljq -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0))
ifeq ($(HAVE_JQ),1)
  CXXFLAGS += -DGLAZER_HAVE_JQ
  LDFLAGS  += -ljq
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
    CXXFLAGS += -fprofile-use=$(PGO_DIR) -DGLAZER_PGO=1
    LDFLAGS  += -fprofile-use=$(PGO_DIR)
    # GCC supports these flags; clang does not
    ifeq ($(IS_CLANG),)
      CXXFLAGS += -fprofile-correction -Wno-missing-profile
    endif
  endif
endif

LDFLAGS += -shared -L$(ERL_EI_LIB) -lei
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -undefined dynamic_lookup
endif

.PHONY: all clean force yes

all: compile

ifeq ($(OPTIMIZE),1)
compile: optimize
else
compile: $(TARGET)
endif

optimize: DEBUG=$(DEBUG) ASAN=$(ASAN)
optimize: $(TARGET) $(HAS_PGO)
ifeq ($(HAS_PGO),yes)
	@echo "===> $(TARGET) is already PGO-optimized. Skipping PGO rebuild."
else
	@echo "===> Building PGO optimized NIF library"
	@echo "===> PGO step 1/3: build instrumented binary"
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) OPTIMIZE=0 PGO=generate clean all
	@cd $(BASEDIR) && env -u OPTIMIZE $(REBAR) compile
	@echo "===> PGO step 2/3: collect profile data"
	@cd $(BASEDIR) && ./bin/pgo-profile.es
ifdef IS_CLANG
	@echo "===> PGO step 2b/3: merge clang profile data ($(LLVM_PROFDATA))"
	@if [ -f "$(PGO_DIR)/default.profdata" ]; then \
	  echo "Profile already merged"; \
	elif ls $(PGO_DIR)/*.profraw >/dev/null 2>&1; then \
	  TOOL="$(LLVM_PROFDATA)"; \
	  if ! command -v $$TOOL >/dev/null 2>&1; then \
	    echo "Error: $$TOOL not found in PATH."; \
	    echo "Available llvm tools:"; \
	    ls -la /usr/bin/llvm-* 2>/dev/null || echo "  (none found)"; \
	    exit 1; \
	  fi; \
	  $$TOOL merge -o $(PGO_DIR)/default.profdata $(PGO_DIR)/*.profraw && \
	  echo "Merged profile data successfully" || \
	  (echo "Error: Failed to merge profile data"; exit 1); \
	else \
	  echo "No .profraw files found in $(PGO_DIR)"; \
	fi
endif
	@echo "===> PGO step 3/3: rebuild with profile data"
	@rm -f $(OBJ_DIR)/glazer_nif.o $(PRIV_DIR)/glazer.so
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) OPTIMIZE=0 PGO=use all
	@cd $(BASEDIR) && env -u OPTIMIZE $(REBAR) compile
	@echo "===> PGO build complete"
endif

# Locate the ASan runtime for LD_PRELOAD on Linux.
# macOS memcheck is not supported (DYLD_INSERT_LIBRARIES is dropped by the
# rebar3 shell wrapper before reaching erl), so this block is Linux-only.
# Try Clang's resource-dir path first; fall back to GCC's -print-file-name.
_CXX_RESOURCE_DIR := $(shell $(CXX) -print-resource-dir 2>/dev/null)
_HOST_ARCH        := $(shell uname -m)
_ASAN_CLANG       := $(_CXX_RESOURCE_DIR)/lib/linux/libclang_rt.asan-$(_HOST_ARCH).so
_ASAN_GCC         := $(shell $(CXX) -print-file-name=libasan.so 2>/dev/null)
ASAN_RT           := $(strip $(if $(wildcard $(_ASAN_CLANG)),$(_ASAN_CLANG),\
                       $(if $(filter-out $(notdir $(_ASAN_GCC)),$(_ASAN_GCC)),$(_ASAN_GCC))))
ASAN_PRELOAD       = LD_PRELOAD="$(ASAN_RT)"
LSAN_SUPPRESSIONS := $(abspath lsan_suppressions.txt)

leak ?= 0
ifeq ($(leak),0)
  DETECT_LEAKS := 0
else
  DETECT_LEAKS := 1
endif

memcheck:
	@echo "===> Building NIF with AddressSanitizer"
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) ASAN=1 \
	  $(if $(VERBOSE),VERBOSE=1) clean compile
	@echo "===> Running eunit under ASan$(if $(filter 1,$(DETECT_LEAKS)), + LeakSanitizer,) ($(ASAN_PRELOAD))"
	@cd .. && ERL_FLAGS="+A 1" ASAN_OPTIONS="detect_leaks=$(DETECT_LEAKS)" \
	  LSAN_OPTIONS="suppressions=$(LSAN_SUPPRESSIONS)" \
	  $(ASAN_PRELOAD) \
	  $(REBAR) eunit 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -v "^===> \(Verifying\|Analyzing\|Compile (apps)\|Not adding provider\|Evaluating config\)"
	@echo "===> Rebuilding normal NIF (removing ASan instrumentation)"
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) $(if $(VERBOSE),VERBOSE=1) clean all
	@cd .. && $(REBAR) compile

info:
	@echo -ne "TARGET: "
	@if [ -f $(TARGET) ]; then \
    echo "$(TARGET) exists"; \
    strings $(TARGET) | grep -E "GLAZER_" | sed 's/=/: /'; \
  else \
    echo "not found"; \
  fi

$(TARGET): $(OBJ) | $(PRIV_DIR)
	@[ -n $(HAVE_JQ) ] && echo "===> Building glazer with jq support" || true
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
