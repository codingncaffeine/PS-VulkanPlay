# Warnings, hardening and sanitizers. Hardening and sanitizer flags apply to
# everything built here; the warning set applies to our own targets only
# (psvp_setup_target), so code compiled for the differential tests is not held to it.
include(CheckCXXCompilerFlag)
include(CheckPIESupported)

check_pie_supported(LANGUAGES CXX)

set(PSVP_WARNINGS
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wformat=2 -Wformat-security
    -Wnon-virtual-dtor -Woverloaded-virtual -Wold-style-cast -Wcast-align
    -Wimplicit-fallthrough -Wundef)

function(psvp_add_checked_flag flag)
    string(MAKE_C_IDENTIFIER "PSVP_HAS${flag}" var)
    check_cxx_compiler_flag("${flag}" ${var})
    if(${var})
        add_compile_options("${flag}")
    endif()
endfunction()

if(PSVP_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(PSVP_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT PSVP_SANITIZE STREQUAL "")
    message(FATAL_ERROR "PSVP_SANITIZE must be empty, address or thread (got '${PSVP_SANITIZE}')")
endif()

# Run-time checks that are cheap enough to ship.
psvp_add_checked_flag(-fstack-protector-strong)
psvp_add_checked_flag(-fstack-clash-protection)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    psvp_add_checked_flag(-fcf-protection=full)
endif()
add_compile_definitions(_GLIBCXX_ASSERTIONS _LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST)
if(PSVP_SANITIZE STREQUAL "" AND CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
    # Some distributions predefine a lower level; ours replaces it without a redefinition warning.
    add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_link_options(-Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack)
endif()

function(psvp_setup_target target)
    target_compile_options(${target} PRIVATE ${PSVP_WARNINGS})
    if(PSVP_WERROR)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()
