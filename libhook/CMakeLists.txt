cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(libhook hook.c)
target_link_libraries(libhook)

install(TARGETS muxer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
