cmake_minimum_required(VERSION 3.24)
project(avc LANGUAGES CXX)

if(NOT WIN32)
    enable_language(ASM)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# `native` bakes the build host's ISA into the binary -- fine for a machine you
# build on, wrong for anything you hand to someone else. `none` keeps the
# x86-64 baseline, which already has SSE2 and is enough for the DSP nodes.
set(AVC_CPU_TARGET "none" CACHE STRING "-march target; 'none' keeps compiler defaults")
option(AVC_BUILD_TESTS "Build unit tests" ON)

include(cmake/Dependencies.cmake)

if(WIN32)
    if(MSVC)
        set(AVC_WARNINGS /W4 /permissive- /wd4100 /wd4244 /wd4267 /wd4324)
    else()
        set(AVC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wno-missing-field-initializers)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
    set(AVC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wno-missing-field-initializers)
endif()

add_library(avc_dsp STATIC
    src/log/Log.cpp
    src/rt/RtThread.cpp
    src/rt/ByteFifo.cpp
    src/rt/TimelineFifo.cpp
    src/rt/ValueSlot.cpp
    src/node/NodeRegistry.cpp
    src/node/PortTypeManifest.cpp
    src/ext/Library.cpp
    src/ext/PluginNode.cpp
    src/ext/ExtensionLoader.cpp
    src/node/basic/GainNode.cpp
    src/node/basic/MixerNode.cpp
    src/node/basic/SplitterNode.cpp
    src/node/basic/MeterNode.cpp
    src/node/voice/PitchNode.cpp
    src/node/voice/FormantNode.cpp
    src/node/eq/BiquadNode.cpp
    src/node/dyn/GateNode.cpp
    src/node/dyn/CompressorNode.cpp
    src/node/fx/RingModNode.cpp
    src/node/debug/SignalNode.cpp
    src/node/debug/ScopeNode.cpp
    src/node/debug/AbNode.cpp
    src/dsp/Biquad.cpp
    src/graph/BufferPool.cpp
    src/graph/GraphSpec.cpp
    src/graph/CompiledGraph.cpp
    src/graph/ColdWorker.cpp
    src/graph/GraphCompiler.cpp
    src/graph/GraphHost.cpp
)
target_include_directories(avc_dsp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(Threads REQUIRED)
target_link_libraries(avc_dsp PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json Threads::Threads
    ${CMAKE_DL_LIBS})
target_compile_options(avc_dsp PRIVATE ${AVC_WARNINGS})

# The built editor, compiled in so the engine ships as one binary. Vite changes
# hashed asset names on each content change, so let CMake notice additions and
# removals without requiring a manual reconfigure.
set(AVC_UI_DIST ${CMAKE_CURRENT_SOURCE_DIR}/ui/dist)
set(AVC_UI_CPP ${CMAKE_CURRENT_BINARY_DIR}/generated/ui_assets.cpp)
file(GLOB_RECURSE AVC_UI_FILES CONFIGURE_DEPENDS ${AVC_UI_DIST}/*)

if(WIN32)
    add_custom_command(
        OUTPUT ${AVC_UI_CPP}
        COMMAND ${CMAKE_COMMAND} -DUI_DIR=${AVC_UI_DIST} -DOUT_CPP=${AVC_UI_CPP}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUiCpp.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUiCpp.cmake ${AVC_UI_FILES}
        COMMENT "Embedding the editor")
    set(AVC_UI_SOURCES ${AVC_UI_CPP})
else()
    set(AVC_UI_ASM ${CMAKE_CURRENT_BINARY_DIR}/generated/ui_assets.S)
    add_custom_command(
        OUTPUT ${AVC_UI_ASM} ${AVC_UI_CPP}
        COMMAND ${CMAKE_COMMAND} -DUI_DIR=${AVC_UI_DIST} -DOUT_ASM=${AVC_UI_ASM}
                -DOUT_CPP=${AVC_UI_CPP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUi.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedUi.cmake ${AVC_UI_FILES}
        COMMENT "Embedding the editor")
    set(AVC_UI_SOURCES ${AVC_UI_ASM} ${AVC_UI_CPP})
endif()

if(WIN32)
    set(AVC_AUDIO_SOURCES
        src/audio/WasapiSession.cpp
        src/audio/WasapiVirtualDevices.cpp
        src/audio/WasapiBackend.cpp)
else()
    set(AVC_AUDIO_SOURCES
        src/audio/PwRegistry.cpp
        src/audio/PwSession.cpp
        src/audio/PwVirtualDevices.cpp
        src/audio/PipeWireBackend.cpp)
endif()

add_executable(avc
    ${AVC_UI_SOURCES}
    src/control/Serialization.cpp
    src/control/PresetStore.cpp
    src/control/StateStore.cpp
    src/control/ExtensionStore.cpp
    src/control/RvcModelManager.cpp
    src/control/ControlPlane.cpp
    src/audio/AudioBackend.cpp
    ${AVC_AUDIO_SOURCES}
    src/app/Ipc.cpp
    src/app/CrashGuard.cpp
    src/app/Daemon.cpp
    src/app/EngineChild.cpp
    src/app/Application.cpp
    src/app/main.cpp
)
target_link_libraries(avc PRIVATE avc_dsp httplib::httplib)
if(WIN32)
    target_include_directories(avc PRIVATE ${miniaudio_SOURCE_DIR})
    target_compile_definitions(avc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(avc PRIVATE ws2_32 avrt)
else()
    target_link_libraries(avc PRIVATE PkgConfig::PIPEWIRE)
endif()
target_compile_options(avc PRIVATE ${AVC_WARNINGS})

if(NOT AVC_CPU_TARGET STREQUAL "none" AND NOT MSVC)
    target_compile_options(avc_dsp PRIVATE -march=${AVC_CPU_TARGET})
    target_compile_options(avc PRIVATE -march=${AVC_CPU_TARGET})
endif()

option(AVC_BUILD_EXAMPLE_EXT "Build the example extension" ON)
if(AVC_BUILD_EXAMPLE_EXT)
    add_subdirectory(examples/ext)
endif()

option(AVC_BUILD_RVC_EXT "Build the native Linux RVC extension" OFF)
if(AVC_BUILD_RVC_EXT)
    if(WIN32 OR APPLE)
        message(FATAL_ERROR "AVC_BUILD_RVC_EXT currently supports Linux only")
    endif()
    add_subdirectory(extensions/rvc)
endif()

if(AVC_BUILD_TESTS)
    if(NOT TARGET GTest::gtest_main)
        find_package(GTest REQUIRED)
    endif()
    include(GoogleTest)
    enable_testing()

    function(avc_add_test name)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE avc_dsp GTest::gtest_main)
        target_compile_options(${name} PRIVATE ${AVC_WARNINGS})
        # PRE_TEST also lets a Windows cross-build compile the test binaries;
        # discovery then runs on the machine that can execute them.
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
    endfunction()

    avc_add_test(rt_test tests/rt_test.cpp)
    avc_add_test(dsp_test tests/dsp_test.cpp)
    avc_add_test(graph_test tests/graph_test.cpp)

    # The extension tests need something to load. gtest_discover_tests gives
    # each test its own process, so every one of them starts with an empty
    # registry and can load the plugin in whichever broken way it is about.
    add_subdirectory(tests/plugins)
    avc_add_test(ext_test tests/ext_test.cpp src/control/ExtensionStore.cpp)
    add_dependencies(ext_test avc_test_plugin)
    target_compile_definitions(ext_test PRIVATE
        AVC_TEST_PLUGIN_PATH="$<TARGET_FILE:avc_test_plugin>")

    avc_add_test(rvc_manifest_test
        tests/rvc_manifest_test.cpp
        tests/rvc_feature_index_test.cpp
        tests/rvc_rmvpe_frontend_test.cpp
        extensions/rvc/src/FeatureIndex.cpp
        extensions/rvc/src/RmvpeFrontend.cpp
        extensions/rvc/src/ModelManifest.cpp)
    target_include_directories(rvc_manifest_test PRIVATE extensions/rvc/include)

    avc_add_test(rvc_model_manager_test
        tests/rvc_model_manager_test.cpp
        src/control/RvcModelManager.cpp)
endif()

include(GNUInstallDirs)
install(TARGETS avc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/avc DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(WIN32)
    install(DIRECTORY DESTINATION ${CMAKE_INSTALL_BINDIR}/extensions)
else()
    install(DIRECTORY DESTINATION ${CMAKE_INSTALL_LIBDIR}/avc/extensions)
endif()

if(WIN32)
    message(STATUS "avc: Windows WASAPI, cpu target ${AVC_CPU_TARGET}")
else()
    message(STATUS "avc: PipeWire ${PIPEWIRE_VERSION}, cpu target ${AVC_CPU_TARGET}")
endif()
