include(FetchContent)

# Curl's WebSocket API arrived in 7.86. Bundle a current, minimal static build by
# default so Windows uses Schannel and an installed AVC extension has no extra
# runtime DLL. Packagers can opt into a recent system libcurl on Linux.
option(AVC_QWEN_USE_SYSTEM_CURL "Use a system libcurl >= 7.86" OFF)

if(AVC_QWEN_USE_SYSTEM_CURL)
    find_package(CURL 7.86 REQUIRED)
else()
    enable_language(C)
    set(BUILD_CURL_EXE OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_LIBCURL_DOCS OFF)
    set(BUILD_MISC_DOCS OFF)
    set(ENABLE_CURL_MANUAL OFF)
    set(BUILD_TESTING OFF)
    set(CURL_ENABLE_EXPORT_TARGET OFF)
    set(CURL_DISABLE_INSTALL ON)
    set(CURL_DISABLE_WEBSOCKETS OFF)
    set(CURL_USE_LIBPSL OFF)
    set(CURL_ZLIB OFF)
    set(CURL_BROTLI OFF)
    set(CURL_ZSTD OFF)
    set(USE_LIBIDN2 OFF)
    set(USE_NGHTTP2 OFF)
    set(USE_NGTCP2 OFF)
    set(USE_QUICHE OFF)
    set(USE_MSH3 OFF)
    set(CURL_USE_GSSAPI OFF)
    set(CURL_USE_LIBSSH2 OFF)
    set(CURL_USE_LIBSSH OFF)
    set(CURL_DISABLE_ALTSVC ON)
    set(CURL_DISABLE_COOKIES ON)
    set(CURL_DISABLE_DOH ON)
    set(CURL_DISABLE_HSTS ON)
    set(CURL_DISABLE_MIME ON)
    set(CURL_DISABLE_NETRC ON)
    set(CURL_DISABLE_NTLM ON)
    set(CURL_DISABLE_BINDLOCAL ON)
    set(CURL_DISABLE_DICT ON)
    set(CURL_DISABLE_FILE ON)
    set(CURL_DISABLE_FTP ON)
    set(CURL_DISABLE_GOPHER ON)
    set(CURL_DISABLE_IMAP ON)
    set(CURL_DISABLE_IPFS ON)
    set(CURL_DISABLE_LDAP ON)
    set(CURL_DISABLE_LDAPS ON)
    set(CURL_DISABLE_MQTT ON)
    set(CURL_DISABLE_POP3 ON)
    set(CURL_DISABLE_RTSP ON)
    set(CURL_DISABLE_SMB ON)
    set(CURL_DISABLE_SMTP ON)
    set(CURL_DISABLE_TELNET ON)
    set(CURL_DISABLE_TFTP ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    if(WIN32)
        set(CURL_USE_SCHANNEL ON)
        set(CURL_USE_OPENSSL OFF)
    else()
        find_package(OpenSSL REQUIRED)
        set(CURL_USE_OPENSSL ON)
    endif()

    FetchContent_Declare(avc_qwen_curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_21_0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(avc_qwen_curl)
endif()

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
if(NOT AVC_QWEN_USE_SYSTEM_CURL)
    install(FILES "${avc_qwen_curl_SOURCE_DIR}/COPYING"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/avc/licenses
        RENAME curl-LICENSE.txt
        COMPONENT qwen-livetranslate)
endif()
