include(GNUInstallDirs)
include(FetchContent)

set(ONNXRUNTIME_ROOT "" CACHE PATH
    "Optional extracted ONNX Runtime C/C++ distribution; empty downloads the pinned release")
option(AVC_RVC_ENABLE_CUDA "Enable the ONNX Runtime CUDA execution provider" OFF)
option(AVC_RVC_ONNXRUNTIME_CUDA13 "Use the CUDA 13 ONNX Runtime package" OFF)
option(AVC_RVC_BUNDLE_ONNXRUNTIME "Install ONNX Runtime beside the RVC extension" ON)
option(AVC_RVC_BUNDLE_CONVERTER
    "Bundle a self-contained PTH to ONNX converter and shared RVC base models" ON)

# Keep model execution reproducible. GitHub publishes SHA-256 digests in the
# release metadata; updating the version means deliberately updating these too.
set(_avc_ort_version "1.24.4")

if(NOT ONNXRUNTIME_ROOT)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _avc_rvc_arch)
    if(_avc_rvc_arch MATCHES "^(x86_64|amd64)$")
        if(AVC_RVC_ENABLE_CUDA AND AVC_RVC_ONNXRUNTIME_CUDA13)
            set(_avc_ort_archive "onnxruntime-linux-x64-gpu_cuda13-${_avc_ort_version}.tgz")
            set(_avc_ort_sha256 "fdc6eb18317b4eaeda8b3b86595e5da7e853f72bac67ccac9b04ffc20c9f7fe0")
        elseif(AVC_RVC_ENABLE_CUDA)
            set(_avc_ort_archive "onnxruntime-linux-x64-gpu-${_avc_ort_version}.tgz")
            set(_avc_ort_sha256 "c5f804ff5d239b436fa59e9f2fb288a39f7eb9552f6a636c8b71e792e91a8808")
        else()
            set(_avc_ort_archive "onnxruntime-linux-x64-${_avc_ort_version}.tgz")
            set(_avc_ort_sha256 "3a211fbea252c1e66290658f1b735b772056149f28321e71c308942cdb54b747")
        endif()
    elseif(_avc_rvc_arch MATCHES "^(aarch64|arm64)$")
        if(AVC_RVC_ENABLE_CUDA)
            message(FATAL_ERROR
                "The pinned ONNX Runtime release has no Linux aarch64 GPU archive; set ONNXRUNTIME_ROOT to a compatible custom build")
        endif()
        set(_avc_ort_archive "onnxruntime-linux-aarch64-${_avc_ort_version}.tgz")
        set(_avc_ort_sha256 "866109a9248d057671a039b9d725be4bd86888e3754140e6701ec621be9d4d7e")
    else()
        message(FATAL_ERROR
            "Automatic ONNX Runtime download does not support '${CMAKE_SYSTEM_PROCESSOR}'; set ONNXRUNTIME_ROOT explicitly")
    endif()

    set(_avc_ort_url
        "https://github.com/microsoft/onnxruntime/releases/download/v${_avc_ort_version}/${_avc_ort_archive}")
    message(STATUS "avc-rvc: downloading pinned ONNX Runtime ${_avc_ort_version} (${_avc_ort_archive})")
    FetchContent_Declare(avc_onnxruntime
        URL "${_avc_ort_url}"
        URL_HASH "SHA256=${_avc_ort_sha256}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(avc_onnxruntime)
    set(ONNXRUNTIME_ROOT "${avc_onnxruntime_SOURCE_DIR}")
else()
    message(STATUS "avc-rvc: using ONNX Runtime from ${ONNXRUNTIME_ROOT}")
endif()

find_path(ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    PATHS ${ONNXRUNTIME_ROOT}/include ${ONNXRUNTIME_ROOT}/include/onnxruntime
    NO_DEFAULT_PATH NO_CACHE)
find_library(ONNXRUNTIME_LIBRARY onnxruntime
    PATHS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/lib64
    NO_DEFAULT_PATH NO_CACHE)

if(NOT ONNXRUNTIME_INCLUDE_DIR OR NOT ONNXRUNTIME_LIBRARY)
    message(FATAL_ERROR
        "ONNX Runtime C++ was not found under ${ONNXRUNTIME_ROOT}")
endif()

add_library(avc_rvc MODULE
    src/FeatureIndex.cpp
    src/ModelManifest.cpp
    src/RmvpeFrontend.cpp
    src/RvcNode.cpp)
target_include_directories(avc_rvc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include)
target_include_directories(avc_rvc SYSTEM PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
target_link_libraries(avc_rvc PRIVATE ${ONNXRUNTIME_LIBRARY} nlohmann_json::nlohmann_json
    Threads::Threads)
target_compile_options(avc_rvc PRIVATE ${AVC_WARNINGS})
target_compile_options(avc_rvc PRIVATE -fvisibility=hidden)
target_compile_definitions(avc_rvc PRIVATE AVC_RVC_WITH_ONNX_RUNTIME=1)
set_target_properties(avc_rvc PROPERTIES PREFIX "" OUTPUT_NAME "avc-rvc")

if(AVC_RVC_ENABLE_CUDA)
    target_compile_definitions(avc_rvc PRIVATE AVC_RVC_ENABLE_CUDA=1)
endif()

set_target_properties(avc_rvc PROPERTIES
    BUILD_RPATH "$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/lib")

install(TARGETS avc_rvc
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/avc/extensions
    COMPONENT rvc)
if(AVC_RVC_BUNDLE_ONNXRUNTIME AND ONNXRUNTIME_ROOT)
    file(GLOB ONNXRUNTIME_SHARED_LIBS
        "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so*"
        "${ONNXRUNTIME_ROOT}/lib/libonnxruntime_providers_*.so")
    if(ONNXRUNTIME_SHARED_LIBS)
        install(FILES ${ONNXRUNTIME_SHARED_LIBS}
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/avc/extensions/lib
            COMPONENT rvc)
    else()
        message(WARNING "No ONNX Runtime shared libraries found to bundle under ONNXRUNTIME_ROOT")
    endif()
endif()

if(AVC_RVC_BUNDLE_CONVERTER)
    include(${CMAKE_SOURCE_DIR}/cmake/RvcConverter.cmake)
endif()
