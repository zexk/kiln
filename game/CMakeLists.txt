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
    voxel.c
    mesh.c
    components.c
    world.c
    texture.c
    gui.c
    systems.c)

# ── Asset copy ───────────────────────────────────────────────────────────────
add_custom_command(TARGET kyub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/assets
        ${CMAKE_CURRENT_BINARY_DIR}/assets)

add_dependencies(kyub kyub_shaders)

target_include_directories(kyub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}            # game/ shims + headers
    ${CMAKE_SOURCE_DIR}/src/core           # linalg.h, noise.h, frustum.h, input.h
    ${CMAKE_SOURCE_DIR}/src/platform       # platform.h
    ${CMAKE_SOURCE_DIR}/src/render         # renderer.h
    ${CMAKE_SOURCE_DIR}/src/ui             # ui.h
    ${CMAKE_SOURCE_DIR}/src/ecs            # world_t, entity_t, component_id_t
    ${CMAKE_SOURCE_DIR}/src/camera         # fps_camera.h
    ${CMAKE_SOURCE_DIR}/src/physics)       # physics.h

target_link_libraries(kyub PRIVATE
    kiln_renderer
    kiln_ecs
    kiln_camera
    kiln_platform
    kiln_physics
    kiln_core
    kiln_ui)
