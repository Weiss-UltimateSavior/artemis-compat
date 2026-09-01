cmake_minimum_required(VERSION 3.16)
project(artemis_compat C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

option(ARTC_BUILD_CLI "Build host CLI test tool (artc)" ON)
option(ARTC_BUILD_JNI "Build Android JNI layer (libartemis.so)" OFF)

# ---- vendored Lua 5.1.5 (static, no standalone interpreters) ----
file(GLOB LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.5/src/*.c)
list(FILTER LUA_SOURCES EXCLUDE REGEX "/lua\\.c$|/luac\\.c$")
add_library(lua51 STATIC ${LUA_SOURCES})
target_include_directories(lua51 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.5/src)
if(UNIX)
    target_compile_definitions(lua51 PRIVATE LUA_USE_POSIX)
endif()
# NDK r28 defaults to _FORTIFY_SOURCE=3, which false-positives inside Lua 5.1's
# GC sweep (strchr FORTIFY abort in luaC_step during luaL_openlibs). Lua 5.1
# predates FORTIFY hardening; disable it for this vendored build on Android.
if(ANDROID)
    target_compile_options(lua51 PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
endif()

# ---- core engine (portable: pack/config/script/log) ----
file(GLOB_RECURSE ARTC_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pack/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/config/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/script/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/log/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/render/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/*.cpp)
add_library(artemis_core STATIC ${ARTC_CORE_SOURCES})
target_include_directories(artemis_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(artemis_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/render)
target_link_libraries(artemis_core PUBLIC lua51)

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
    find_library(egl-lib EGL)
    find_library(gles-lib GLESv2)
    find_library(opensl-lib OpenSLES)
    target_link_libraries(artemis PRIVATE artemis_core log android EGL GLESv2 ${opensl-lib})
endif()
