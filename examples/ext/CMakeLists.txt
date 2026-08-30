add_library(avc_example_ext MODULE example.cpp)

target_include_directories(avc_example_ext PRIVATE ${CMAKE_SOURCE_DIR}/include)

set_target_properties(avc_example_ext PROPERTIES
    PREFIX ""
    OUTPUT_NAME "avc_example"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/extensions
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/extensions)

target_compile_options(avc_example_ext PRIVATE ${AVC_WARNINGS})
if(NOT MSVC)
    target_compile_options(avc_example_ext PRIVATE
        -fvisibility=hidden -fvisibility-inlines-hidden)
endif()

# Nothing in an extension should be resolved out of the host by accident: a
# symbol the engine happens to export today is not part of the ABI.
if(NOT MSVC)
    target_link_options(avc_example_ext PRIVATE -Wl,--no-undefined)
endif()
