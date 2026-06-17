find_program(GLSLC glslc REQUIRED)

set(shader_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(shader_out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${shader_out_dir})

set(spv_files "")
foreach(shader mesh.vert mesh.frag text.vert text.frag)
  add_custom_command(
    OUTPUT ${shader_out_dir}/${shader}.spv
    COMMAND ${GLSLC} ${shader_src_dir}/${shader} -o ${shader_out_dir}/${shader}.spv
    DEPENDS ${shader_src_dir}/${shader}
    COMMENT "glslc ${shader}")
  list(APPEND spv_files ${shader_out_dir}/${shader}.spv)
endforeach()
add_custom_target(kiln_shaders DEPENDS ${spv_files})

add_library(kiln_render STATIC render.c)
add_dependencies(kiln_render kiln_shaders)

# Ship the compiled shaders next to the binary so an installed build finds them
# at <exe>/../share/kiln/shaders (see core_resource_dir).
install(DIRECTORY ${shader_out_dir}/ DESTINATION share/kiln/shaders
        FILES_MATCHING PATTERN "*.spv")
target_include_directories(kiln_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kiln_render PRIVATE KILN_SHADER_DIR="${shader_out_dir}")
target_link_libraries(kiln_render PUBLIC kiln_core kiln_platform Vulkan::Vulkan X11::X11)
