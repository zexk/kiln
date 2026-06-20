find_program(GLSLC glslc REQUIRED)
find_package(Vulkan REQUIRED)

# ── Shader compilation ───────────────────────────────────────────────────────
set(shader_src_dir ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(shader_out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${shader_out_dir})

set(spv_files "")
foreach(shader
    basic.vert   basic.frag
    hud.vert     hud.frag
    skybox.vert  skybox.frag
    outline.vert outline.frag)
    add_custom_command(
        OUTPUT  ${shader_out_dir}/${shader}.spv
        COMMAND ${GLSLC} ${shader_src_dir}/${shader}
                         -o ${shader_out_dir}/${shader}.spv
        DEPENDS ${shader_src_dir}/${shader}
        COMMENT "glslc ${shader}")
    list(APPEND spv_files ${shader_out_dir}/${shader}.spv)
endforeach()
add_custom_target(kyub_shaders DEPENDS ${spv_files})

# ── Game executable ──────────────────────────────────────────────────────────
add_executable(kyub
    main.c
    ecs.c
    voxel.c
    mesh.c
    camera.c
    components.c
    world.c
    texture.c
    gui.c
    systems.c)

add_dependencies(kyub kyub_shaders)

target_include_directories(kyub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # game/ shims + headers
    ${CMAKE_SOURCE_DIR}/src/core           # linalg.h, log.h, timer.h, noise.h, frustum.h, input.h
    ${CMAKE_SOURCE_DIR}/src/platform       # platform.h
    ${CMAKE_SOURCE_DIR}/src/render)        # renderer.h

target_link_libraries(kyub PRIVATE
    kiln_renderer
    kiln_platform
    kiln_core)
