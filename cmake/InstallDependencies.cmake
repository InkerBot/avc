# Preserve runtime DLLs when a caller chooses a dynamic vcpkg triplet.
# The Windows presets use static libraries with the dynamic MSVC CRT.
if(WIN32)
    install(FILES $<TARGET_RUNTIME_DLLS:avc>
        DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(TARGET avc_qwen_livetranslate)
        install(FILES $<TARGET_RUNTIME_DLLS:avc_qwen_livetranslate>
            DESTINATION ${CMAKE_INSTALL_BINDIR}/extensions
            COMPONENT qwen-livetranslate)
    endif()
endif()

# vcpkg collects the upstream license and notices into each port's copyright.
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    function(avc_install_vcpkg_license port component)
        set(license "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${port}/copyright")
        if(EXISTS "${license}")
            if(component STREQUAL "rvc")
                # The standalone RVC installer collects this existing doc tree.
                set(destination "${CMAKE_INSTALL_DOCDIR}/third-party/${port}")
            else()
                set(destination "${CMAKE_INSTALL_DATADIR}/avc/licenses")
            endif()
            install(FILES "${license}"
                DESTINATION "${destination}"
                RENAME "${port}-LICENSE.txt"
                COMPONENT "${component}")
        endif()
    endfunction()

    foreach(port cpp-httplib fmt nlohmann-json spdlog)
        avc_install_vcpkg_license(${port} Unspecified)
    endforeach()
    if(WIN32)
        foreach(port miniaudio webview2 wil)
            avc_install_vcpkg_license(${port} Unspecified)
        endforeach()
    endif()
    if(TARGET avc_qwen_livetranslate)
        foreach(port curl zlib openssl nlohmann-json)
            avc_install_vcpkg_license(${port} qwen-livetranslate)
        endforeach()
    endif()
    if(TARGET avc_rvc)
        avc_install_vcpkg_license(nlohmann-json rvc)
    endif()
endif()
