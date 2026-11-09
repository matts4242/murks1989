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

# Kill rates in a stand-up fight on flat open ground (per-hit weight calibration). build/attrition_test calibrate
add_executable(attrition_test tests/attrition_test.cpp)
target_link_libraries(attrition_test PRIVATE murks_core)

# Checks the Historical Values rules (fire cycles, zones, radio, displacement, deployment). build/rules_test
add_executable(rules_test tests/rules_test.cpp)
target_link_libraries(rules_test PRIVATE murks_core)

# Gun ranges by equipment and the fire support net. build/fire_support_test
add_executable(fire_support_test tests/fire_support_test.cpp)
target_link_libraries(fire_support_test PRIVATE murks_core)

# Mounted infantry: dismount, mount up, losses aboard, AI drill. build/dismount_test
add_executable(dismount_test tests/dismount_test.cpp)
target_link_libraries(dismount_test PRIVATE murks_core)

# Suppression: build-up, hold, decay, pinning, armour and cover. build/suppression_test
add_executable(suppression_test tests/suppression_test.cpp)
target_link_libraries(suppression_test PRIVATE murks_core)

# Solo orders to team members and the Rejoin order (MRK-22). build/team_orders_test
add_executable(team_orders_test tests/team_orders_test.cpp)
target_link_libraries(team_orders_test PRIVATE murks_core)

# Real-map 4-day scripted runs over several seeds (needs zlib). build/playtest game/data/poland_1989.murkz 4 active 1989 8
find_package(ZLIB)
if(ZLIB_FOUND)
    add_executable(playtest tests/playtest.cpp)
    target_link_libraries(playtest PRIVATE murks_core ZLIB::ZLIB)
endif()
