ifndef VERBOSE
MAKEFLAGS += --no-print-directory
endif

PRIV_DIR := $(abspath priv)
OBJ_DIR  := $(abspath obj)
DEBUG    ?= 0
REBAR    ?= rebar3
APP      := $(shell sed -n '/application,/{s/^.*, //; s/,.*$$//; p; q}' src/*.app.src)

all: compile

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

doc docs:
	$(REBAR) ex_doc

benchmark bench: deps
	@echo "Running benchmarks..."
	@mix bench

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

.PHONY: all deps doc compile clean distclean test nif
