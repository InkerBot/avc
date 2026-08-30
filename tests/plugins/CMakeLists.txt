add_library(avc_test_plugin MODULE test_plugin.cpp)

target_include_directories(avc_test_plugin PRIVATE ${CMAKE_SOURCE_DIR}/include)
set_target_properties(avc_test_plugin PROPERTIES
    PREFIX ""
    OUTPUT_NAME "avc_test_plugin"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-extensions
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-extensions)
target_compile_options(avc_test_plugin PRIVATE ${AVC_WARNINGS})
if(NOT MSVC)
    target_compile_options(avc_test_plugin PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden)
    target_link_options(avc_test_plugin PRIVATE -Wl,--no-undefined)
endif()
