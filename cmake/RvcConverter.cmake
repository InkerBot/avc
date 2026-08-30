# Everything is pinned because this directory is installed as executable code.

set(_avc_rvc_converter_root "${CMAKE_BINARY_DIR}/rvc-converter")
set(_avc_rvc_python_release "20260718")
set(_avc_rvc_python_version "3.10.20")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _avc_rvc_converter_arch)
if(_avc_rvc_converter_arch MATCHES "^(x86_64|amd64)$")
    set(_avc_rvc_python_arch "x86_64")
    set(_avc_rvc_python_sha256 "3d71c71aad818dab1776dca94f76667d88126e06623d916a21371117d17d21e7")
    set(_avc_rvc_torch "torch==2.12.1")
    set(_avc_rvc_torch_index "https://download.pytorch.org/whl/cpu")
elseif(_avc_rvc_converter_arch MATCHES "^(aarch64|arm64)$")
    set(_avc_rvc_python_arch "aarch64")
    set(_avc_rvc_python_sha256 "7590cad6d464d9cfc7f30ec15a6ab919113fa0d265406882f8605a79d3a43bfa")
    set(_avc_rvc_torch "torch==2.12.1")
    set(_avc_rvc_torch_index "https://pypi.org/simple")
else()
    message(FATAL_ERROR "The bundled RVC converter does not support ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(_avc_rvc_python_archive
    "cpython-${_avc_rvc_python_version}+${_avc_rvc_python_release}-${_avc_rvc_python_arch}-unknown-linux-gnu-install_only_stripped.tar.gz")
