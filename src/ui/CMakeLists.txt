add_library(kiln_ui STATIC ui.c)
target_include_directories(kiln_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_ui PUBLIC kiln_core kiln_render)

# ── kiln_ui_gl: renderer-backed 2D primitives + ui_draw_t backend ────────────
# Built on the low-level abstract renderer (kiln_renderer). Ships its own HUD
# shaders, compiled to SPIR-V and located at runtime via $KILN_UI_SHADER_DIR
# (falling back to the baked build path).
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
add_custom_target(kiln_ui_gl_shaders DEPENDS ${ui_spv_files})

add_library(kiln_ui_gl STATIC ui_gl.c)
add_dependencies(kiln_ui_gl kiln_ui_gl_shaders)
target_include_directories(kiln_ui_gl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(kiln_ui_gl PRIVATE
    KILN_UI_SHADER_DIR="${ui_shader_out_dir}")
target_link_libraries(kiln_ui_gl PUBLIC kiln_ui kiln_renderer)

install(DIRECTORY ${ui_shader_out_dir}/ DESTINATION share/kiln/shaders
        FILES_MATCHING PATTERN "*.spv")
