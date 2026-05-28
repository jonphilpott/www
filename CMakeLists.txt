cmake_minimum_required(VERSION 3.14)
project(www CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# llama.cpp — the inference engine.
# FetchContent clones it once into your build directory; subsequent cmake runs
# use the cached copy. To pin to a specific version replace GIT_TAG with e.g.
# GIT_TAG b5000  (find tags at https://github.com/ggerganov/llama.cpp/tags)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    llama
    GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
    GIT_SHALLOW    TRUE         # skip full history — much faster clone
    GIT_PROGRESS   TRUE         # show git progress so it doesn't look frozen
)

# Turn off everything in llama.cpp we don't need so the build stays fast
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(llama)

# ---------------------------------------------------------------------------
# cpp-httplib — a single-header HTTP server. We pull the repo and add its
# root directory to our include path; no CMake target needed, just httplib.h.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(httplib)

# ---------------------------------------------------------------------------
# Our binary
# ---------------------------------------------------------------------------
add_executable(www
    src/main.cpp
    src/generator.cpp
    src/renderer.cpp
)

target_include_directories(www PRIVATE
    src/
    ${httplib_SOURCE_DIR}   # gives us #include "httplib.h"
)

target_link_libraries(www PRIVATE llama)

# -O3 -march=native makes a meaningful difference to inference throughput;
# the CPU-specific tuning is fine here because we're building to run locally.
target_compile_options(www PRIVATE -O3 -march=native)
