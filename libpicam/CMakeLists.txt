cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

#find_package(Boost REQUIRED COMPONENTS program_options)

#add_library(libpicam picam.cpp)
#find_package(PkgConfig)
#pkg_check_modules(LIBAV REQUIRED libavformat libavcodec)
#include_directories(${LIBAV_INCLUDE_DIRS})
#pkg_check_modules(LIBFONT REQUIRED harfbuzz fontconfig)
#include_directories(${LIBFONT_INCLUDE_DIRS})
#target_link_libraries(libpicam picam_option video_encoder timestamp audio log httplivestreaming muxer libstate libhook preview rtsp asound ${LIBFONT_LIBRARIES} lzma pthread ${LIBCAMERA_LINK_LIBRARIES} ${Boost_LIBRARIES})
#target_link_libraries(libpicam ${LIBAV_LIBRARIES} -static)
#set_target_properties(libpicam PROPERTIES PREFIX "")

#install(TARGETS libpicam LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

project(picam)
find_package(PkgConfig)
pkg_check_modules(LIBFONT REQUIRED harfbuzz fontconfig)
include_directories(${LIBFONT_INCLUDE_DIRS})

pkg_check_modules(LIBAV REQUIRED libavformat libavcodec)
# Remove "fdk-aac" from LIBAV_LIBRARIES and stores it into LIBAV_WITHOUT_FDK
string(REPLACE fdk-aac "" LIBAV_WITHOUT_FDK "${LIBAV_LIBRARIES}")
include_directories(${LIBAV_INCLUDE_DIRS})

add_executable(picam main.cpp picam.cpp)
target_link_libraries(picam picam_option video_encoder timestamp audio log httplivestreaming muxer libstate libhook preview rtsp asound ${LIBFONT_LIBRARIES} ${LIBCAMERA_LINK_LIBRARIES} ${LIBAV_WITHOUT_FDK} lzma pthread)

# statically link libfdk-aac
target_link_libraries(picam fdk-aac.a)

set(EXECUTABLES picam)
set(EXECUTABLE_OUTPUT_PATH  ${CMAKE_BINARY_DIR})
install(TARGETS ${EXECUTABLES} RUNTIME DESTINATION bin)
