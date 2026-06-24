find_program(GLSLC glslc REQUIRED)
set(ui_shader_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(ui_shader_out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${ui_shader_out_dir})

set(ui_spv_files "")
foreach(shader hud.vert hud.frag)
    add_custom_command(
        OUTPUT  ${ui_shader_out_dir}/${shader}.spv
        COMMAND ${GLSLC} ${ui_shader_src_dir}/${shader}
                         -o ${ui_shader_out_dir}/${shader}.spv
        DEPENDS ${ui_shader_src_dir}/${shader}
        COMMENT "glslc ui/${shader}")
    list(APPEND ui_spv_files ${ui_shader_out_dir}/${shader}.spv)
endforeach()
add_custom_target(kiln_ui_shaders DEPENDS ${ui_spv_files})

add_library(kiln_ui STATIC ui.c ui_gl.c)
add_dependencies(kiln_ui kiln_ui_shaders)
target_include_directories(kiln_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kiln_ui PRIVATE
    KILN_UI_SHADER_DIR="${ui_shader_out_dir}")
target_link_libraries(kiln_ui PUBLIC kiln_core kiln_render kiln_renderer)

install(DIRECTORY ${ui_shader_out_dir}/ DESTINATION share/kiln/shaders
        FILES_MATCHING PATTERN "*.spv")
