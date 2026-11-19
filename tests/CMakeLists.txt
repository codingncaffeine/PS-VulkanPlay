add_executable(psvp_tests
    main.cpp
    unit/test_args.cpp
    unit/test_base64.cpp
    unit/test_paths.cpp)
target_include_directories(psvp_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(psvp_tests PRIVATE psvp_proto psvp_core)
psvp_setup_target(psvp_tests)
add_test(NAME unit COMMAND psvp_tests)

# Every fuzz target also runs over its saved inputs as an ordinary test, with any compiler.
function(psvp_add_fuzz_target name)
    add_executable(psvp_fuzz_${name}_replay fuzz/replay_main.cpp fuzz/fuzz_${name}.cpp)
    target_link_libraries(psvp_fuzz_${name}_replay PRIVATE psvp_proto)
    psvp_setup_target(psvp_fuzz_${name}_replay)
    add_test(NAME fuzz_${name}_replay
        COMMAND psvp_fuzz_${name}_replay "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/${name}")
    if(PSVP_FUZZ)
        add_executable(psvp_fuzz_${name} fuzz/fuzz_${name}.cpp)
        target_link_libraries(psvp_fuzz_${name} PRIVATE psvp_proto)
        target_compile_options(psvp_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        target_link_options(psvp_fuzz_${name} PRIVATE -fsanitize=fuzzer)
        psvp_setup_target(psvp_fuzz_${name})
    endif()
endfunction()

psvp_add_fuzz_target(base64)

# Reads the screen of the virtual X server a window check runs on (tools/launch-check.sh).
add_executable(psvp_fbcheck ../tools/fbcheck/fbcheck.cpp)
psvp_setup_target(psvp_fbcheck)

# Differential tests against the reference implementation. Test only: the
# reference is fetched into scratch/ by tools/oracle/run.sh and never shipped.
if(PSVP_ORACLE_DIR)
    if(NOT EXISTS "${PSVP_ORACLE_DIR}/lib/src/base64.c")
        message(FATAL_ERROR "PSVP_ORACLE_DIR has no lib/src/base64.c: ${PSVP_ORACLE_DIR}")
    endif()
    enable_language(C)
    add_library(psvp_oracle_reference STATIC
        "${PSVP_ORACLE_DIR}/lib/src/base64.c")
    target_include_directories(psvp_oracle_reference SYSTEM PUBLIC "${PSVP_ORACLE_DIR}/lib/include")
    target_compile_options(psvp_oracle_reference PRIVATE -w)

    add_executable(psvp_oracle_tests
        oracle/main.cpp
        oracle/diff_base64.cpp)
    target_link_libraries(psvp_oracle_tests PRIVATE psvp_proto psvp_oracle_reference)
    psvp_setup_target(psvp_oracle_tests)
    add_test(NAME oracle COMMAND psvp_oracle_tests)
endif()
