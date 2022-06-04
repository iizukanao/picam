cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(subtitle subtitle.c)
# target_link_libraries(subtitle)

install(TARGETS subtitle LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

