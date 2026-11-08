cmake_minimum_required(VERSION 3.22)
project(LynxRack VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 8.0.12 next to this folder (../JUCE) or pass -DJUCE_DIR=...
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE" CACHE PATH "Path to JUCE 8.0.12")
add_subdirectory(${JUCE_DIR} JUCE)

# --- shared AlphaAudio plugin library (same code as Lemon Mixer Rack / FLP Organizer) -----------------------
set(LEMON_COMMON_SOURCES
    common/HostLog.h
    common/CrashGuard.h   common/CrashGuard.cpp
    common/FlDatabase.h   common/FlDatabase.cpp
    common/PluginDb.h     common/PluginDb.cpp
    common/PluginScanner.h common/PluginScanner.cpp)

# SQLite catalog (lemon_plugins.db). Drop your sqlite3.c + sqlite3.h into ThirdParty/sqlite/ (or point
# LEMON_SQLITE_DIR at them); on Linux/macOS the system library is used when found. Without either, the rack
# still works and writes plugins.xml + plugin_catalog.json, just not the .db.
set(LEMON_SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/sqlite" CACHE PATH "Folder holding sqlite3.c and sqlite3.h")
if(EXISTS "${LEMON_SQLITE_DIR}/sqlite3.c" AND EXISTS "${LEMON_SQLITE_DIR}/sqlite3.h")
    add_library(lemon_sqlite STATIC "${LEMON_SQLITE_DIR}/sqlite3.c")
    target_include_directories(lemon_sqlite PUBLIC "${LEMON_SQLITE_DIR}")
    target_compile_definitions(lemon_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
    set_target_properties(lemon_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set(LEMON_SQLITE_LIB lemon_sqlite)
    message(STATUS "Lynx Rack: SQLite from ${LEMON_SQLITE_DIR}")
else()
    find_package(SQLite3 QUIET)
    if(SQLite3_FOUND)
        set(LEMON_SQLITE_LIB SQLite::SQLite3)
        message(STATUS "Lynx Rack: system SQLite")
    else()
        set(LEMON_SQLITE_LIB "")
        message(STATUS "Lynx Rack: no SQLite found, the .db catalog is disabled (plugins.xml + plugin_catalog.json still written)")
    endif()
endif()

function(lynx_use_common target)
    target_sources(${target} PRIVATE ${LEMON_COMMON_SOURCES})
    if(LEMON_SQLITE_LIB)
        target_link_libraries(${target} PRIVATE ${LEMON_SQLITE_LIB})
    else()
        target_compile_definitions(${target} PRIVATE LEMON_NO_SQLITE=1)
    endif()
endfunction()

juce_add_plugin(LynxRack
    COMPANY_NAME                "AlphaAudio"
    PLUGIN_MANUFACTURER_CODE    Alph
    PLUGIN_CODE                 LyRk
    PRODUCT_NAME                "Lynx Rack"
    FORMATS                     VST3 Standalone
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           TRUE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Instrument Synth)

target_sources(LynxRack PRIVATE
    core/RackTypes.h
    core/ModuleLink.h
    core/RackEngine.h   core/RackEngine.cpp
    core/ChildHost.h    core/ChildHost.cpp
    core/RackProcessor.h core/RackProcessor.cpp
    ui/Theme.h
    ui/PianoRoll.h      ui/PianoRoll.cpp
    ui/RackStrip.h      ui/RackStrip.cpp
    ui/RackEditor.h     ui/RackEditor.cpp)

lynx_use_common(LynxRack)

# LemonScan: describes ONE plugin file in its own process, so a plugin that crashes or hangs while being scanned
# cannot take the DAW down. Built here and copied next to the plugin after every build.
juce_add_console_app(LemonScan PRODUCT_NAME "LemonScan")
target_sources(LemonScan PRIVATE tools/LemonScan/ScanMain.cpp)
target_compile_definitions(LemonScan PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(LemonScan PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
foreach(t LynxRack_VST3 LynxRack_Standalone)
    if(TARGET ${t})
        add_dependencies(${t} LemonScan)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:LemonScan>" "$<TARGET_FILE_DIR:${t}>/"
            COMMENT "Copying LemonScan next to ${t}")
    endif()
endforeach()

target_compile_definitions(LynxRack PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=1          # the rack hosts VST3 plugins
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(LynxRack
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(LynxRack PRIVATE /EHsc /permissive-)   # /EHsc: SEH guard around hosted plugins stays well-defined
endif()

# Headless engine tests: cmake -DLYNX_BUILD_TESTS=ON ... && ctest
option(LYNX_BUILD_TESTS "Build the headless engine tests" OFF)
if(LYNX_BUILD_TESTS)
    juce_add_console_app(LynxRackTests PRODUCT_NAME "LynxRackTests")
    target_sources(LynxRackTests PRIVATE tests/EngineTest.cpp core/RackEngine.cpp)
    target_compile_definitions(LynxRackTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=0 JUCE_USE_WIN32_SEH=0)
    target_link_libraries(LynxRackTests PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
    enable_testing()
    add_test(NAME engine COMMAND LynxRackTests)
endif()

if(LYNX_BUILD_TESTS)
    juce_add_console_app(LynxRackUiShot PRODUCT_NAME "LynxRackUiShot")
    lynx_use_common(LynxRackUiShot)
    target_sources(LynxRackUiShot PRIVATE tests/UiShot.cpp
        core/RackEngine.cpp core/ChildHost.cpp core/RackProcessor.cpp ui/PianoRoll.cpp ui/RackStrip.cpp ui/RackEditor.cpp)
    target_compile_definitions(LynxRackUiShot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(LynxRackUiShot PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
endif()

if(LYNX_BUILD_TESTS)
    juce_add_console_app(LynxRackLoadTest PRODUCT_NAME "LynxRackLoadTest")
    target_sources(LynxRackLoadTest PRIVATE tests/PluginLoadTest.cpp)
    target_compile_definitions(LynxRackLoadTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(LynxRackLoadTest PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
endif()

if(LYNX_BUILD_TESTS)
    # A minimal VST3 instrument and an end-to-end host test (scan -> catalog -> load -> play -> state -> quarantine)
    juce_add_plugin(LynxTestSynth PRODUCT_NAME "LynxTestSynth" COMPANY_NAME "AlphaAudio" PLUGIN_MANUFACTURER_CODE Alph PLUGIN_CODE LyTs
        FORMATS VST3 IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE COPY_PLUGIN_AFTER_BUILD FALSE VST3_CATEGORIES Instrument Synth)
    target_sources(LynxTestSynth PRIVATE tests/TestSynth.cpp)
    target_compile_definitions(LynxTestSynth PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(LynxTestSynth PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)

    juce_add_console_app(LynxRackHostTest PRODUCT_NAME "LynxRackHostTest")
    lynx_use_common(LynxRackHostTest)
    target_sources(LynxRackHostTest PRIVATE tests/HostTest.cpp core/RackEngine.cpp core/ChildHost.cpp)
    target_compile_definitions(LynxRackHostTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_PLUGINHOST_VST3=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(LynxRackHostTest PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_recommended_config_flags)
    add_dependencies(LynxRackHostTest LemonScan LynxTestSynth_VST3)
    add_test(NAME hosttest COMMAND LynxRackHostTest $<TARGET_FILE:LemonScan> $<TARGET_FILE_DIR:LynxTestSynth_VST3>)
endif()
