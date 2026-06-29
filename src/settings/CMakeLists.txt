add_library(kiln_settings STATIC settings.c)
target_include_directories(kiln_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(kiln_settings PUBLIC kiln_core kiln_platform)
