set(KILN_RENDERER "vulkan" CACHE STRING "Renderer backend (vulkan)")
set_property(CACHE KILN_RENDERER PROPERTY STRINGS vulkan)

if(KILN_RENDERER STREQUAL "vulkan")
    find_program(GLSLC glslc REQUIRED)
    find_package(Vulkan REQUIRED)

    set(shader_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
    set(shader_out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${shader_out_dir})

    set(spv_files "")
    foreach(shader mesh.vert mesh.frag mesh_inst.vert shadow.vert shadow_inst.vert text.vert text.frag)
        add_custom_command(
            OUTPUT  ${shader_out_dir}/${shader}.spv
            COMMAND ${GLSLC} ${shader_src_dir}/${shader}
                             -o ${shader_out_dir}/${shader}.spv
            DEPENDS ${shader_src_dir}/${shader}
            COMMENT "glslc ${shader}")
        list(APPEND spv_files ${shader_out_dir}/${shader}.spv)
    endforeach()
    add_custom_target(kiln_shaders DEPENDS ${spv_files})

    add_library(kiln_render STATIC render_vk.c particles.c)
    add_dependencies(kiln_render kiln_shaders)
    target_compile_definitions(kiln_render PRIVATE
        KILN_SHADER_DIR="${shader_out_dir}")
    target_link_libraries(kiln_render PUBLIC
        kiln_core kiln_platform Vulkan::Vulkan
        $<$<NOT:$<BOOL:${WIN32}>>:X11::X11>)

    install(DIRECTORY ${shader_out_dir}/ DESTINATION share/kiln/shaders
            FILES_MATCHING PATTERN "*.spv")

    # ── Low-level abstract renderer (renderer_* API, ported from Kyub) ────────
    add_library(kiln_renderer STATIC
        r_renderer.c
        r_instance.c
        r_swapchain.c
        r_command.c
        r_pipeline.c
        r_buffer.c
        r_texture.c
        r_draw.c)
    target_link_libraries(kiln_renderer PUBLIC
        kiln_core kiln_platform Vulkan::Vulkan
        $<$<NOT:$<BOOL:${WIN32}>>:X11::X11>)
    target_include_directories(kiln_renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

else()
    message(FATAL_ERROR "Unknown renderer backend: ${KILN_RENDERER}")
endif()

target_include_directories(kiln_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
