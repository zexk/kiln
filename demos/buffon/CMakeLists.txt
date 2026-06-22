add_executable(buffon main.c)
target_include_directories(buffon PRIVATE
    ${CMAKE_SOURCE_DIR}/src/render
    ${CMAKE_SOURCE_DIR}/src/platform
    ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(buffon PRIVATE kiln_render kiln_platform kiln_core)
install(TARGETS buffon RUNTIME DESTINATION bin)
