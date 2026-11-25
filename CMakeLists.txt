cmake_minimum_required(VERSION 3.22)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version")
endif()
project(CartaStudio VERSION 0.6.2 LANGUAGES C CXX)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG f72bad64d29715216226685810c5196bd0d79d77
    GIT_SHALLOW FALSE)
FetchContent_MakeAvailable(JUCE)

# GCC 15 on Linux ARM64 has produced a startup crash in JUCE's Unicode LRU
# cache at -O3 (Ubuntu 26.04). Limit the workaround to this JUCE translation
# unit and toolchain; audio processing and other source files keep their flags.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set_source_files_properties(${juce_SOURCE_DIR}/modules/juce_graphics/juce_graphics.cpp PROPERTIES COMPILE_OPTIONS -O1)
endif()

if(APPLE)
    enable_language(OBJCXX)
endif()

find_program(CARTA_NODE_EXECUTABLE NAMES node HINTS /opt/local/bin /opt/homebrew/bin /usr/local/bin REQUIRED)
find_path(CARTA_LIBUSB_INCLUDE_DIR NAMES libusb.h PATH_SUFFIXES libusb-1.0 HINTS /opt/local/include /opt/homebrew/include /usr/local/include /usr/include REQUIRED)
find_library(CARTA_LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 HINTS /opt/local/lib /opt/homebrew/lib /usr/local/lib /usr/lib REQUIRED)
get_filename_component(CARTA_LIBUSB_LIBDIR "${CARTA_LIBUSB_LIBRARY}" DIRECTORY)
get_filename_component(CARTA_LIBUSB_REALPATH "${CARTA_LIBUSB_LIBRARY}" REALPATH)
get_filename_component(CARTA_LIBUSB_SONAME "${CARTA_LIBUSB_REALPATH}" NAME)
if(WIN32)
    set(CARTA_LIBUSB_DLL "" CACHE FILEPATH "Optional path to libusb-1.0.dll to bundle next to carta-netmd.exe")
endif()

