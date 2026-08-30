# Toolchains without GNU .incbin (notably MSVC) need byte arrays in C++.

set(definitions "")
set(table "")
set(count 0)

if(EXISTS "${UI_DIR}/index.html")
    file(GLOB_RECURSE assets RELATIVE "${UI_DIR}" "${UI_DIR}/*")
    foreach(asset IN LISTS assets)
        file(READ "${UI_DIR}/${asset}" bytes HEX)
        string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
        string(APPEND definitions
            "static const unsigned char avc_ui_${count}[] = {${bytes}0x00};\n")
        string(APPEND table
            "    { \"${asset}\", { reinterpret_cast<const char *>(avc_ui_${count}), sizeof(avc_ui_${count}) - 1 } },\n")
        math(EXPR count "${count} + 1")
    endforeach()
endif()

file(WRITE "${OUT_CPP}"
"#include \"control/UiAssets.hpp\"

#include <array>

namespace avc::control::ui {
namespace {

${definitions}
const std::array<Asset, ${count} + 1> kAssets{{
${table}    { {}, {} },
}};

}

const Asset *find(std::string_view path)
{
    for (unsigned i = 0; i < ${count}; ++i) {
        if (kAssets[i].path == path) return &kAssets[i];
    }
    return nullptr;
}

bool empty() { return ${count} == 0; }

}
")

message(STATUS "avc: embedded ${count} editor asset(s) as portable C++")
