cmake_minimum_required(VERSION 3.6)

include(GNUInstallDirs)

find_package(Boost REQUIRED COMPONENTS program_options)

add_library(libpicam picam.cpp)
find_package(PkgConfig)
pkg_check_modules(LIBAV REQUIRED harfbuzz fontconfig libavformat libavcodec)
include_directories(${LIBAV_INCLUDE_DIRS})
target_link_libraries(libpicam picam_option video_encoder outputs timestamp audio log httplivestreaming muxer libstate libhook preview rtsp asound ${LIBAV_LIBRARIES} lzma pthread ${LIBCAMERA_LINK_LIBRARIES} ${Boost_LIBRARIES})

install(TARGETS muxer LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
