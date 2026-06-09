ifndef VERBOSE
MAKEFLAGS += --no-print-directory
endif

PRIV_DIR := $(abspath priv)
OBJ_DIR  := $(abspath obj)
DEBUG    ?= 0
REBAR    ?= rebar3
APP      := $(shell sed -n '/application,/{s/^.*, //; s/,.*$$//; p; q}' src/*.app.src)

all: compile

help:
	@echo "Usage: make [target] [VAR=value ...]"
	@echo ""
	@echo "Build targets:"
	@echo "  all          Build NIF and compile Erlang (default)"
	@echo "  nif          Build only the C NIF shared library"
	@echo "  compile      Build NIF + run rebar3 compile"
	@echo "  optimize     PGO build: instrument → benchmark → rebuild (fastest binary)"
	@echo "  clean        Remove build artifacts"
	@echo "  distclean    Remove all generated files including deps"
	@echo ""
	@echo "Development:"
	@echo "  test         Run eunit test suite"
	@echo "  memcheck     Build with ASan (-fsanitize=address) and run eunit"
	@echo "  benchmark    Run benchmarks via mix bench"
	@echo "  deps         Fetch mix dependencies"
	@echo "  doc          Generate documentation"
	@echo ""
	@echo "Publishing:"
	@echo "  publish      Publish to hex.pm  (pass replace=1 to replace existing)"
	@echo "  deprecate    Deprecate a hex.pm release  (pass vsn=X.Y.Z)"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1      Build NIF without optimisations (-O0 -g)"
	@echo "  ASAN=1       Build with AddressSanitizer (implied by memcheck)"
	@echo "  VERBOSE=1    Show full compiler command lines"

nif: $(PRIV_DIR)/glazer.so

$(PRIV_DIR)/glazer.so: $(wildcard c_src/*.cpp c_src/*.hpp)
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) DEBUG=$(DEBUG) \
	  $(if $(VERBOSE),VERBOSE=1,) --no-print-directory

compile: $(PRIV_DIR)/glazer.so
	$(REBAR) compile

clean:
	$(REBAR) clean
	@$(MAKE) --no-print-directory -C c_src PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) DEBUG=$(DEBUG) clean 2>/dev/null || true

distclean: clean
	@rm -rf obj priv/glazer.so _build

test:
	$(REBAR) eunit

ASAN_RT := $(shell $(CXX) -print-file-name=libasan.so 2>/dev/null)

memcheck:
	@echo "==> Building NIF with AddressSanitizer"
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) ASAN=1 \
	  $(if $(VERBOSE),VERBOSE=1,) clean all
	@$(REBAR) compile
	@echo "==> Running eunit under ASan (LD_PRELOAD=$(ASAN_RT))"
	ERL_FLAGS="+A 1" ASAN_OPTIONS="detect_leaks=0" \
	  LD_PRELOAD="$(ASAN_RT)" \
	  $(REBAR) eunit
	@echo "==> memcheck done — rebuild for normal use: make"

doc docs:
	$(REBAR) ex_doc

benchmark bench: deps
	@echo "Running benchmarks..."
	@mix bench

# Profile-guided optimisation: instrument → run tests as workload → rebuild.
# Usage: make optimize
optimize:
	@echo "==> PGO step 1/3: build instrumented binary"
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) PGO=generate clean all
	@$(REBAR) compile
	@echo "==> PGO step 2/3: collect profile data via test suite"
	mix bench 1>/dev/null
	@echo "==> PGO step 3/3: rebuild with profile data"
	@rm -f $(OBJ_DIR)/glazer_nif.o $(PRIV_DIR)/glazer.so
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) OBJ_DIR=$(OBJ_DIR) PGO=use all
	@$(REBAR) compile
	@echo "==> PGO build complete"

deps:
	@mix deps.get

publish: docs
	$(REBAR) hex publish$(if $(replace), --replace)

deprecate:
	@if [ -z $(vsn) ]; then \
	  echo "Usage: $(MAKE) $@ vsn=X.Y.Z      - Deprecate version X.Y.Z"; \
	  exit 1; \
	fi
	$(REBAR) hex retire $(APP) $(vsn) deprecated --message Deprecated

.PHONY: all help doc compile clean distclean test memcheck nif optimize benchmark bench deps publish deprecate
