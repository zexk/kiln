add_executable(kiln-bake mesh_bake.c)
target_link_libraries(kiln-bake PRIVATE kiln_assets kiln_core)
