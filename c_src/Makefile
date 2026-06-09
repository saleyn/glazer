CURDIR   := $(shell pwd)
BASEDIR	 := $(abspath $(dir $(CURDIR)))
CXX      ?= g++
PRIV_DIR ?= $(if $(REBAR_BARE_COMPILER_OUTPUT_DIR),$(REBAR_BARE_COMPILER_OUTPUT_DIR),$(BASEDIR))/priv
OBJ_DIR  ?= $(BASEDIR)/obj
DEBUG    ?= 0

SRC      := glaze_nif.cpp
OBJ      := $(OBJ_DIR)/glaze_nif.o
TARGET   := $(PRIV_DIR)/glazer.so

ERL_ERTS_INCLUDE := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts/erts-~ts/include\",[code:root_dir(),erlang:system_info(version)]),halt(0).")
ERL_EI_INCLUDE   := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts\",[code:lib_dir(erl_interface,include)]),halt(0).")
ERL_EI_LIB       := $(shell erl -noshell -noinput \
  -eval "io:format(\"~ts\",[code:lib_dir(erl_interface,lib)]),halt(0).")

CXXFLAGS := -std=c++23 -fPIC -Wall -finline-functions \
            -I$(ERL_ERTS_INCLUDE) -I$(ERL_EI_INCLUDE) -I.

# Detect compiler family for LTO flags
CXX_ID := $(shell $(CXX) --version 2>&1 | head -1)
ifneq ($(findstring clang,$(CXX_ID)),)
  LTO_FLAGS  := -flto=thin
  LINK_FLAGS := -fuse-ld=lld
else
  LTO_FLAGS  := -flto=auto -fno-fat-lto-objects
  LINK_FLAGS :=
endif

ifeq ($(DEBUG),1)
  CXXFLAGS += -O0 -g
else
  CXXFLAGS += -O3 -DNDEBUG -march=native -mtune=native $(LTO_FLAGS)
  LDFLAGS  += $(LINK_FLAGS)
endif

LDFLAGS += -shared -L$(ERL_EI_LIB) -lei
ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -undefined dynamic_lookup
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ) | $(PRIV_DIR)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@

$(OBJ): $(SRC) | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(PRIV_DIR) $(OBJ_DIR):
	@mkdir -p $@

clean:
	rm -f $(OBJ) $(TARGET)
