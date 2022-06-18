cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(text text.c)
target_link_libraries(text log fontconfig harfbuzz)

install(TARGETS text LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

