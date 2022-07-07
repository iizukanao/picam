cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(libhook STATIC hook.c)
target_link_libraries(libhook)
set_target_properties(libhook PROPERTIES PREFIX "")

install(TARGETS libhook LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
