cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

add_library(video_encoder video_encoder.cpp)
target_link_libraries(video_encoder text subtitle timestamp)

install(TARGETS video_encoder LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

