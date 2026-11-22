add_executable(psvp_tests
    main.cpp
    unit/test_args.cpp
    unit/test_base64.cpp
    unit/test_color.cpp
    unit/test_tone_map.cpp
    unit/test_scaler.cpp
    unit/test_present_clock.cpp
    unit/test_peak_detect.cpp
    unit/test_connection_view.cpp
    unit/test_controller_feedback.cpp
    unit/test_decoder.cpp
    unit/test_discovery.cpp
    unit/test_erasure.cpp
    unit/test_frame_pacer.cpp
    unit/test_frame_refs.cpp
    unit/test_h264_refs.cpp
    unit/test_haptics.cpp
    unit/test_hevc_clip.cpp
    unit/test_hex.cpp
    unit/test_import.cpp
    unit/test_input.cpp
    unit/test_motion.cpp
    unit/test_stand_in_touch.cpp
    unit/test_json.cpp
    unit/test_media.cpp
    unit/test_overlay.cpp
    unit/test_controller_list.cpp
    unit/test_controller_presses.cpp
    unit/test_controller_settings.cpp
    unit/test_paths.cpp
    unit/test_probe.cpp
    unit/test_regist.cpp
    unit/test_rp_cipher.cpp
    unit/test_session.cpp
    unit/test_sound_buffer.cpp
    unit/test_store.cpp
    unit/test_stream_message.cpp
    unit/test_stream_menu.cpp
    unit/test_stream_setup.cpp
    unit/test_text_message.cpp
    unit/test_transport.cpp
    unit/test_screen_clock.cpp
    ../tools/screen-clock/timing.cpp)
