cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(httplivestreaming STATIC httplivestreaming.c)
target_link_libraries(httplivestreaming mpegts crypto)

install(TARGETS httplivestreaming LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

