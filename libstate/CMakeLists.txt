cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(libstate STATIC state.c)
target_link_libraries(libstate)
set_target_properties(libstate PROPERTIES PREFIX "")

install(TARGETS libstate LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
