cmake_minimum_required(VERSION 3.16)
project(artemis_compat C CXX)

# Preserve standalone defaults without creating windows/tools in consumers.
set(_artc_standalone OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(_artc_standalone ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    endif()
endif()
option(ARTC_BUILD_CLI "Build host CLI test tool (artc)" ${_artc_standalone})
option(ARTC_BUILD_JNI "Build Android JNI layer (libartemis.so)" OFF)
option(ARTC_BUILD_TESTS "Build the host regression suite" OFF)
option(ARTC_TEST_GLES "Run compositor regressions using EGL/GLES" OFF)
set(_artc_mac_default OFF)
if(_artc_standalone AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_artc_mac_default ON)
endif()
option(ARTC_BUILD_MAC_HOST "Build the native macOS GL host" ${_artc_mac_default})
if(ARTC_BUILD_MAC_HOST AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR "The Cocoa macOS host requires a macOS toolchain")
endif()
include(cmake/ArtemisBackends.cmake)

# ---- vendored Lua 5.1.5 (static, no standalone interpreters) ----
file(GLOB LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.5/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/lua\\.c$|/luac\\.c$")
add_library(lua51 STATIC ${LUA_SOURCES})
set_target_properties(lua51 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(lua51 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.5/src)
if(UNIX)
    target_compile_definitions(lua51 PRIVATE LUA_USE_POSIX)
endif()
# NDK r28 defaults to _FORTIFY_SOURCE=3, which false-positives inside Lua 5.1's
# GC sweep (strchr FORTIFY abort in luaC_step during luaL_openlibs). Lua 5.1
# predates FORTIFY hardening; disable it for this vendored build on Android.
if(ANDROID OR OHOS)
    target_compile_options(lua51 PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
endif()

# ---- shared dependencies (lua, zlib, optional FFmpeg) ----
find_package(ZLIB REQUIRED)
add_library(artemis_deps INTERFACE)
target_include_directories(artemis_deps INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(artemis_deps INTERFACE lua51 ZLIB::ZLIB)

# Movie playback uses FFmpeg when the host provides it; the video decoder
# compiles to a stub otherwise.
option(ARTC_ENABLE_FFMPEG "Decode Artemis movies with FFmpeg" ON)
if(ARTC_ENABLE_FFMPEG)
    if(FFMPEG_FOUND)
        target_include_directories(artemis_deps INTERFACE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(artemis_deps INTERFACE ${FFMPEG_LIBRARIES})
        target_compile_definitions(artemis_deps INTERFACE ARTC_HAS_FFMPEG=1)
    elseif(NOT CMAKE_CROSSCOMPILING)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(ARTC_FFMPEG QUIET IMPORTED_TARGET
                libavformat libavcodec libavutil libswresample libswscale)
            if(ARTC_FFMPEG_FOUND)
                target_link_libraries(artemis_deps INTERFACE PkgConfig::ARTC_FFMPEG)
                target_compile_definitions(artemis_deps INTERFACE ARTC_HAS_FFMPEG=1)
            endif()
        endif()
    endif()
endif()

# ---- core engine (portable: pack/config/script/log/render/audio) ----
file(GLOB_RECURSE ARTC_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pack/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/script/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/save/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/input/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util/*.cpp)
# Compile exactly one Audio::Impl. Other backends remain available to hosts.
if(_artc_audio STREQUAL "OHAUDIO")
    list(REMOVE_ITEM ARTC_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/audio.cpp")
    list(APPEND ARTC_CORE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/ohos/audio_ohos.cpp")
endif()
add_library(artemis_core STATIC ${ARTC_CORE_SOURCES})
add_library(artemis::core ALIAS artemis_core)
set_target_properties(artemis_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(artemis_core PUBLIC cxx_std_17)
target_compile_options(artemis_core PRIVATE -Wall -Wextra)
target_include_directories(artemis_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(artemis_core PUBLIC artemis_deps artemis_platform)

# ---- host CLI test tool ----
if(ARTC_BUILD_CLI)
    add_executable(artc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/artc.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/cli/host_drive.cpp)
    target_link_libraries(artc PRIVATE artemis_core)
endif()

# ---- Android JNI layer: produces libartemis.so ----
if(ARTC_BUILD_JNI)
    add_library(artemis SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jni/jni_bridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/jni/native_activity.cpp)
    target_link_libraries(artemis PRIVATE artemis::core)
    set_target_properties(artemis PROPERTIES OUTPUT_NAME artemis)
endif()

# ---- host regression suite (synthetic fixtures; see tests/) ----
if(ARTC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---- native macOS GL host (Cocoa window + legacy OpenGL context) ----
if(ARTC_BUILD_MAC_HOST)
    enable_language(OBJCXX)
    # Recompile the core with ARTC_HAS_GLES so the compositor/layer shaders use
    # the real GL path (artemis_core stays math-only for the CLI/tests).
    add_executable(artemis-mac ${ARTC_CORE_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/host/mac_main.mm)
    target_include_directories(artemis-mac PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/render
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
    target_compile_features(artemis-mac PRIVATE cxx_std_17)
    target_compile_definitions(artemis-mac PRIVATE ARTC_HAS_GLES=1 ARTC_USE_DESKTOP_GL=1)
    target_compile_options(artemis-mac PRIVATE -Wno-deprecated-declarations)
    target_link_libraries(artemis-mac PRIVATE artemis_deps
        "-framework Cocoa" "-framework OpenGL")
endif()

# ---- install rules (T3-1): downstream integration should not hand-copy so's
include(GNUInstallDirs)
install(TARGETS artemis_core lua51 ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET artemis)
    install(TARGETS artemis LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if(TARGET artc)
    install(TARGETS artc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET artemis-mac)
    install(TARGETS artemis-mac RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/artemis-compat
        FILES_MATCHING PATTERN "*.h")
