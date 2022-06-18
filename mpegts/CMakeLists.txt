cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(mpegts mpegts.c)
target_link_libraries(mpegts avutil avcodec)

install(TARGETS mpegts LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

