# Regression suite for the compat engine. Configured from the repository root
# (the parent defines the `artemis_core` target):
#
#   cmake -B build-test -DARTC_BUILD_TESTS=ON
#   cmake --build build-test -j8
#   ctest --test-dir build-test --output-on-failure
#
# Fixtures under data/ are synthetic: an original rectangle-glyph font and two
# generated tones. No commercial game content is included.
enable_testing()
set(ARTC_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data" CACHE PATH
    "Runtime location of synthetic test fixtures")

add_executable(runtime_regressions runtime_regressions.cpp)
target_link_libraries(runtime_regressions PRIVATE artemis_core)
add_test(NAME runtime_regressions COMMAND runtime_regressions)

add_executable(emote_regressions emote_regressions.cpp)
target_link_libraries(emote_regressions PRIVATE artemis_core)
add_test(NAME emote_regressions COMMAND emote_regressions)

add_executable(audio_stream_regressions audio_stream_regressions.cpp)
target_compile_definitions(audio_stream_regressions PRIVATE
    ARTC_TEST_DATA="${ARTC_TEST_DATA_DIR}")
target_link_libraries(audio_stream_regressions PRIVATE artemis_core)
add_test(NAME audio_stream_regressions COMMAND audio_stream_regressions)

add_executable(pf8_regressions pf8_regressions.cpp)
target_link_libraries(pf8_regressions PRIVATE artemis_core)
add_test(NAME pf8_regressions COMMAND pf8_regressions)

add_executable(preprocess_regressions preprocess_regressions.cpp)
target_link_libraries(preprocess_regressions PRIVATE artemis_core)
add_test(NAME preprocess_regressions COMMAND preprocess_regressions)

add_executable(psb_encrypted_regressions psb_encrypted_regressions.cpp)
target_link_libraries(psb_encrypted_regressions PRIVATE artemis_core)
add_test(NAME psb_encrypted_regressions COMMAND psb_encrypted_regressions)

add_executable(hlsl_glsl_regressions hlsl_glsl_regressions.cpp)
target_link_libraries(hlsl_glsl_regressions PRIVATE artemis_core)
add_test(NAME hlsl_glsl_regressions COMMAND hlsl_glsl_regressions)

add_executable(emote_mesh_regressions emote_mesh_regressions.cpp)
target_link_libraries(emote_mesh_regressions PRIVATE artemis_core)
add_test(NAME emote_mesh_regressions COMMAND emote_mesh_regressions)

add_executable(block_decode_regressions block_decode_regressions.cpp)
target_link_libraries(block_decode_regressions PRIVATE artemis_core)
add_test(NAME block_decode_regressions COMMAND block_decode_regressions)

add_executable(layer_kind_regressions layer_kind_regressions.cpp)
target_link_libraries(layer_kind_regressions PRIVATE artemis_core)
add_test(NAME layer_kind_regressions COMMAND layer_kind_regressions)

# Video decoding only when the core was built with FFmpeg and the tool is
# available to synthesize a fixture.
get_target_property(ARTC_DEFINES artemis_deps INTERFACE_COMPILE_DEFINITIONS)
if(ARTC_DEFINES AND "ARTC_HAS_FFMPEG=1" IN_LIST ARTC_DEFINES)
    find_program(ARTC_FFMPEG_TOOL ffmpeg)
    if(ARTC_FFMPEG_TOOL)
        set(ARTC_TEST_MOVIE "${CMAKE_CURRENT_BINARY_DIR}/synthetic-movie.mp4")
        add_custom_command(OUTPUT "${ARTC_TEST_MOVIE}"
            COMMAND "${ARTC_FFMPEG_TOOL}" -loglevel error -y
                -f lavfi -i color=c=red:s=32x32:r=10:d=0.5
                -f lavfi -i sine=frequency=440:sample_rate=48000:duration=0.5
                -c:v mpeg4 -c:a aac -shortest "${ARTC_TEST_MOVIE}"
            VERBATIM)
        add_executable(video_decoder_regressions video_decoder_regressions.cpp
            "${ARTC_TEST_MOVIE}")
        target_link_libraries(video_decoder_regressions PRIVATE artemis_core)
        add_test(NAME video_decoder_regressions COMMAND video_decoder_regressions
            "${ARTC_TEST_MOVIE}")
    endif()
endif()

# GLES compositor regressions need a real context (desktop ANGLE, or the OHOS
# system EGL/GLES). Off by default.
option(ARTC_TEST_GLES "Run compositor regressions using desktop ANGLE" OFF)
if(ARTC_TEST_GLES)
    if(NOT OHOS)
        find_package(unofficial-angle CONFIG REQUIRED)
        find_path(ARTC_GLES_INCLUDE_DIR GLES2/gl2.h REQUIRED)
        target_include_directories(artemis_core PUBLIC ${ARTC_GLES_INCLUDE_DIR})
        target_compile_definitions(artemis_core PUBLIC ARTC_HAS_GLES=1)
        target_link_libraries(artemis_core PUBLIC
            unofficial::angle::libEGL unofficial::angle::libGLESv2)
    endif()
    add_executable(compositor_regressions compositor_regressions.cpp)
    if(ARTC_TEST_MOVIE)
        target_sources(compositor_regressions PRIVATE "${ARTC_TEST_MOVIE}")
        target_compile_definitions(compositor_regressions PRIVATE
            ARTC_TEST_MOVIE="${ARTC_TEST_MOVIE}")
    endif()
    target_compile_definitions(compositor_regressions PRIVATE
        ARTC_TEST_DATA="${ARTC_TEST_DATA_DIR}")
    target_link_libraries(compositor_regressions PRIVATE artemis_core)
    add_test(NAME compositor_regressions COMMAND compositor_regressions)
endif()
