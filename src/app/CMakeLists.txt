add_executable(kiln main.c app.c)
target_link_libraries(kiln PRIVATE kiln_core kiln_platform kiln_ecs kiln_camera kiln_render kiln_assets)
target_compile_definitions(kiln PRIVATE KILN_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
install(TARGETS kiln RUNTIME DESTINATION bin)
