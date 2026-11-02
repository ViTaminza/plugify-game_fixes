# game_fixes — Plugify C++ plugin that ports the GameFixes fork (one Metamod:Source plugin with
# every CS2 server fix it ships, each toggled in game_fixes.ini): beam_crash, demo_record,
# hammer_id, slow_animation, workshop_voice, voice_fix, steam_ban, team_limit,
# input_activator_crash, sv_cheats, rampbug, water_jump, keyvalues_flood, team_switch_spam,
# voice_flood, blank_map, player_ghost, fake_messages, cstv_bot_quota and jump_spam. Hooks go
# through the polyhook plugin (s2sdk already hooks several of the same engine functions; polyhook
# reuses those hooks and just adds callbacks); signatures, offsets, vtables and the one byte patch
# live in assets/gamedata.jsonc.
#
# Pure s2sdk + polyhook (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no wrapper library. What
# pisexcompat used to provide lives in this plugin now — the KV1 reader (src/kv1.cpp), the config
# path resolution (src/paths.cpp), the B1/B2 map gate (src/map_gate.cpp), the per-fix option
# readers (src/config.cpp), the map-gated schema cache (src/schema_cache.hpp) and the chat output
# (src/output.hpp).
#
#   plugify-game_fixes  the plugin shared library (entry "bin/game_fixes" -> bin/libgame_fixes.so)

set(GAME_FIXES_VERSION "1.2.0")

# Signature hygiene: checks every pattern in assets/gamedata.jsonc against the shipped game
# binaries and writes SIGSCAN.md. Skips (passes) when the game files are not there.
add_executable(game_fixes_signature_check_tests
        tests/signature_check_tests.cpp
)
target_include_directories(game_fixes_signature_check_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(game_fixes_signature_check_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_fixes_signature_check_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(game_fixes_signature_check_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Pure-logic tests, no s2sdk/polyhook bindings involved: the ban-map writes (ban_map.hpp), the
# timer/next-frame pump and the per-map delayed-work teardown (scheduler.cpp), the map gate
# (map_gate.hpp), the KV1 reader and the option defaults of the shipped config (kv1.cpp/config.cpp),
# the config lookup order (paths.cpp), the map-gated schema cache (schema_cache.hpp) and the
# Itanium index encoding (api.hpp).
add_executable(game_fixes_core_tests
        tests/core_logic_tests.cpp
        src/config.cpp
        src/kv1.cpp
        src/map_gate.cpp
        src/paths.cpp
        src/scheduler.cpp
)
target_include_directories(game_fixes_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(game_fixes_core_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(game_fixes_core_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_fixes_core_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        # Lets the tests parse the config the plugin ships, so a broken block or default fails CI.
        GAME_FIXES_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/assets/game_fixes.ini"
)
set_target_properties(game_fixes_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# Package regression: package.sh must ship the plugin's own complete config
# (assets/game_fixes.ini, 20 fix blocks), never a fork work tree under ref/ (14 blocks), and must
# refuse to write outside its build output. The test runs the real script in a throwaway
# BUILD_DIR/OUT_DIR with fake artifacts; no engine and no network are involved.
add_executable(game_fixes_package_tests
        tests/package_tests.cpp
        src/config.cpp
        src/kv1.cpp
)
target_include_directories(game_fixes_package_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(game_fixes_package_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(game_fixes_package_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        # The test invokes this very package.sh and compares against its assets/game_fixes.ini.
        GAME_FIXES_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
set_target_properties(game_fixes_package_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

add_library(plugify-game_fixes SHARED
        src/plugin.cpp
        src/api.cpp
        src/config.cpp
        src/kv1.cpp
        src/map_gate.cpp
        src/paths.cpp
        src/scheduler.cpp
        src/fixes/beam_crash.cpp
        src/fixes/demo_record.cpp
        src/fixes/hammer_id.cpp
        src/fixes/input_activator_crash.cpp
        src/fixes/keyvalues_flood.cpp
        src/fixes/rampbug.cpp
        src/fixes/slow_animation.cpp
        src/fixes/steam_ban.cpp
        src/fixes/sv_cheats.cpp
        src/fixes/team_limit.cpp
        src/fixes/team_switch_spam.cpp
        src/fixes/voice_flood.cpp
        src/fixes/voice_fix.cpp
        src/fixes/water_jump.cpp
        src/fixes/workshop_voice.cpp
        src/fixes/blank_map.cpp
        src/fixes/player_ghost.cpp
        src/fixes/fake_messages.cpp
        src/fixes/cstv_bot_quota.cpp
        src/fixes/jump_spam.cpp
        # Binding globals (__s2sdk_* / __polyhook_*) filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:polyhook_bindings>
)
target_include_directories(plugify-game_fixes PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/polyhook/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-game_fixes SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-game_fixes PRIVATE
        -static-libstdc++ -static-libgcc
        ${CMAKE_DL_LIBS}
)
target_compile_options(plugify-game_fixes PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-game_fixes PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="game_fixes"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-game_fixes PROPERTIES OUTPUT_NAME game_fixes)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-game_fixes PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/game_fixes.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/game_fixes.pplugin"
)
