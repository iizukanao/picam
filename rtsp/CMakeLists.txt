cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(rtsp STATIC rtsp.c)
target_link_libraries(rtsp)

install(TARGETS rtsp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
