REBAR_BUILD_DIR ?= _build/default
BUILD_DIR  ?= $(REBAR_BUILD_DIR)/lib/glazejson/c_src
PRIV_DIR   := $(abspath priv)
BUILD_TYPE ?= Release
NIF_DEBUG  ?= 0

ifeq ($(NIF_DEBUG),1)
  BUILD_TYPE := Debug
endif

all: compile

deps: nif

nif: $(PRIV_DIR)/glazejson.so

$(PRIV_DIR)/glazejson.so: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) $(if $(VERBOSE),--verbose,)

$(BUILD_DIR)/Makefile: $(BUILD_DIR) $(PRIV_DIR)
	cmake -S c_src -B $(BUILD_DIR) \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	  -DPRIV_DIR=$(PRIV_DIR)

$(PRIV_DIR) $(BUILD_DIR):
	mkdir -p $@

compile: nif
	rebar3 compile

clean:
	rebar3 clean
	cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean: clean
	@rm -rf $(BUILD_DIR) priv/glazejson.so
	@rm -fr _build

test:
	rebar3 eunit

benchmark:
	mix bench

.PHONY: all deps compile clean distclean test nif