FetchContent_Declare(avc_rvc_python
    URL "https://github.com/astral-sh/python-build-standalone/releases/download/${_avc_rvc_python_release}/${_avc_rvc_python_archive}"
    URL_HASH "SHA256=${_avc_rvc_python_sha256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(avc_rvc_python)

FetchContent_Declare(avc_rvc_export_source
    GIT_REPOSITORY https://github.com/RVC-Project/Retrieval-based-Voice-Conversion.git
    GIT_TAG 7b284a634667c34103eaaeed972b48ccdb4b893e
    GIT_SHALLOW FALSE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(avc_rvc_export_source)

set(_avc_rvc_base_dir "${_avc_rvc_converter_root}/base")
file(MAKE_DIRECTORY "${_avc_rvc_base_dir}")

function(avc_rvc_download_model filename url sha256)
    set(path "${_avc_rvc_base_dir}/${filename}")
    if(EXISTS "${path}")
        file(SHA256 "${path}" existing_sha256)
        if(existing_sha256 STREQUAL sha256)
            message(STATUS "avc-rvc: using cached ${filename}")
            return()
        endif()
        file(REMOVE "${path}")
    endif()
    file(DOWNLOAD "${url}" "${path}"
        EXPECTED_HASH "SHA256=${sha256}"
        TLS_VERIFY ON
        SHOW_PROGRESS
        STATUS status)
    list(GET status 0 code)
    list(GET status 1 message)
    if(NOT code EQUAL 0)
        file(REMOVE "${path}")
        message(FATAL_ERROR "Cannot download ${filename}: ${message}")
    endif()
endfunction()

avc_rvc_download_model(contentvec-v1.onnx
    "https://huggingface.co/Huxleyy/rvc_onnx_version/resolve/a0211acbeb2dfd4965cbde92113c6dc5b7948526/baseline/vec-256-layer-9.onnx"
    "61d0d0598803f74d5a4bcba65ca367571889accc0e86196f0982e8e455d56393")
avc_rvc_download_model(contentvec-v2.onnx
    "https://huggingface.co/TigreGotico/voiceclonnx-rvc/resolve/cbfbabdabe6a1414292bade8f857fb6074abf130/contentvec_768l12_q8.onnx"
    "23d4914b29779cd68af19191405423e82ec978fafe9c12227df23d3ebe04d421")
avc_rvc_download_model(rmvpe.onnx
    "https://huggingface.co/TigreGotico/voiceclonnx-rvc/resolve/cbfbabdabe6a1414292bade8f857fb6074abf130/rmvpe_q8.onnx"
    "9151c489d8c09a2c31c035e5eb24651c18e9f05cc04c4cc1afc5086c9bce7d1e")

file(MAKE_DIRECTORY "${_avc_rvc_converter_root}")
configure_file("${CMAKE_SOURCE_DIR}/tools/rvc/avc-rvc-convert.in"
    "${_avc_rvc_converter_root}/avc-rvc-convert" @ONLY)
file(CHMOD "${_avc_rvc_converter_root}/avc-rvc-convert"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

set(_avc_rvc_packages_stamp "${_avc_rvc_converter_root}/.packages-ready")
set(_avc_rvc_packages
    "${_avc_rvc_torch}"
    onnx==1.16.2
    numpy==1.26.4
    scipy==1.13.1
    faiss-cpu==1.12.0
    packaging==24.1
    filelock==3.16.1
    typing-extensions==4.12.2
    sympy==1.13.3
    networkx==3.3
    jinja2==3.1.4
    fsspec==2024.9.0
    MarkupSafe==3.0.2
    mpmath==1.3.0
    protobuf==5.28.3)
set(_avc_rvc_packages_recipe "${CMAKE_CURRENT_BINARY_DIR}/rvc-converter-packages.txt")
configure_file("${CMAKE_SOURCE_DIR}/cmake/RvcConverterPackages.in"
    "${_avc_rvc_packages_recipe}" @ONLY)
add_custom_command(
    OUTPUT "${_avc_rvc_packages_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${avc_rvc_python_SOURCE_DIR}" "${_avc_rvc_converter_root}/python"
    COMMAND "${_avc_rvc_converter_root}/python/bin/python3" -m pip install
            --disable-pip-version-check --no-cache-dir --upgrade --only-binary=:all:
            --target "${_avc_rvc_converter_root}/site-packages"
            --index-url "${_avc_rvc_torch_index}"
            --extra-index-url https://pypi.org/simple
            ${_avc_rvc_packages}
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${_avc_rvc_converter_root}/rvc/lib/infer_pack"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${avc_rvc_export_source_SOURCE_DIR}/rvc/lib/infer_pack"
            "${_avc_rvc_converter_root}/rvc/lib/infer_pack"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_avc_rvc_converter_root}/rvc/__init__.py"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_avc_rvc_converter_root}/rvc/lib/__init__.py"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_avc_rvc_packages_stamp}"
    DEPENDS "${_avc_rvc_packages_recipe}"
    COMMENT "Installing the self-contained RVC conversion runtime"
    VERBATIM)

set(_avc_rvc_converter_stamp "${_avc_rvc_converter_root}/.ready")
add_custom_command(
    OUTPUT "${_avc_rvc_converter_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tools/rvc/convert_pth.py"
            "${_avc_rvc_converter_root}/convert_pth.py"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_avc_rvc_converter_stamp}"
    DEPENDS "${_avc_rvc_packages_stamp}"
            "${CMAKE_SOURCE_DIR}/tools/rvc/convert_pth.py"
            "${CMAKE_SOURCE_DIR}/tools/rvc/avc-rvc-convert.in"
    COMMENT "Installing the RVC PTH converter entry point"
    VERBATIM)
add_custom_target(avc_rvc_converter ALL DEPENDS "${_avc_rvc_converter_stamp}")
add_dependencies(avc_rvc avc_rvc_converter)

set(_avc_rvc_install_root "${CMAKE_INSTALL_FULL_LIBEXECDIR}/avc/rvc-converter")
target_compile_definitions(avc PRIVATE
    AVC_RVC_BUILD_CONVERTER_ROOT="${_avc_rvc_converter_root}"
    AVC_RVC_BUILD_BINARY_DIR="${CMAKE_BINARY_DIR}"
    AVC_RVC_INSTALL_LIBEXECDIR="${CMAKE_INSTALL_LIBEXECDIR}"
    AVC_RVC_INSTALLED_CONVERTER_ROOT="${_avc_rvc_install_root}")
add_dependencies(avc avc_rvc_converter)

install(DIRECTORY "${_avc_rvc_converter_root}/"
    DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/avc/rvc-converter"
    COMPONENT rvc
    USE_SOURCE_PERMISSIONS
    PATTERN ".ready" EXCLUDE
    PATTERN ".packages-ready" EXCLUDE)
install(FILES "${avc_rvc_export_source_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}/third-party/rvc-converter"
    COMPONENT rvc
    RENAME RVC-LICENSE)
