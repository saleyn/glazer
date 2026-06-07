cmake_minimum_required(VERSION 3.14)
project(glazejson CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ENV{MAKEFLAGS} "--no-print-directory")

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

target_compile_options(glazejson PRIVATE
  -fPIC
  -Wall
  -finline-functions
  $<$<CONFIG:Release>:-O3 -DNDEBUG -flto=auto -fno-fat-lto-objects -march=native -mtune=native>
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
