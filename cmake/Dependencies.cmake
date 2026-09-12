# The vcpkg toolchain resolves these packages from the pinned manifest.
find_package(spdlog 1.12 CONFIG REQUIRED)
find_package(nlohmann_json 3.11 CONFIG REQUIRED)
# httplib's config only accepts the same minor version; the manifest baseline
# selects its version instead of treating an older minor as a minimum here.
find_package(httplib CONFIG REQUIRED)

if(WIN32)
    find_path(MINIAUDIO_INCLUDE_DIR NAMES miniaudio.h REQUIRED)
    find_package(unofficial-webview2 CONFIG REQUIRED)
endif()

if(AVC_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()
