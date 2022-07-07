cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(muxer STATIC muxer.cpp)
target_link_libraries(muxer)

install(TARGETS muxer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