set(CARTA_PORFFOR_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/porffor/lib")
if(APPLE)
    set(CARTA_PORFFOR_CARTA1_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta1_porffor.dylib")
    set(CARTA_PORFFOR_CARTA3_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta3_porffor.dylib")
    set(CARTA_PORFFOR_CARTA5_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta5_porffor.dylib")
    set(CARTA_PORFFOR_NETMD_LINK "${CARTA_PORFFOR_LIB_DIR}/libnetmd_porffor.dylib")
    set(CARTA_PORFFOR_HIMD_LINK "${CARTA_PORFFOR_LIB_DIR}/libhimd_porffor.dylib")
    set(CARTA_PORFFOR_CODEC_RUNTIME_LIBS
        "${CARTA_PORFFOR_CARTA1_LINK}" "${CARTA_PORFFOR_CARTA3_LINK}" "${CARTA_PORFFOR_CARTA5_LINK}")
elseif(WIN32)
    set(CARTA_PORFFOR_CARTA1_LINK "${CARTA_PORFFOR_LIB_DIR}/carta1_porffor.lib")
    set(CARTA_PORFFOR_CARTA3_LINK "${CARTA_PORFFOR_LIB_DIR}/carta3_porffor.lib")
    set(CARTA_PORFFOR_CARTA5_LINK "${CARTA_PORFFOR_LIB_DIR}/carta5_porffor.lib")
    set(CARTA_PORFFOR_NETMD_LINK "${CARTA_PORFFOR_LIB_DIR}/netmd_porffor.lib")
    set(CARTA_PORFFOR_HIMD_LINK "${CARTA_PORFFOR_LIB_DIR}/himd_porffor.lib")
    set(CARTA_PORFFOR_CODEC_RUNTIME_LIBS
        "${CARTA_PORFFOR_LIB_DIR}/carta1_porffor.dll"
        "${CARTA_PORFFOR_LIB_DIR}/carta3_porffor.dll"
        "${CARTA_PORFFOR_LIB_DIR}/carta5_porffor.dll")
else()
    set(CARTA_PORFFOR_CARTA1_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta1_porffor.so")
    set(CARTA_PORFFOR_CARTA3_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta3_porffor.so")
    set(CARTA_PORFFOR_CARTA5_LINK "${CARTA_PORFFOR_LIB_DIR}/libcarta5_porffor.so")
    set(CARTA_PORFFOR_NETMD_LINK "${CARTA_PORFFOR_LIB_DIR}/libnetmd_porffor.so")
    set(CARTA_PORFFOR_HIMD_LINK "${CARTA_PORFFOR_LIB_DIR}/libhimd_porffor.so")
    set(CARTA_PORFFOR_CODEC_RUNTIME_LIBS
        "${CARTA_PORFFOR_CARTA1_LINK}" "${CARTA_PORFFOR_CARTA3_LINK}" "${CARTA_PORFFOR_CARTA5_LINK}")
endif()

add_custom_target(CartaPorfforCodecs
    COMMAND ${CARTA_NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Tools/build_porffor_codecs.mjs"
    BYPRODUCTS
        "${CARTA_PORFFOR_CARTA1_LINK}"
        "${CARTA_PORFFOR_CARTA3_LINK}"
        "${CARTA_PORFFOR_CARTA5_LINK}"
    COMMENT "Building isolated Porffor ATRAC codecs (carta1, carta3, carta5)"
    VERBATIM)
add_custom_target(CartaPorfforNetMd
    COMMAND ${CARTA_NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Tools/build_porffor_netmd.mjs"
    BYPRODUCTS "${CARTA_PORFFOR_NETMD_LINK}"
    COMMENT "Building isolated Porffor NetMD + exploits library"
    VERBATIM)
add_custom_target(CartaPorfforHimd
    COMMAND ${CARTA_NODE_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Tools/build_porffor_himd.mjs"
    BYPRODUCTS "${CARTA_PORFFOR_HIMD_LINK}"
    COMMENT "Building isolated Porffor Hi-MD + FAT16 library"
    VERBATIM)

function(carta_link_porffor_codecs target)
    target_link_libraries(${target} PRIVATE
        "${CARTA_PORFFOR_CARTA1_LINK}"
        "${CARTA_PORFFOR_CARTA3_LINK}"
        "${CARTA_PORFFOR_CARTA5_LINK}")
    if(APPLE)
        target_link_libraries(${target} PRIVATE iconv)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR}")
    elseif(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CARTA_PORFFOR_CODEC_RUNTIME_LIBS}
                "$<TARGET_FILE_DIR:${target}>/"
            VERBATIM)
    else()
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};$ORIGIN;$ORIGIN/../lib"
            INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
    endif()
endfunction()

# Standalone GPL-2.0 NetMD USB helper CLI (links zero JUCE or Studio engine code).
add_executable(carta-netmd
    carta-netmd/main.cpp
    carta-netmd/NetMdHostBridge.cpp
)
target_include_directories(carta-netmd PRIVATE carta-netmd)
target_include_directories(carta-netmd SYSTEM PRIVATE "${CARTA_LIBUSB_INCLUDE_DIR}")
target_compile_features(carta-netmd PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(carta-netmd PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(carta-netmd CartaPorfforNetMd)
if(APPLE)
    target_link_libraries(carta-netmd PRIVATE iconv
        "${CARTA_PORFFOR_NETMD_LINK}" "${CARTA_LIBUSB_LIBRARY}"
        "-framework IOKit" "-framework CoreFoundation"
        "-Wl,-export_dynamic")
    set_target_properties(carta-netmd PROPERTIES
        BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};${CARTA_LIBUSB_LIBDIR};@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks")
elseif(WIN32)
    target_link_libraries(carta-netmd PRIVATE
        "${CARTA_PORFFOR_NETMD_LINK}" "${CARTA_LIBUSB_LIBRARY}" bcrypt)
    if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
        add_custom_command(TARGET carta-netmd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:carta-netmd>/"
            VERBATIM)
    endif()
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(carta-netmd PRIVATE
        "${CARTA_PORFFOR_NETMD_LINK}" "${CARTA_LIBUSB_LIBRARY}" Threads::Threads "-Wl,--export-dynamic")
    set_target_properties(carta-netmd PROPERTIES
        BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};${CARTA_LIBUSB_LIBDIR};$ORIGIN;$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
endif()

# Standalone GPL-2.0 Hi-MD SCSI/UMSC helper CLI (links zero JUCE or Studio engine code).
add_executable(carta-himd
    carta-himd/main.cpp
    carta-himd/HiMdHostBridge.cpp
    carta-himd/HiMdScsiTransport.cpp
)
target_include_directories(carta-himd PRIVATE carta-himd)
target_include_directories(carta-himd SYSTEM PRIVATE "${CARTA_LIBUSB_INCLUDE_DIR}")
target_compile_features(carta-himd PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(carta-himd PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_dependencies(carta-himd CartaPorfforHimd)
if(APPLE)
    target_link_libraries(carta-himd PRIVATE iconv
        "${CARTA_PORFFOR_HIMD_LINK}" "${CARTA_LIBUSB_LIBRARY}"
        "-framework IOKit" "-framework CoreFoundation" "-framework DiskArbitration"
        "-Wl,-export_dynamic")
    set_target_properties(carta-himd PROPERTIES
        BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};${CARTA_LIBUSB_LIBDIR};@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks")
elseif(WIN32)
    target_link_libraries(carta-himd PRIVATE
        "${CARTA_PORFFOR_HIMD_LINK}" "${CARTA_LIBUSB_LIBRARY}" bcrypt setupapi cfgmgr32 uuid advapi32)
    if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
        add_custom_command(TARGET carta-himd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:carta-himd>/"
            VERBATIM)
    endif()
elseif(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(carta-himd PRIVATE
        "${CARTA_PORFFOR_HIMD_LINK}" "${CARTA_LIBUSB_LIBRARY}" Threads::Threads "-Wl,--export-dynamic")
    set_target_properties(carta-himd PROPERTIES
        BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};${CARTA_LIBUSB_LIBDIR};$ORIGIN;$ORIGIN/../lib"
        INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib")
endif()

set(CARTA_ENGINE_SOURCES
    Source/Core/Session.cpp
    Source/Audio/Resampler.cpp
    Source/Audio/Transport.cpp
    Source/Audio/Spectrogram.cpp
    Source/Audio/DeviceSettings.cpp
    Source/Editing/Editing.cpp
    Source/IO/SessionIO.cpp
    Source/MiniDisc/MdAuthoring.cpp
    Source/MiniDisc/Utoc.cpp
)

set(CARTA_CODEC_SOURCES
    Source/MiniDisc/NativeCodecs.cpp
)

set(CARTA_NETMD_SOURCES
    Source/MiniDisc/NetMdBridge.cpp
)

set(CARTA_HIMD_SOURCES
    Source/MiniDisc/HiMdBridge.cpp
)

set(CARTA_STUDIO_UI_SOURCES
    Source/Platform/Ipc.cpp
    Source/MiniDisc/MdWorkspace.cpp
    Source/Studio/StudioDialogs.cpp
    Source/Studio/StudioLifecycle.cpp
    Source/Studio/StudioIpcCommands.cpp
    Source/Studio/StudioProjectIO.cpp
    Source/Studio/StudioTimelineView.cpp
    Source/Studio/StudioMouseAndDrag.cpp
    Source/Studio/StudioMenusAndKeys.cpp
    Source/Studio/StudioEditingActions.cpp
    Source/Studio/StudioMiniDiscActions.cpp
)

juce_add_gui_app(CartaStudio
    PRODUCT_NAME "Carta Studio"
    COMPANY_NAME "Carta"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppIcon.png")
target_sources(CartaStudio PRIVATE
    Source/Main.cpp
    ${CARTA_ENGINE_SOURCES}
    ${CARTA_CODEC_SOURCES}
    ${CARTA_NETMD_SOURCES}
    ${CARTA_HIMD_SOURCES}
    ${CARTA_STUDIO_UI_SOURCES}
)
target_include_directories(CartaStudio PRIVATE Source)
add_dependencies(CartaStudio CartaPorfforCodecs carta-netmd carta-himd)
carta_link_porffor_codecs(CartaStudio)
if(APPLE)
    target_sources(CartaStudio PRIVATE Source/Platform/NativeSavePrompt_mac.mm)
    target_link_libraries(CartaStudio PRIVATE "-framework AppKit")
    set_target_properties(CartaStudio PROPERTIES
        BUILD_RPATH "${CARTA_PORFFOR_LIB_DIR};@executable_path/../Frameworks"
        INSTALL_RPATH "@executable_path/../Frameworks")
    add_custom_command(TARGET CartaStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CARTA_PORFFOR_CARTA1_LINK}"
            "${CARTA_PORFFOR_CARTA3_LINK}"
            "${CARTA_PORFFOR_CARTA5_LINK}"
            "${CARTA_PORFFOR_NETMD_LINK}"
            "${CARTA_PORFFOR_HIMD_LINK}"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CARTA_LIBUSB_REALPATH}"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks/${CARTA_LIBUSB_SONAME}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:carta-netmd>"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-netmd"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:carta-himd>"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-himd"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Resources/THIRD_PARTY_NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-netmd/LICENSE"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Resources/LICENSE-carta-netmd-GPL-2.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-himd/LICENSE"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Resources/LICENSE-carta-himd-GPL-2.0.txt"
        COMMAND chmod u+w "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks/${CARTA_LIBUSB_SONAME}"
        COMMAND install_name_tool -id "@rpath/${CARTA_LIBUSB_SONAME}"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks/${CARTA_LIBUSB_SONAME}"
        COMMAND install_name_tool -change "${CARTA_LIBUSB_LIBDIR}/${CARTA_LIBUSB_SONAME}"
            "@executable_path/../Frameworks/${CARTA_LIBUSB_SONAME}"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-netmd"
        COMMAND install_name_tool -change "${CARTA_LIBUSB_LIBDIR}/${CARTA_LIBUSB_SONAME}"
            "@executable_path/../Frameworks/${CARTA_LIBUSB_SONAME}"
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-himd"
        COMMAND codesign --force --sign -
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Frameworks/${CARTA_LIBUSB_SONAME}"
        COMMAND codesign --force --sign -
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-netmd"
        COMMAND codesign --force --sign -
            "$<TARGET_BUNDLE_DIR:CartaStudio>/Contents/Helpers/carta-himd"
        VERBATIM)
elseif(WIN32)
    target_link_libraries(CartaStudio PRIVATE shell32)
    target_sources(CartaStudio PRIVATE Source/Platform/NativePopupMenu_win.cpp)
    add_custom_command(TARGET CartaStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:carta-netmd>"
            "$<TARGET_FILE:carta-himd>"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:CartaStudio>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-netmd/LICENSE"
            "$<TARGET_FILE_DIR:CartaStudio>/LICENSE-carta-netmd-GPL-2.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-himd/LICENSE"
            "$<TARGET_FILE_DIR:CartaStudio>/LICENSE-carta-himd-GPL-2.0.txt"
        VERBATIM)
    if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
        add_custom_command(TARGET CartaStudio POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:CartaStudio>/"
            VERBATIM)
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(CARTA_STUDIO_GTK_PROMPTS "Use GTK for native Linux text prompts when available" ON)
    if(CARTA_STUDIO_GTK_PROMPTS)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(CARTA_GTK3 QUIET IMPORTED_TARGET gtk+-x11-3.0)
        endif()
    endif()
    if(CARTA_STUDIO_GTK_PROMPTS AND CARTA_GTK3_FOUND)
        target_sources(CartaStudio PRIVATE Source/Platform/NativeTextPrompt_linux.cpp)
        target_compile_definitions(CartaStudio PRIVATE CARTA_STUDIO_GTK_PROMPTS=1)
        target_link_libraries(CartaStudio PRIVATE PkgConfig::CARTA_GTK3)
    endif()
    add_custom_command(TARGET CartaStudio POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:carta-netmd>"
            "$<TARGET_FILE:carta-himd>"
            ${CARTA_PORFFOR_CODEC_RUNTIME_LIBS}
            "${CARTA_PORFFOR_NETMD_LINK}"
            "${CARTA_PORFFOR_HIMD_LINK}"
            "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:CartaStudio>/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-netmd/LICENSE"
            "$<TARGET_FILE_DIR:CartaStudio>/LICENSE-carta-netmd-GPL-2.0.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/carta-himd/LICENSE"
            "$<TARGET_FILE_DIR:CartaStudio>/LICENSE-carta-himd-GPL-2.0.txt"
        VERBATIM)
endif()
target_compile_features(CartaStudio PRIVATE cxx_std_17)
target_compile_definitions(CartaStudio PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(CartaStudio PRIVATE juce::juce_audio_utils juce::juce_gui_extra juce::juce_dsp juce::juce_cryptography
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

include(CTest)
option(CARTA_STUDIO_COMPONENT_TESTS "Build timeline and dialog interaction tests (requires a GUI session or Xvfb)" OFF)
if(BUILD_TESTING)
    add_executable(CartaNativeAlertPolicyTests Tests/NativeAlertPolicyTests.cpp)
    target_include_directories(CartaNativeAlertPolicyTests PRIVATE Source)
    target_compile_features(CartaNativeAlertPolicyTests PRIVATE cxx_std_17)
    add_test(NAME carta-native-alert-policy COMMAND CartaNativeAlertPolicyTests)

    juce_add_console_app(CartaMdAuthoringTests PRODUCT_NAME "Carta MD Authoring Tests")
    target_sources(CartaMdAuthoringTests PRIVATE Tests/MdAuthoringTests.cpp ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaMdAuthoringTests PRIVATE Source)
    target_compile_features(CartaMdAuthoringTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaMdAuthoringTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(CartaMdAuthoringTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(APPLE)
        target_link_libraries(CartaMdAuthoringTests PRIVATE iconv)
    endif()
    add_test(NAME carta-md-authoring COMMAND CartaMdAuthoringTests)

    juce_add_console_app(CartaUtocTests PRODUCT_NAME "Carta UTOC Tests")
    target_sources(CartaUtocTests PRIVATE Tests/UtocTests.cpp ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaUtocTests PRIVATE Source)
    target_compile_features(CartaUtocTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaUtocTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(CartaUtocTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(APPLE)
        target_link_libraries(CartaUtocTests PRIVATE iconv)
    endif()
    add_test(NAME carta-utoc COMMAND CartaUtocTests)

    juce_add_console_app(CartaSessionTests PRODUCT_NAME "Carta Session Tests")
    target_sources(CartaSessionTests PRIVATE
        Tests/SessionTests.cpp Tests/WaveformBenchmark.cpp Tests/RenderBenchmark.cpp
        Tests/HistoryBenchmark.cpp Tests/LongImportCheck.cpp Tests/WriteLimitTest.cpp
        Tests/MappedCrashTest.cpp Tests/ExportCrashTest.cpp Tests/SoftwarePaintProbe.cpp
        ${CARTA_ENGINE_SOURCES}
    )
    target_include_directories(CartaSessionTests PRIVATE Source)
    target_compile_features(CartaSessionTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaSessionTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(CartaSessionTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(APPLE)
        target_link_libraries(CartaSessionTests PRIVATE iconv)
    endif()
    add_test(NAME carta-session COMMAND CartaSessionTests)
    if(UNIX)
        add_test(NAME carta-write-limit COMMAND CartaSessionTests --write-limit-test)
        add_test(NAME carta-mapped-crash COMMAND CartaSessionTests --mapped-crash-test)
        add_test(NAME carta-export-crash COMMAND CartaSessionTests --export-crash-test)
        add_test(NAME carta-project-crash COMMAND CartaSessionTests --project-crash-test)
    endif()

    if(CARTA_STUDIO_COMPONENT_TESTS)
        if(CARTA_STUDIO_GTK_PROMPTS AND CARTA_GTK3_FOUND)
            find_package(X11 REQUIRED)
            juce_add_console_app(CartaGtkPromptTests PRODUCT_NAME "Carta GTK Prompt Tests")
            target_sources(CartaGtkPromptTests PRIVATE Tests/GtkPromptTests.cpp Source/Platform/NativeTextPrompt_linux.cpp)
            target_include_directories(CartaGtkPromptTests PRIVATE Source)
            target_compile_features(CartaGtkPromptTests PRIVATE cxx_std_17)
            target_compile_definitions(CartaGtkPromptTests PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
            target_link_libraries(CartaGtkPromptTests PRIVATE juce::juce_gui_basics PkgConfig::CARTA_GTK3 X11::X11)
            add_test(NAME carta-gtk-prompts COMMAND CartaGtkPromptTests)
            find_program(CARTA_XDOTOOL xdotool)
            find_package(Python3 COMPONENTS Interpreter QUIET)
            if(CARTA_XDOTOOL AND Python3_Interpreter_FOUND)
                add_test(NAME carta-gtk-prompt-keyboard COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/gtk_prompt_keyboard_test.py $<TARGET_FILE:CartaGtkPromptTests>)
                add_test(NAME carta-gtk-alert-integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Tests/gtk_alert_integration_test.py $<TARGET_FILE:CartaStudio>)
            endif()
        endif()

        # Component tests compile Studio UI sources directly with CARTA_STUDIO_COMPONENT_TEST=1
        # rather than sharing object files with CartaStudio, because test hooks change class layout.
        juce_add_console_app(CartaComponentTests PRODUCT_NAME "Carta Component Tests")
        target_sources(CartaComponentTests PRIVATE
            Tests/ComponentTests.cpp
            ${CARTA_ENGINE_SOURCES}
            ${CARTA_CODEC_SOURCES}
            ${CARTA_NETMD_SOURCES}
            ${CARTA_HIMD_SOURCES}
            ${CARTA_STUDIO_UI_SOURCES}
        )
        target_include_directories(CartaComponentTests PRIVATE Source)
        add_dependencies(CartaComponentTests CartaPorfforCodecs carta-netmd carta-himd)
        target_compile_features(CartaComponentTests PRIVATE cxx_std_17)
        target_compile_definitions(CartaComponentTests PRIVATE CARTA_STUDIO_COMPONENT_TEST=1 JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(CartaComponentTests PRIVATE juce::juce_audio_utils juce::juce_gui_extra juce::juce_dsp juce::juce_cryptography PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
        carta_link_porffor_codecs(CartaComponentTests)
        if(WIN32)
            target_link_libraries(CartaComponentTests PRIVATE shell32)
            add_custom_command(TARGET CartaComponentTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:carta-netmd>" "$<TARGET_FILE:carta-himd>" "$<TARGET_FILE_DIR:CartaComponentTests>/"
                VERBATIM)
        endif()
        add_test(NAME carta-component COMMAND CartaComponentTests)
        add_test(NAME carta-clip-copy-drag COMMAND CartaComponentTests --clip-copy-drag)
        add_test(NAME carta-guarded-alerts COMMAND CartaComponentTests --guarded-alerts)
        add_test(NAME carta-md-panel-layout COMMAND CartaComponentTests --md-panel-layout)
        add_test(NAME carta-md-authoring-workflow COMMAND CartaComponentTests --md-authoring-workflow)
    endif()

    juce_add_console_app(CartaPorfforCodecTests PRODUCT_NAME "Carta Porffor Codec Tests")
    target_sources(CartaPorfforCodecTests PRIVATE Tests/PorfforCodecTests.cpp ${CARTA_CODEC_SOURCES} ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaPorfforCodecTests PRIVATE Source)
    add_dependencies(CartaPorfforCodecTests CartaPorfforCodecs)
    target_compile_features(CartaPorfforCodecTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaPorfforCodecTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 CARTA_STUDIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(CartaPorfforCodecTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    carta_link_porffor_codecs(CartaPorfforCodecTests)
    add_test(NAME carta-porffor-codecs COMMAND CartaPorfforCodecTests)

    juce_add_console_app(CartaNetMdTests PRODUCT_NAME "Carta NetMD Tests")
    target_sources(CartaNetMdTests PRIVATE Tests/NetMdTests.cpp ${CARTA_NETMD_SOURCES} ${CARTA_CODEC_SOURCES} ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaNetMdTests PRIVATE Source)
    add_dependencies(CartaNetMdTests CartaPorfforCodecs carta-netmd)
    target_compile_features(CartaNetMdTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaNetMdTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(CartaNetMdTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    carta_link_porffor_codecs(CartaNetMdTests)
    if(WIN32)
        add_custom_command(TARGET CartaNetMdTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:carta-netmd>" "$<TARGET_FILE_DIR:CartaNetMdTests>/"
            VERBATIM)
        if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
            add_custom_command(TARGET CartaNetMdTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:CartaNetMdTests>/"
                VERBATIM)
        endif()
    endif()
    add_test(NAME carta-netmd COMMAND CartaNetMdTests)

    juce_add_console_app(CartaHiMdTests PRODUCT_NAME "Carta Hi-MD Tests")
    target_sources(CartaHiMdTests PRIVATE Tests/HiMdTests.cpp ${CARTA_CODEC_SOURCES} ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaHiMdTests PRIVATE Source)
    add_dependencies(CartaHiMdTests CartaPorfforCodecs carta-himd)
    target_compile_features(CartaHiMdTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaHiMdTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        CARTA_STUDIO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        CARTA_HIMD_BINARY="$<TARGET_FILE:carta-himd>"
        CARTA_NODE_BINARY="${CARTA_NODE_EXECUTABLE}")
    target_link_libraries(CartaHiMdTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    carta_link_porffor_codecs(CartaHiMdTests)
    if(WIN32)
        add_custom_command(TARGET CartaHiMdTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:carta-himd>" "$<TARGET_FILE_DIR:CartaHiMdTests>/"
            VERBATIM)
        if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
            add_custom_command(TARGET CartaHiMdTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:CartaHiMdTests>/"
                VERBATIM)
        endif()
    endif()
    add_test(NAME carta-himd COMMAND CartaHiMdTests)

    juce_add_console_app(CartaHiMdBridgeTests PRODUCT_NAME "Carta Hi-MD Bridge Tests")
    target_sources(CartaHiMdBridgeTests PRIVATE Tests/HiMdBridgeTests.cpp ${CARTA_HIMD_SOURCES} ${CARTA_CODEC_SOURCES} ${CARTA_ENGINE_SOURCES})
    target_include_directories(CartaHiMdBridgeTests PRIVATE Source)
    add_dependencies(CartaHiMdBridgeTests CartaPorfforCodecs carta-himd)
    target_compile_features(CartaHiMdBridgeTests PRIVATE cxx_std_17)
    target_compile_definitions(CartaHiMdBridgeTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(CartaHiMdBridgeTests PRIVATE juce::juce_audio_formats juce::juce_dsp juce::juce_graphics juce::juce_cryptography
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    carta_link_porffor_codecs(CartaHiMdBridgeTests)
    if(WIN32)
        target_link_libraries(CartaHiMdBridgeTests PRIVATE shell32)
        add_custom_command(TARGET CartaHiMdBridgeTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:carta-himd>" "$<TARGET_FILE_DIR:CartaHiMdBridgeTests>/"
            VERBATIM)
        if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
            add_custom_command(TARGET CartaHiMdBridgeTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:CartaHiMdBridgeTests>/"
                VERBATIM)
        endif()
    endif()
    add_test(NAME carta-himd-bridge COMMAND CartaHiMdBridgeTests)

    # Pure C++ unit tests for OS SCSI pass-through & USB BOT transport (zero JUCE / zero Porffor).
    add_executable(CartaHiMdTransportTests
        Tests/HiMdTransportTests.cpp
        carta-himd/HiMdScsiTransport.cpp
    )
    target_include_directories(CartaHiMdTransportTests PRIVATE carta-himd)
    target_include_directories(CartaHiMdTransportTests SYSTEM PRIVATE "${CARTA_LIBUSB_INCLUDE_DIR}")
    target_compile_features(CartaHiMdTransportTests PRIVATE cxx_std_17)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
        target_compile_options(CartaHiMdTransportTests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(APPLE)
        target_link_libraries(CartaHiMdTransportTests PRIVATE
            "${CARTA_LIBUSB_LIBRARY}"
            "-framework IOKit" "-framework CoreFoundation" "-framework DiskArbitration")
        set_target_properties(CartaHiMdTransportTests PROPERTIES BUILD_RPATH "${CARTA_LIBUSB_LIBDIR}")
    elseif(WIN32)
        target_link_libraries(CartaHiMdTransportTests PRIVATE
            "${CARTA_LIBUSB_LIBRARY}" setupapi cfgmgr32 uuid)
        if(CARTA_LIBUSB_DLL AND EXISTS "${CARTA_LIBUSB_DLL}")
            add_custom_command(TARGET CartaHiMdTransportTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CARTA_LIBUSB_DLL}" "$<TARGET_FILE_DIR:CartaHiMdTransportTests>/"
                VERBATIM)
        endif()
    elseif(UNIX)
        find_package(Threads REQUIRED)
        target_link_libraries(CartaHiMdTransportTests PRIVATE "${CARTA_LIBUSB_LIBRARY}" Threads::Threads)
        set_target_properties(CartaHiMdTransportTests PROPERTIES BUILD_RPATH "${CARTA_LIBUSB_LIBDIR}")
    endif()
    add_test(NAME carta-himd-transport COMMAND CartaHiMdTransportTests)
endif()

# The streaming reader adds gapless MPEG-1 MP3 import on Linux. The native
# CoreAudio reader remains the default on macOS; the option can be overridden.
set(CARTA_STUDIO_MINIMP3_DEFAULT OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CARTA_STUDIO_MINIMP3_DEFAULT ON)
endif()
option(CARTA_STUDIO_MINIMP3 "Build the streaming minimp3 reader" ${CARTA_STUDIO_MINIMP3_DEFAULT})
if(CARTA_STUDIO_MINIMP3)
    FetchContent_Declare(minimp3 GIT_REPOSITORY https://github.com/lieff/minimp3.git GIT_TAG ea99364f61c14656440e8d77e9c233ccf3124633)
    FetchContent_MakeAvailable(minimp3)
    foreach(target CartaStudio CartaSessionTests CartaComponentTests)
        if(TARGET ${target})
            target_sources(${target} PRIVATE Source/IO/NativeMp3Format.cpp)
            target_include_directories(${target} SYSTEM PRIVATE ${minimp3_SOURCE_DIR})
            target_compile_definitions(${target} PRIVATE CARTA_STUDIO_MINIMP3=1)
        endif()
    endforeach()
endif()
