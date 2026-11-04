# Builds the engine-independent Murks core and its headless benchmark.
# (The Godot extension itself is built with SCons; see README.md.)
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release
#   build/core_bench 24 2        (Windows: build\Release\core_bench.exe 24 2)
cmake_minimum_required(VERSION 3.16)
project(murks_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(murks_core STATIC
    core/src/generator.cpp
    core/src/map.cpp
    core/src/pathfinder.cpp
    core/src/render_data.cpp
    core/src/sim.cpp
)
target_include_directories(murks_core PUBLIC core/include)

add_executable(core_bench tests/core_bench.cpp)
target_link_libraries(core_bench PRIVATE murks_core)

# Checks the HQ-loss rules (acting commander, reduced resupply, save/load). build/hq_loss_test
add_executable(hq_loss_test tests/hq_loss_test.cpp)
target_link_libraries(hq_loss_test PRIVATE murks_core)

# Adds local roads (and optionally rail) to a map baked by tools/bake_region.py.
add_executable(finish_map tools/finish_map.cpp)
target_link_libraries(finish_map PRIVATE murks_core)
