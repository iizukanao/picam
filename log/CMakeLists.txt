cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(log STATIC log.c)
target_link_libraries(log)

install(TARGETS log LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

