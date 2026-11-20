cmake_minimum_required(VERSION 3.25)

# The version lives here and nowhere else; the sources read it from the generated version.h.
project(ps-vulkanplay
    VERSION 0.0.1
    DESCRIPTION "PlayStation Remote Play client"
    HOMEPAGE_URL "https://github.com/codingncaffeine/PS-VulkanPlay"
    LANGUAGES CXX)

option(PSVP_BUILD_APP "Build the application (needs SDL3 and Vulkan)" ON)
option(PSVP_BUILD_TESTS "Build the tests and the test tools" ON)
option(PSVP_WERROR "Treat warnings as errors" OFF)
option(PSVP_FUZZ "Build the fuzzers (clang with libFuzzer)" OFF)
set(PSVP_SANITIZE "" CACHE STRING "Sanitizers to build with: empty, address (with undefined) or thread")
set_property(CACHE PSVP_SANITIZE PROPERTY STRINGS "" address thread)
set(PSVP_ORACLE_DIR "" CACHE PATH
    "A checkout of the reference implementation, for the differential tests only (see tools/oracle)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Every program lands at the top of the build folder, where the scripts look for it.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

include(cmake/CompilerSetup.cmake)

configure_file(src/app/version.h.in generated/psvp/version.h @ONLY)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# The protocol: no sockets, no threads, no clock of its own.
add_library(psvp_proto STATIC
    src/proto/base64.cpp
    src/proto/controller_feedback.cpp
    src/proto/discovery.cpp
    src/proto/erasure.cpp
    src/proto/frame_assembler.cpp
    src/proto/hex.cpp
    src/proto/input.cpp
    src/proto/key_exchange.cpp
    src/proto/launch_spec.cpp
    src/proto/media_packet.cpp
    src/proto/media_sender.cpp
    src/proto/probe.cpp
    src/proto/regist.cpp
    src/proto/rp_cipher.cpp
    src/proto/session.cpp
    src/proto/sound.cpp
    src/proto/stream_cipher.cpp
    src/proto/stream_message.cpp
    src/proto/stream_setup.cpp
    src/proto/text_message.cpp
    src/proto/transport.cpp
    src/proto/wire.cpp)
target_include_directories(psvp_proto PUBLIC src)
target_link_libraries(psvp_proto PRIVATE OpenSSL::Crypto)
psvp_setup_target(psvp_proto)

# Sockets with deadlines.
add_library(psvp_net STATIC
    src/net/socket.cpp)
target_include_directories(psvp_net PUBLIC src)
psvp_setup_target(psvp_net)

# Decoding: pictures with FFmpeg, sound with Opus, and the generated clip that checks both.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PSVP_FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(PSVP_OPUS REQUIRED IMPORTED_TARGET opus)
add_library(psvp_media STATIC
    src/media/color.cpp
    src/media/frame_pacer.cpp
    src/media/frame_refs.cpp
    src/media/haptics.cpp
    src/media/sound_decoder.cpp
    src/media/sound_buffer.cpp
    src/media/test_clip.cpp
    src/media/video_decoder.cpp)
target_include_directories(psvp_media PUBLIC src)
target_link_libraries(psvp_media PRIVATE PkgConfig::PSVP_FFMPEG PkgConfig::PSVP_OPUS)
psvp_setup_target(psvp_media)

# What the application needs besides a window: options, folders, the log, the
# list of registered consoles and the command-line commands.
add_library(psvp_core STATIC
    src/app/account_id.cpp
    src/app/args.cpp
    src/app/chiaki_import.cpp
    src/app/commands.cpp
    src/app/console_store.cpp
    src/app/decoding.cpp
    src/app/json.cpp
    src/app/log.cpp
    src/app/paths.cpp
    src/app/window_place.cpp)
target_include_directories(psvp_core PUBLIC src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(psvp_core PUBLIC psvp_proto psvp_net psvp_media)
psvp_setup_target(psvp_core)

if(PSVP_BUILD_APP)
    find_package(SDL3 3.2 REQUIRED CONFIG)
    find_package(Vulkan REQUIRED)

    # The shaders are compiled when the program is built, into arrays inside it.
    find_program(PSVP_GLSLANG NAMES glslangValidator glslang REQUIRED)
    function(psvp_shader source name variable)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/psvp/${name}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${PSVP_GLSLANG}" -V --quiet --vn "${variable}" -o "${output}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}" VERBATIM)
        set_property(GLOBAL APPEND PROPERTY PSVP_SHADERS "${output}")
    endfunction()
    psvp_shader(src/render/shaders/video.vert video_vert kVideoVertexShader)
    psvp_shader(src/render/shaders/video.frag video_frag kVideoFragmentShader)
    get_property(PSVP_SHADER_FILES GLOBAL PROPERTY PSVP_SHADERS)

    add_library(psvp_render STATIC
        src/render/video_pass.cpp
        src/render/vk_window.cpp
        ${PSVP_SHADER_FILES})
    # The window's icon, in two sizes, inside the program: it needs no installed file to show it.
    include(cmake/EmbedFile.cmake)
    psvp_embed("${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/window-256.bmp" "${CMAKE_CURRENT_BINARY_DIR}/generated/psvp/window_icon_256.inc")
    psvp_embed("${CMAKE_CURRENT_SOURCE_DIR}/assets/icons/window-48.bmp" "${CMAKE_CURRENT_BINARY_DIR}/generated/psvp/window_icon_48.inc")
    target_link_libraries(psvp_render PUBLIC psvp_core PRIVATE SDL3::SDL3 Vulkan::Vulkan)
    psvp_setup_target(psvp_render)

    add_executable(ps-vulkanplay
        src/app/haptics_volume.cpp
        src/app/main.cpp
        src/app/player.cpp)
    target_link_libraries(ps-vulkanplay PRIVATE psvp_core psvp_proto psvp_render SDL3::SDL3)
    # Asking the desktop's sound service for a controller's haptics volume: where its client
    # library is there. Without it the program is the same, less that one step.
    pkg_check_modules(PSVP_PULSE IMPORTED_TARGET libpulse)
    if(PSVP_PULSE_FOUND)
        target_compile_definitions(ps-vulkanplay PRIVATE PSVP_HAVE_PULSE)
        target_link_libraries(ps-vulkanplay PRIVATE PkgConfig::PSVP_PULSE)
    endif()
    psvp_setup_target(ps-vulkanplay)

    include(GNUInstallDirs)
    install(TARGETS ps-vulkanplay RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    # The desktop entry is named after the application ID the window carries, which is how a
    # desktop finds the entry, and through it the icon, for a running window.
    install(FILES packaging/io.github.codingncaffeine.PSVulkanPlay.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(DIRECTORY packaging/icons/hicolor DESTINATION "${CMAKE_INSTALL_DATADIR}/icons")
endif()

if(PSVP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
