cmake_minimum_required(VERSION 3.10)

include(GNUInstallDirs)

add_library(audio STATIC audio.cpp)
# If asound is not linked, snd_pcm_hw_params_get_rate() will silently fail
target_link_libraries(audio asound)

install(TARGETS audio LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
