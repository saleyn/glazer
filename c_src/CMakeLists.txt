cmake_minimum_required(VERSION 3.14)
project(glazejson CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ENV{MAKEFLAGS} "--no-print-directory")

# ---- Detect <format> availability ----
# Some toolchains (e.g. GCC 12's libstdc++, or Clang against an older system
# libstdc++) don't ship a working <format> header, which Glaze otherwise
# includes by default (see glaze/core/write_chars.hpp). Probe for it here and
# tell Glaze to fall back to <cstdio>-based float formatting when missing.
include(CheckIncludeFileCXX)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_include_file_cxx("format" GLZ_HAVE_STD_FORMAT)
unset(CMAKE_REQUIRED_FLAGS)

# ---- Fetch glaze ----
include(FetchContent)
FetchContent_Declare(
  glaze
  GIT_REPOSITORY https://github.com/stephenberry/glaze.git
  GIT_TAG        v7.7.1
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glaze)

# ---- Erlang include paths ----
execute_process(
  COMMAND erl -noshell -noinput
    -eval "io:format(\"~ts/erts-~ts/include\", [code:root_dir(), erlang:system_info(version)]), halt(0)."
  OUTPUT_VARIABLE ERL_ERTS_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND erl -noshell -noinput
    -eval "io:format(\"~ts\", [code:lib_dir(erl_interface, include)]), halt(0)."
  OUTPUT_VARIABLE ERL_EI_INCLUDE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
execute_process(
  COMMAND erl -noshell -noinput
    -eval "io:format(\"~ts\", [code:lib_dir(erl_interface, lib)]), halt(0)."
  OUTPUT_VARIABLE ERL_EI_LIB
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# ---- NIF shared library ----
add_library(glazejson SHARED glaze_nif.cpp)

target_include_directories(glazejson PRIVATE
  ${ERL_ERTS_INCLUDE}
  ${ERL_EI_INCLUDE}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_directories(glazejson PRIVATE ${ERL_EI_LIB})
target_link_libraries(glazejson PRIVATE glaze::glaze ei)

# NIF symbols (enif_*) are resolved at load time by the BEAM, not at link
# time. On Linux this is fine by default (lazy symbol binding), but macOS's
# linker rejects undefined symbols in shared libraries unless told otherwise.
if(APPLE)
  target_link_options(glazejson PRIVATE -undefined dynamic_lookup)
endif()

if(NOT GLZ_HAVE_STD_FORMAT)
  target_compile_definitions(glazejson PRIVATE GLZ_USE_STD_FORMAT_FLOAT=0)
endif()

include(CheckCXXCompilerFlag)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_compiler_flag("-march=native" GLZ_HAVE_MARCH_NATIVE)
check_cxx_compiler_flag("-mtune=native" GLZ_HAVE_MTUNE_NATIVE)
unset(CMAKE_REQUIRED_FLAGS)

set(GLZ_NATIVE_FLAGS "")
if(GLZ_HAVE_MARCH_NATIVE)
  list(APPEND GLZ_NATIVE_FLAGS -march=native)
endif()
if(GLZ_HAVE_MTUNE_NATIVE)
  list(APPEND GLZ_NATIVE_FLAGS -mtune=native)
endif()

target_compile_options(glazejson PRIVATE
  -fPIC
  -Wall
  -finline-functions
  $<$<CONFIG:Release>:-O3 -DNDEBUG -flto=auto -fno-fat-lto-objects ${GLZ_NATIVE_FLAGS}>
  $<$<CONFIG:Debug>:-O0 -g>
)

# Remove the "lib" prefix so Erlang can load it as "glazejson.so"
set_target_properties(glazejson PROPERTIES
  PREFIX ""
  OUTPUT_NAME "glazejson"
)

# ---- Output to priv/ ----
if(DEFINED PRIV_DIR)
  set_target_properties(glazejson PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PRIV_DIR}"
  )
else()
  set_target_properties(glazejson PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../priv"
  )
endif()
