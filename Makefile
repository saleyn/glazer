ifndef VERBOSE
MAKEFLAGS += --no-print-directory
endif

REBAR_BUILD_DIR ?= _build/default
BUILD_DIR  ?= $(REBAR_BUILD_DIR)/lib/glazejson/.build
PRIV_DIR   := $(abspath priv)
BUILD_TYPE ?= Release
NIF_DEBUG  ?= 0
REBAR			 ?= rebar3
APP        := $(shell sed -n '/application,/{s/^.*, //; s/,.*$$//; p; q}' src/*.app.src)

ifeq ($(NIF_DEBUG),1)
  BUILD_TYPE := Debug
endif

all: compile

deps: nif

nif: $(PRIV_DIR)/glazejson.so

$(PRIV_DIR)/glazejson.so: $(BUILD_DIR)/Makefile
	@cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) $(if $(VERBOSE),--verbose,) -- --no-print-directory

$(BUILD_DIR)/Makefile: $(BUILD_DIR) $(PRIV_DIR)
	@cmake -S c_src -B $(BUILD_DIR) \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	  -DPRIV_DIR=$(PRIV_DIR) \
	  $(if $(VERBOSE),,>/dev/null)

$(PRIV_DIR) $(BUILD_DIR):
	@mkdir -p $@

compile: nif
	$(REBAR) compile

clean:
	$(REBAR) clean
	@cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean: clean
	@rm -rf $(BUILD_DIR) priv/glazejson.so
	@rm -fr _build

test:
	$(REBAR) eunit

doc docs:
	$(REBAR) ex_doc

benchmark bench:
	@echo "Running benchmarks..."
	@mix bench

publish: docs
	$(REBAR) hex publish$(if $(replace), --replace)

deprecate:
	@if [ -z $(vsn) ]; then \
    echo "Usage: $(MAKE) $@ vsn=X.Y.Z      - Deprecate version X.Y.Z"; \
    exit 1; \
  fi
	$(REBAR) hex retire $(APP) $(vsn) deprecated --message Deprecated


.PHONY: all deps doc compile clean distclean test nif
