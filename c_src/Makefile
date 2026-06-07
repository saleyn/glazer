# NIF build rules — invoked from the root Makefile via `$(MAKE) -C c_src ...`,
# so the working directory here is c_src/ and paths are relative to it.

# rebar3 sets REBAR_BUILD_DIR (e.g. _build/default or _build/test) for hooks,
# relative to the project root; fall back to _build/default otherwise.
REBAR_BUILD_DIR ?= $(abspath ../_build/default)
BUILD_DIR  ?= $(REBAR_BUILD_DIR)/lib/glazejson/c_src
PRIV_DIR   := $(abspath ../priv)
BUILD_TYPE ?= Release
NIF_DEBUG  ?= 0

ifeq ($(NIF_DEBUG),1)
  BUILD_TYPE := Debug
endif

nif: $(PRIV_DIR)/glazejson.so

$(PRIV_DIR)/glazejson.so: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) $(if $(VERBOSE),--verbose,)
	@mkdir -p $(PRIV_DIR)

$(BUILD_DIR)/Makefile:
	@mkdir -p $(BUILD_DIR)
	cmake -S . -B $(BUILD_DIR) \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	  -DPRIV_DIR=$(PRIV_DIR)

clean:
	cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

distclean:
	rm -rf $(BUILD_DIR) $(PRIV_DIR)/glazejson.so

.PHONY: nif clean distclean
