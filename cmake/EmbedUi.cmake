# .incbin rather than a hex char array on purpose. A React bundle is around a
# megabyte; as C source that is several megabytes the compiler has to parse on
# every build, while the assembler copies the bytes straight through.

set(asm_body "    .section .rodata\n")
set(externs "")
set(table "")
set(count 0)

if(EXISTS "${UI_DIR}/index.html")
    file(GLOB_RECURSE assets RELATIVE "${UI_DIR}" "${UI_DIR}/*")
    foreach(asset IN LISTS assets)
        set(symbol "avc_ui_${count}")
        string(APPEND asm_body
            "    .globl ${symbol}_data\n"
            "    .balign 8\n"
            "${symbol}_data:\n"
            "    .incbin \"${UI_DIR}/${asset}\"\n"
            "${symbol}_end:\n"
            "    .globl ${symbol}_size\n"
            "    .balign 8\n"
            "${symbol}_size:\n"
            "    .quad ${symbol}_end - ${symbol}_data\n")
        string(APPEND externs
            "extern \"C\" const char ${symbol}_data[];\n"
            "extern \"C\" const unsigned long ${symbol}_size;\n")
        string(APPEND table
            "    { \"${asset}\", { ${symbol}_data, ${symbol}_size } },\n")
        math(EXPR count "${count} + 1")
    endforeach()
endif()

file(WRITE "${OUT_ASM}" "${asm_body}")

file(WRITE "${OUT_CPP}"
"#include \"control/UiAssets.hpp\"

#include <array>

namespace avc::control::ui {
namespace {

${externs}
// Not constexpr: the sizes are symbols the linker fills in, so the table is
// built at load time rather than at compile time.
const std::array<Asset, ${count} + 1> kAssets{{
${table}    { {}, {} },
}};

}

const Asset *find(std::string_view path)
{
    for (unsigned i = 0; i < ${count}; ++i) {
        if (kAssets[i].path == path) {
            return &kAssets[i];
        }
    }
    return nullptr;
}

bool empty()
{
    return ${count} == 0;
}

}
")

message(STATUS "avc: embedded ${count} editor asset(s)")
