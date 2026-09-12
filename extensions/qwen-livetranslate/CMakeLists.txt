# The manifest enables curl's TLS and WebSocket features for this extension.
find_package(CURL 7.86 CONFIG REQUIRED)

add_library(avc_qwen_livetranslate MODULE
    src/AudioResampler.cpp
    src/Protocol.cpp
    src/SpeechNodes.cpp
    src/QwenLiveTranslateNode.cpp)
target_include_directories(avc_qwen_livetranslate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(avc_qwen_livetranslate PRIVATE
    CURL::libcurl
    nlohmann_json::nlohmann_json
    Threads::Threads)
target_compile_options(avc_qwen_livetranslate PRIVATE ${AVC_WARNINGS})
if(NOT MSVC)
    target_compile_options(avc_qwen_livetranslate PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_options(avc_qwen_livetranslate PRIVATE -Wl,--no-undefined)
else()
    target_compile_options(avc_qwen_livetranslate PRIVATE /utf-8)
    target_compile_definitions(avc_qwen_livetranslate PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
set_target_properties(avc_qwen_livetranslate PROPERTIES
    PREFIX ""
    OUTPUT_NAME "avc-qwen-livetranslate")

include(GNUInstallDirs)
if(WIN32)
    install(TARGETS avc_qwen_livetranslate
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/extensions
        LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/extensions
        COMPONENT qwen-livetranslate)
else()
    install(TARGETS avc_qwen_livetranslate
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/avc/extensions
        COMPONENT qwen-livetranslate)
endif()
