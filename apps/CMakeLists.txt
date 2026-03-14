
######################
#  Executable target #
######################

set(APP_FILES main.cpp)

if(APPLE)
    add_executable(sushi MACOSX_BUNDLE ${APP_FILES})
else()
    add_executable(sushi ${APP_FILES})
endif()

####################################
#  Compiler Flags and definitions  #
####################################

if(MSVC)
    # C4996: Deprecated warning. This was flooding the terminal,
    #        I might remove the suppression from here eventually,
    #        and out into the code again.
    set(SUSHI_COMPILE_OPTIONS /W4 /fp:fast /wd4996)
    set(APP_LINK_LIBRARIES ${APP_LINK_LIBRARIES} winmm.lib) # oscpack GetCurrentTimeMs
else()
    set(SUSHI_COMPILE_OPTIONS -Wall -Wextra -Wno-psabi -fPIC -fno-rtti -ffast-math)
endif()

target_compile_options(sushi PRIVATE ${SUSHI_COMPILE_OPTIONS})

target_link_libraries(sushi PRIVATE sushi_library ${APP_LINK_LIBRARIES})

####################
#  Install         #
####################

set(DOC_FILES_INSTALL
        "${PROJECT_SOURCE_DIR}/LICENSE.md"
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/HISTORY.md"
        )

if (APPLE)
    # TODO: fix to work when not submodule
    install(CODE [[
    include(BundleUtilities)
    fixup_bundle("${CMAKE_INSTALL_PREFIX}/sushi/sushi.app" "" "")
    ]] COMPONENT Runtime)
endif()

### Custom command to copy the dynamic dependencies to the binary folder
#   Mainly for twine.dll because windows cannot find it from ../twine but
#   needs it in the same directory
if (MSVC)
    add_custom_command(TARGET sushi POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:sushi> $<TARGET_FILE_DIR:sushi>
            COMMAND_EXPAND_LISTS)
endif()

install(TARGETS sushi RUNTIME DESTINATION bin BUNDLE DESTINATION Applications)

############################
#  sushi-gui (Cocoa) target #
############################

if(APPLE AND (SUSHI_WITH_VST3 OR SUSHI_WITH_CLAP OR SUSHI_WITH_AUV2 OR SUSHI_WITH_CMAJOR))
    # SwiftUI node graph viewer requires Ninja or Xcode generator (Make doesn't support Swift)
    set(SUSHI_GUI_SWIFT_SUPPORTED FALSE)
    if(CMAKE_GENERATOR MATCHES "Ninja|Xcode")
        enable_language(Swift)
        set(SUSHI_GUI_SWIFT_SUPPORTED TRUE)
    endif()

    set(SUSHI_GUI_SOURCES main_cocoa.mm sushi_status_bar.mm)

    if(SUSHI_GUI_SWIFT_SUPPORTED)
        # Swift static library (CMake cannot mix Swift + ObjC++ in one target)
        add_library(sushi-gui-swift STATIC
            NodeGraphModel.swift
            NodeGraphView.swift
            NodeGraphWindowController.swift
            VisualGraphView.swift
        )
        set_target_properties(sushi-gui-swift PROPERTIES
            Swift_MODULE_NAME SushiGUISwift
        )
        if(NOT CMAKE_GENERATOR MATCHES "Xcode")
            target_compile_options(sushi-gui-swift PRIVATE
                "SHELL:-import-objc-header ${CMAKE_CURRENT_SOURCE_DIR}/sushi-gui-Bridging-Header.h"
            )
        else()
            set_target_properties(sushi-gui-swift PROPERTIES
                XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
                "${CMAKE_CURRENT_SOURCE_DIR}/sushi-gui-Bridging-Header.h"
            )
        endif()

        list(APPEND SUSHI_GUI_SOURCES node_graph_bridge.mm)
    endif()

    add_executable(sushi-gui MACOSX_BUNDLE ${SUSHI_GUI_SOURCES})
    target_compile_options(sushi-gui PRIVATE ${SUSHI_COMPILE_OPTIONS} -fobjc-arc)

    if(SUSHI_GUI_SWIFT_SUPPORTED)
        set_target_properties(sushi-gui PROPERTIES LINKER_LANGUAGE CXX)
        target_compile_definitions(sushi-gui PRIVATE SUSHI_BUILD_WITH_NODE_GRAPH_VIEW=1)
        target_include_directories(sushi-gui PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

        # Find the Swift runtime library path for linking Swift static lib from C++ linker
        execute_process(
            COMMAND xcrun --toolchain default --find swiftc
            OUTPUT_VARIABLE SWIFTC_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        get_filename_component(SWIFT_BIN_DIR "${SWIFTC_PATH}" DIRECTORY)
        set(SWIFT_LIB_DIR "${SWIFT_BIN_DIR}/../lib/swift/macosx")
        cmake_path(NORMAL_PATH SWIFT_LIB_DIR)

        target_link_libraries(sushi-gui PRIVATE
            sushi_library
            sushi-gui-swift
            ${APP_LINK_LIBRARIES}
            "-framework Cocoa"
            "-framework SwiftUI"
            "-L${SWIFT_LIB_DIR}"
            "-lswiftCore"
        )
        target_link_options(sushi-gui PRIVATE
            "-Wl,-rpath,${SWIFT_LIB_DIR}"
            "-L/usr/lib/swift"
        )
    else()
        target_link_libraries(sushi-gui PRIVATE sushi_library ${APP_LINK_LIBRARIES} "-framework Cocoa")
    endif()

    set_target_properties(sushi-gui PROPERTIES
            MACOSX_BUNDLE_BUNDLE_NAME "Sushi GUI"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.elkaudio.sushi.gui"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_INFO_STRING "Sushi GUI ${PROJECT_VERSION}"
            MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
    )
    install(TARGETS sushi-gui BUNDLE DESTINATION Applications)
endif()
foreach(ITEM ${DOC_FILES_INSTALL})
    install(FILES ${ITEM} DESTINATION share/sushi/doc)
endforeach()
