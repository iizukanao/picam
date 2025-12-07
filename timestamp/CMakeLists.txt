cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(timestamp STATIC timestamp.c)
target_link_libraries(timestamp text)

install(TARGETS timestamp LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

