ifndef VERBOSE
MAKEFLAGS += --no-print-directory
endif

PRIV_DIR ?= $(if $(REBAR_BARE_COMPILER_OUTPUT_DIR),$(REBAR_BARE_COMPILER_OUTPUT_DIR)/priv,$(abspath priv))
DEBUG    ?= 0
REBAR    ?= rebar3
APP      := $(shell sed -nE 's/^\{application, ([a-zA-Z0-9_]+),.*/\1/p' src/*.app.src | head -n1)
OPTIMIZE ?= 0
ASAN     ?= 0

ifneq ($(filter $(OPTIMIZE),1 true),)
OPTIMIZE := 1
else
OPTIMIZE := 0
endif

export ASAN

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
	@echo "  cover        Run eunit with coverage analysis and print a summary"
	@echo "  check        Run xref and dialyzer"
	@echo "  dialyzer     Run dialyzer"
	@echo "  memcheck     Build with ASan (-fsanitize=address) and run eunit (leak=1 adds LSan)"
	@echo "  benchmark    Run benchmarks via mix bench"
	@echo "  deps         Fetch mix dependencies"
	@echo "  doc          Generate documentation"
	@echo ""
	@echo "Publishing:"
	@echo "  bump-version Increment patch version and commit"
	@echo "  publish      Publish to hex.pm  (pass replace=1 to replace existing)"
	@echo "  deprecate    Deprecate a hex.pm release  (pass vsn=X.Y.Z)"
	@echo ""
	@echo "Variables:"
	@echo "  DEBUG=1      Build NIF without optimisations (-O0 -g)"
	@echo "  ASAN=1       Build with AddressSanitizer (implied by memcheck)"
	@echo "  leak=1       Enable LeakSanitizer during memcheck (off by default)"
	@echo "  VERBOSE=1    Show full compiler command lines"
	@echo "  OPTIMIZE=1   Make all/compile run the PGO 'optimize' build (same as 'make optimize')"

compile: deps
	@$(REBAR) $@

info:
	@$(MAKE) -C c_src $@

nif:
	@$(MAKE) -C c_src DEBUG=$(DEBUG) OPTIMIZE=$(OPTIMIZE) PRIV_DIR=$(PRIV_DIR) $(if $(VERBOSE),VERBOSE=1) compile

clean:
	@$(REBAR) clean
	@$(MAKE) -C c_src clean 2>/dev/null || true

distclean: clean
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) clean 2>/dev/null || true
	@rm -rf obj _build .perf.txt

test:
	@rm -rf _build/test obj priv/glazer.so
	@$(REBAR) eunit
	@mix test

check:
	@$(REBAR) xref
	@$(REBAR) dialyzer

memcheck:
	@$(MAKE) -C c_src ASAN=1 $(if $(VERBOSE),VERBOSE=1 )$@

doc docs:
	@mix docs 2>&1 | grep -v "using single-quoted strings" \
	              #| grep -v "indeed want a charlist" \
	              #| grep -v "change all single-quoted" \

benchmark bench: do-bench

do-bench: deps
	@rm -f .perf.txt
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) optimize
	PARALLEL=$(if $(PARALLEL),$(PARALLEL),1) MIX_ENV=bench mix bench | tee .perf.txt;

# ELIXIR_ERL_OPTIONS quiets rustler_precompiled's [debug] "Copying NIF from
# cache and extracting..." noise (torque/yaml_rustler/rusty_csv all use it)
# without touching the project-wide Logger level — scoped to bench targets
# only via this env var, not a global :logger config in mix.exs.

bench-json bench-yaml bench-csv: export ELIXIR_ERL_OPTIONS = -logger level warning
bench-json bench-yaml bench-csv: deps
	@PARALLEL=$(if $(PARALLEL),$(PARALLEL),1) MIX_ENV=bench mix $@

# Profile-guided optimisation: instrument → run tests as workload → rebuild.
# Usage: make optimize
optimize:
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) $@

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

cover:
	mix test --cover
	$(REBAR) cover --verbose

bump-version:
	@FILE=$$(ls -1 src/*.app.src | head -n1); \
	CURRENT=$$(grep -m1 '{vsn,' $$FILE | sed -E 's/.*"([0-9]+\.[0-9]+\.[0-9]+)".*/\1/'); \
	MAJOR=$$(echo $$CURRENT | cut -d. -f1); \
	MINOR=$$(echo $$CURRENT | cut -d. -f2); \
	PATCH=$$(echo $$CURRENT | cut -d. -f3); \
	NEW=$$(echo "$${MAJOR}.$${MINOR}.$$((PATCH + 1))" | tr -d '\n'); \
	echo "Bumping version from $${CURRENT} to $${NEW}"; \
	sed -i "s/{vsn, \"$${CURRENT}\"}/{vsn, \"$${NEW}\"}/" $$FILE; \
	echo "Changed: {vsn, \"$${CURRENT}\"} -> {vsn, \"$${NEW}\"}"; \
	sed -i 's/\({:\?glazer,[[:space:]]*"~>\)[^"]*/\1 '"$${MAJOR}.$${MINOR}"'/' README.md; \
	echo ""; \
	read -p "Commit this change? [Y/n] " -n 1 -r || true; \
	echo ""; \
	if [[ $$REPLY =~ ^[Yy]$$ ]] || [[ -z $$REPLY ]]; then \
	  git commit -am "Bump version to $${NEW}"; \
	fi

.PHONY: all help doc compile clean distclean test cover check dialyzer memcheck nif \
        optimize benchmark bench publish deprecate bump-version deps \
        bench-yaml bench-json bench-csv do-bench
