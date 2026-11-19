# psvp_embed(INPUT OUTPUT): writes the bytes of INPUT as a comma-separated list
# of numbers to OUTPUT, for an #include inside an array's braces. Done when the
# build is configured, and again whenever INPUT changes.
function(psvp_embed input output)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(READ "${input}" bytes HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${bytes}")
    # One long line is hard on some tools: a line break every 8 bytes.
    string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],)(0x[0-9a-f][0-9a-f],))" "\\1\n" bytes "${bytes}")
    file(WRITE "${output}.new" "${bytes}\n")
    file(COPY_FILE "${output}.new" "${output}" ONLY_IF_DIFFERENT)
    file(REMOVE "${output}.new")
endfunction()
