ifndef VERBOSE
MAKEFLAGS += --no-print-directory
endif

# Parallel build: one job per CPU. Override with JOBS=n (JOBS=1 for serial).
# Windows sets NUMBER_OF_PROCESSORS; macOS has sysctl hw.ncpu; Linux has nproc.
ifndef JOBS
  ifeq ($(OS),Windows_NT)
    JOBS := $(NUMBER_OF_PROCESSORS)
  else ifeq ($(shell uname -s),Darwin)
    JOBS := $(shell sysctl -n hw.ncpu 2>/dev/null)
  else
    JOBS := $(shell nproc 2>/dev/null)
  endif
endif
ifeq ($(strip $(JOBS)),)
  JOBS := 1
endif
MAKEFLAGS += -j$(JOBS)

PRIV_DIR ?= $(if $(REBAR_BARE_COMPILER_OUTPUT_DIR),$(REBAR_BARE_COMPILER_OUTPUT_DIR)/priv,$(abspath priv))
REBAR    ?= rebar3
APP      := $(shell sed -nE 's/^\{application, ([a-zA-Z0-9_]+),.*/\1/p' src/*.app.src | head -n1)
# OPTIMIZE: 1 forces the PGO build, 0 forces no PGO, empty (default) picks
# PGO for a prod profile or GLAZER_PGO=1 (see c_src/Makefile).
OPTIMIZE ?=
ASAN     ?= 0
MIX_ENV  ?= default

ifneq ($(filter $(OPTIMIZE),1 true),)
OPTIMIZE := 1
else ifneq ($(filter $(OPTIMIZE),0 false),)
OPTIMIZE := 0
else
OPTIMIZE :=
endif

export ASAN

all: compile

help:
	@echo "Usage: make [target] [VAR=value ...]"
	@echo
	@echo "Build targets:"
	@echo "  all          Build NIF and compile Erlang (default)"
	@echo "  nif          Build only the C NIF shared library"
	@echo "  compile      Build NIF + run rebar3 compile"
	@echo "  release      Build NIF + run rebar3 as prod compile"
	@echo "  optimize     PGO build: instrument → benchmark → rebuild (fastest binary)"
	@echo "  clean        Remove build artifacts"
	@echo "  distclean    Remove all generated files including deps"
	@echo
	@echo "Development:"
	@echo "  test         Run eunit test suite"
	@echo "  cover        Run eunit with coverage analysis and print a summary"
	@echo "  check        Run xref and dialyzer"
	@echo "  dialyzer     Run dialyzer"
	@echo "  memcheck     Build with ASan (-fsanitize=address) and run eunit (leak=1 adds LSan)"
	@echo "  benchmark    Run benchmarks via mix bench"
	@echo "  benchmark-sv Benchmark schema validation against other Elixir JSON Schema libraries"
	@echo "  deps         Fetch mix dependencies"
	@echo "  doc          Generate documentation"
	@echo "  idna-tables  Regenerate c_src/glazer_idna_tables.hpp from ICU (needs icu-uc)"
	@echo
	@echo "Publishing:"
	@echo "  bump-version Increment patch version and commit"
	@echo "  publish      Publish to hex.pm  (pass replace=1 to replace existing)"
	@echo "  deprecate    Deprecate a hex.pm release  (pass vsn=X.Y.Z)"
	@echo
	@echo "Variables:"
	@echo "  ASAN=1       Build with AddressSanitizer (implied by memcheck)"
	@echo "  leak=1       Enable LeakSanitizer during memcheck (off by default)"
	@echo "  VERBOSE=1    Show full compiler command lines"
	@echo "  OPTIMIZE=1   Force the PGO 'optimize' build (same as 'make optimize')"
	@echo "  OPTIMIZE=0   Never use PGO"
	@echo "  GLAZER_PGO=1 Use PGO (CI); unset, PGO is used for a prod profile only (MIX_ENV=prod / rebar3 as prod)"
	@echo "  PRIORITY=N   For bench-* targets: run as 'sudo -E nice -N bash -c make <target>' (nice -20 is highest priority)"
	@echo "  N=5          For bench-* targets: run benchmark N times and print the minimum of each column (bin/bench-min.awk)"
	@echo
	@echo "Examples:"
	@echo "  PRIORITY=-20 N=2 OPTIMIZE=1 TOP=2 make bench-json  # run JSON benchmark twice with PGO and high priority"

compile: deps
	@$(REBAR) as $(MIX_ENV) $@

release: deps
	@$(REBAR) as prod compile

info:
	@$(MAKE) -C c_src $@

nif:
	@[ -n "$(SKIP_NIF_RECOMPILE)" ] && echo "===> Skipping NIF compile" || $(MAKE) $(if $(VERBOSE),,-s) -C c_src OPTIMIZE=$(OPTIMIZE) PRIV_DIR=$(PRIV_DIR) compile

clean:
	@$(REBAR) clean
	@$(MAKE) -C c_src clean 2>/dev/null || true

distclean: clean
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) clean 2>/dev/null || true
	@rm -rf obj _build .perf.txt

test:
	@#rm -rf _build/test obj priv/glazer.so
	@mix test
	@$(REBAR) as test eunit
	@$(MAKE) test-jsts

# IDNA/UTS 46 tables: c_src/glazer_idna_tables.hpp is generated from ICU by
# c_src/tools/gen_idna_tables.cpp. Run this after a Unicode/ICU update. The
# committed header is what the NIF build uses, so nothing here runs by default.
IDNA_CXX     ?= g++
IDNA_GEN_BIN := _build/gen_idna_tables
IDNA_HEADER  := c_src/glazer_idna_tables.hpp

idna-tables:
	@mkdir -p _build
	@$(IDNA_CXX) -std=c++17 -O2 c_src/tools/gen_idna_tables.cpp -o $(IDNA_GEN_BIN) \
	  $$(pkg-config --cflags --libs icu-uc)
	@$(IDNA_GEN_BIN) > $(IDNA_HEADER).tmp && mv $(IDNA_HEADER).tmp $(IDNA_HEADER)
	@echo "===> Regenerated $(IDNA_HEADER)"

.PHONY: idna-tables

# Official JSON Schema test suite (2020-12 scope), see test/json_schema_suite.es.
# Not part of `make test`: the first run needs network access to fetch the suite.
# Set JSTS_DIR to a local checkout of json-schema-org/JSON-Schema-Test-Suite to
# skip the fetch.
JSTS_REPO   := https://github.com/json-schema-org/JSON-Schema-Test-Suite.git
JSTS_COMMIT := 38dea93
JSTS_DIR    ?= _build/json-schema-test-suite

test-jsts:
	@if [ ! -d "$(JSTS_DIR)/validation/tests" ]; then \
	  command -v git >/dev/null 2>&1 || { echo "Error: git is required to fetch the JSON Schema test suite"; exit 1; }; \
	  echo "===> Fetching JSON-Schema-Test-Suite $(JSTS_COMMIT) into $(JSTS_DIR)"; \
	  git clone -q --filter=blob:none --no-checkout $(JSTS_REPO) "$(JSTS_DIR)" && \
	  git -C "$(JSTS_DIR)" checkout -q $(JSTS_COMMIT) || { echo "Error: could not check out $(JSTS_COMMIT)"; exit 1; }; \
	fi
	@$(REBAR) as test compile
	@echo "===> Running JSON Schema Test Suite tests from $(JSTS_DIR)/validation/tests"
	@test/json_schema_suite.es "$(JSTS_DIR)/validation/tests"

.PHONY: test-jsts

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

ifdef N
ifneq ($(shell [ "$(N)" -ge 1 ] 2>/dev/null && echo ok),ok)
$(error N must be a positive integer, got '$(N)')
endif
endif

do-bench: deps bench-deps
	@rm -f .perf.txt
	@$(MAKE) PRIV_DIR=$(PRIV_DIR) optimize
	@# SKIP_NIF_RECOMPILE: mix's :make compiler would rebuild the NIF in the bench
	@# env, which is not a PGO profile, and replace the PGO library built above.
	@# N=k runs mix bench k times and writes one table per format with the minimum
	@# of each column over the runs (bin/bench-min.awk).
	@if [ -n "$(N)" ]; then \
	  for i in $$(seq 1 $(N)); do echo "===> run $$i/$(N)" >&2; echo "@@RUN@@"; SKIP_NIF_RECOMPILE=1 MIX_ENV=bench mix bench; done | awk -f bin/bench-min.awk | tee .perf.txt; \
	else \
	  SKIP_NIF_RECOMPILE=1 MIX_ENV=bench mix bench | tee .perf.txt; \
	fi

# ELIXIR_ERL_OPTIONS quiets rustler_precompiled's [debug] "Copying NIF from
# cache and extracting..." noise (torque/yaml_rustler/rusty_csv all use it)
# without touching the project-wide Logger level — scoped to bench targets
# only via this env var, not a global :logger config in mix.exs.

# bench-only deps (e.g. ymlr, `only: :bench`) are not fetched by `deps`, which
# only runs `mix deps.get` when mix.lock is older than mix.exs. So check that
# every package locked in mix.lock has a deps/<name> directory, and only run
# mix deps.get when one is missing. Totals are not compared: deps/ also holds
# transitive packages that mix.lock does not list.
bench-deps: deps
	@want=$$(grep -cE '^  "[a-z0-9_]+": \{:' mix.lock); \
	have=0; \
	for d in $$(grep -oE '^  "[a-z0-9_]+": \{:' mix.lock | sed -E 's/^  "([a-z0-9_]+)".*/\1/'); do \
	  [ -d deps/$$d ] && have=$$((have + 1)); \
	done; \
	if [ "$$have" -ne "$$want" ]; then \
	  echo "bench-deps: $$have of $$want locked deps present, running mix deps.get"; \
	  MIX_ENV=bench mix deps.get; \
	else \
	  echo "bench-deps: all $$want locked deps present in deps/, skipping fetch"; \
	fi; \
	out=$$(MIX_ENV=bench mix deps 2>&1); \
	case "$$out" in *"not available"*) \
	  echo "bench-deps: mix reports bench dependencies as not available, running mix deps.get"; \
	  MIX_ENV=bench mix deps.get ;; \
	esac

# PRIORITY=N runs the target as: sudo -E nice -N bash -c 'make <target>'.
# The inner make has PRIORITY cleared and BENCH_PREPARED set, so it skips the
# deps prerequisites (already done by this make) and runs the mix command below.
bench-json bench-yaml bench-csv benchmark-sv: export ELIXIR_ERL_OPTIONS = -logger level warning

# benchmark-sv runs on an O3 NIF without PGO. c_src/Makefile selects O3 for the prod
# profile (MIX_ENV=prod), and OPTIMIZE=0 turns PGO off. Target-specific variables
# reach the prerequisites, and bench-nif passes MIX_ENV on to the NIF build.
benchmark-sv: MIX_ENV := prod
benchmark-sv: OPTIMIZE := 0

# The NIF is built once, here, before mix runs (OPTIMIZE picks PGO or not), into
# the directory the bench env loads from. Mix runs with SKIP_NIF_RECOMPILE=1, so
# it never rebuilds the NIF mid-run, and the build output stays out of the table.
# The prerequisites form a chain (deps -> bench-deps -> bench-nif -> bench-compile).
# With -j, independent prerequisites run concurrently, so bench-compile must not
# be a sibling of bench-deps: it would run before the bench deps are fetched.
bench-nif: bench-deps
	@# mix links _build/bench/lib/glazer/priv to the repo's priv/. Replace that link with
	@# a real directory, so the bench NIF is never written into the shared priv/glazer.so
	@# (a dev or test build there would otherwise replace the PGO library being benchmarked).
	@if [ -L "$(CURDIR)/_build/bench/lib/glazer/priv" ]; then rm -f "$(CURDIR)/_build/bench/lib/glazer/priv"; fi
	@mkdir -p "$(CURDIR)/_build/bench/lib/glazer/priv"
	@# Every mix run copies the repo's priv/ into that directory when sizes differ (Mix's
	@# build_structure / Mix.Utils.symlink_or_copy). A non-PGO priv/glazer.so left by `make
	@# compile` would then replace the PGO build, so remove it before mix runs.
	@rm -f "$(CURDIR)/priv/glazer.so"
	@$(MAKE) MIX_ENV=$(MIX_ENV) OPTIMIZE=$(OPTIMIZE) PRIV_DIR="$(CURDIR)/_build/bench/lib/glazer/priv" REBAR_BARE_COMPILER_OUTPUT_DIR="$(CURDIR)/_build/bench/lib/glazer" nif

bench-compile: bench-nif
	@SKIP_NIF_RECOMPILE=1 MIX_ENV=bench mix compile
	@# Fail before benchmarking if OPTIMIZE=1 but the NIF that will be loaded is not the PGO build.
	@if [ "$(OPTIMIZE)" = 1 ] && ! strings "$(CURDIR)/_build/bench/lib/glazer/priv/glazer.so" | grep -q 'GLAZER_PGO_OPTIMIZED=1'; then \
	  echo "error: OPTIMIZE=1 but _build/bench/lib/glazer/priv/glazer.so is not PGO-optimized" >&2; exit 1; \
	fi

bench-json bench-yaml bench-csv benchmark-sv: $(if $(BENCH_PREPARED),,bench-compile)
# N=k runs the benchmark k times and prints one table with the minimum of each
# column over the runs (bin/bench-min.awk).
ifdef N
ifneq ($(shell [ "$(N)" -ge 1 ] 2>/dev/null && echo ok),ok)
$(error N must be a positive integer, got '$(N)')
endif
endif
# PRIORITY and N are independent. PRIORITY decides whether the run is wrapped in
# sudo nice (the inner make gets N too). N decides whether the run repeats.
ifdef PRIORITY
	@# sudo resets PATH to secure_path (and may ignore -E), which hides the mix installed by
	@# setup-beam or asdf. Pass PATH and HOME explicitly so the inner make finds mix and the Hex cache.
	@sudo env PATH="$$PATH" HOME="$$HOME" nice $(if $(filter -%,$(PRIORITY)),-n $(PRIORITY),-$(PRIORITY)) bash -c 'OPTIMIZE=$(OPTIMIZE) TOP=$(TOP) $(MAKE) $@ PRIORITY= BENCH_PREPARED=1 $(if $(N),N=$(N))'
else
	@if [ -n "$(N)" ]; then \
	  for i in $$(seq 1 $(N)); do echo "===> run $$i/$(N)" >&2; echo "@@RUN@@"; SKIP_NIF_RECOMPILE=1 TOP=$(if $(TOP),$(TOP),) MIX_ENV=bench mix $@ $(if $(TOP),--top=$(TOP)); done | awk -f bin/bench-min.awk; \
	else \
	  SKIP_NIF_RECOMPILE=1 TOP=$(if $(TOP),$(TOP),) MIX_ENV=bench mix $@ $(if $(TOP),--top=$(TOP)); \
	fi
endif

# Profile-guided optimisation: instrument → run tests as workload → rebuild.
# Usage: make optimize
# Skip PGO when building as a dependency (pgo-profile.es won't be available in Hex distributions)
# Profile-guided optimisation: let c_src/Makefile handle the decision
optimize:
	@$(MAKE) -C c_src PRIV_DIR=$(PRIV_DIR) $@

deps: mix.lock

mix.lock: mix.exs
	@rm -f mix.lock
	@mix deps.get

publish: docs
	$(REBAR) hex publish$(if $(replace), --replace)

deprecate:
	@if [ -z $(vsn) ]; then \
	  echo "Usage: $(MAKE) $@ vsn=X.Y.Z      - Deprecate version X.Y.Z"; \
	  exit 1; \
	fi
	$(REBAR) hex retire $(APP) $(vsn) deprecated --message $(if $(message),"$(message)",Deprecated)

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
        optimize benchmark bench publish deprecate bump-version \
        bench-yaml bench-json bench-csv benchmark-sv bench-deps bench-nif bench-compile do-bench
