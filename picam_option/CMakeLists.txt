cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(picam_option STATIC picam_option.cpp)
target_link_libraries(picam_option)

install(TARGETS picam_option LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