target_include_directories(psvp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/screen-clock")
target_compile_definitions(psvp_tests PRIVATE "PSVP_TEST_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/scenario/fixtures\""
    "PSVP_TEST_DATA=\"${CMAKE_CURRENT_SOURCE_DIR}/data\"")
target_link_libraries(psvp_tests PRIVATE psvp_proto psvp_core)
psvp_setup_target(psvp_tests)
add_test(NAME unit COMMAND psvp_tests)

# Every fuzz target also runs over its saved inputs as an ordinary test, with any compiler.
function(psvp_add_fuzz_target name)
    add_executable(psvp_fuzz_${name}_replay fuzz/replay_main.cpp fuzz/fuzz_${name}.cpp)
    target_link_libraries(psvp_fuzz_${name}_replay PRIVATE psvp_core)
    psvp_setup_target(psvp_fuzz_${name}_replay)
    add_test(NAME fuzz_${name}_replay
        COMMAND psvp_fuzz_${name}_replay "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${name}")
    if(PSVP_FUZZ)
        add_executable(psvp_fuzz_${name} fuzz/fuzz_${name}.cpp)
        target_link_libraries(psvp_fuzz_${name} PRIVATE psvp_core)
        target_compile_options(psvp_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(psvp_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        psvp_setup_target(psvp_fuzz_${name})
    endif()
endfunction()

psvp_add_fuzz_target(base64)
psvp_add_fuzz_target(import)
psvp_add_fuzz_target(json)
psvp_add_fuzz_target(media)
psvp_add_fuzz_target(refs)
psvp_add_fuzz_target(h264refs)
psvp_add_fuzz_target(message)
psvp_add_fuzz_target(regist)
psvp_add_fuzz_target(session)
psvp_add_fuzz_target(stream)
psvp_add_fuzz_target(text)
psvp_add_fuzz_target(transport)

# Reads the screen of the virtual X server a window check runs on (tools/launch-check.sh).
add_executable(psvp_fbcheck ../tools/fbcheck/fbcheck.cpp)
psvp_setup_target(psvp_fbcheck)

# Draws pictures decoded on a graphics card as the window does, offscreen, and compares them with the
# software way (on a machine whose card decodes; tools/wall.sh with PSVP_WALL_GPU=1).
if(TARGET psvp_render)
    add_executable(psvp_render_check ../tools/render-check/render_check.cpp)
    target_link_libraries(psvp_render_check PRIVATE psvp_render psvp_media Vulkan::Vulkan)
    psvp_setup_target(psvp_render_check)
    # I5: libplacebo as the picture reference, where it is installed (a test tool only; never shipped).
    pkg_check_modules(PSVP_PLACEBO QUIET IMPORTED_TARGET libplacebo>=7)
    if(PSVP_PLACEBO_FOUND)
        add_executable(psvp_placebo_check ../tools/placebo-check/placebo_check.cpp)
        target_link_libraries(psvp_placebo_check PRIVATE psvp_media PkgConfig::PSVP_PLACEBO)
        psvp_setup_target(psvp_placebo_check)
    endif()
endif()

# The lab's clock on the screen (PLAN I3): when each frame of the clip reached a virtual screen, scored
# against when the console stand-in sent it (tests/scenario/lab.sh).
add_executable(psvp_screen_clock ../tools/screen-clock/screen_clock.cpp ../tools/screen-clock/timing.cpp)
target_link_libraries(psvp_screen_clock PRIVATE psvp_media)
psvp_setup_target(psvp_screen_clock)

# Stands in for a console in the scenario tests (tests/scenario).
add_executable(psvp-mock-console ../tools/mock-console/mock_console.cpp)
target_link_libraries(psvp-mock-console PRIVATE psvp_proto psvp_net psvp_media)
psvp_setup_target(psvp-mock-console)

# Differential tests against the reference implementation. Test only: the
# reference is fetched into scratch/ by tools/oracle/run.sh, builds its library
# inside this build folder for these programs, and is never shipped.
if(PSVP_ORACLE_DIR)
    if(NOT EXISTS "${PSVP_ORACLE_DIR}/lib/src/regist.c")
        message(FATAL_ERROR "PSVP_ORACLE_DIR is not a checkout of the reference: ${PSVP_ORACLE_DIR}")
    endif()
    enable_language(C)
    set(CHIAKI_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(CHIAKI_ENABLE_CLI OFF CACHE BOOL "" FORCE)
    set(CHIAKI_ENABLE_GUI OFF CACHE BOOL "" FORCE)
    set(CHIAKI_ENABLE_STEAM_SHORTCUT OFF CACHE BOOL "" FORCE)
    set(CHIAKI_ENABLE_SETSU OFF CACHE STRING "" FORCE)
    set(CHIAKI_ENABLE_STEAMDECK_NATIVE OFF CACHE STRING "" FORCE)
    set(CHIAKI_ENABLE_FFMPEG_DECODER OFF CACHE STRING "" FORCE)
    set(CHIAKI_ENABLE_PI_DECODER OFF CACHE STRING "" FORCE)
    set(CHIAKI_USE_SYSTEM_CURL ON CACHE STRING "" FORCE)
    set(CHIAKI_LIB_MINIUPNPC_EXTERNAL_PROJECT ON CACHE BOOL "" FORCE)
    # SYSTEM: its headers are not held to our warning set.
    add_subdirectory("${PSVP_ORACLE_DIR}" "${CMAKE_BINARY_DIR}/reference" EXCLUDE_FROM_ALL SYSTEM)

    add_executable(psvp_oracle_tests
        oracle/main.cpp
        oracle/diff_base64.cpp
        oracle/diff_discovery.cpp
        oracle/diff_erasure.cpp
        oracle/diff_media.cpp
        oracle/diff_regist.cpp
        oracle/diff_stream.cpp
        oracle/reference_messages.c)
    target_link_libraries(psvp_oracle_tests PRIVATE psvp_proto chiaki-lib)
    # The reference's generated message coder and the helpers its own sources use with it.
    set_source_files_properties(oracle/reference_messages.c PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_BINARY_DIR}/reference/lib/protobuf;${PSVP_ORACLE_DIR}/lib/src"
        COMPILE_OPTIONS "-w")
    psvp_setup_target(psvp_oracle_tests)
    add_test(NAME oracle COMMAND psvp_oracle_tests)

    # The reference's own client, to show that the mock console passes for a console (tests/scenario).
    add_executable(psvp_reference_client oracle/reference_client.c)
    target_link_libraries(psvp_reference_client PRIVATE chiaki-lib)
    target_compile_options(psvp_reference_client PRIVATE -Wall -Wextra)
endif()
