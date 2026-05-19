cmake_minimum_required(VERSION 3.22.1)

project(dsre_audio C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(
    dsre_audio
    SHARED
    libdsre_audio.c
)

set(FFMPEG_ROOT ${CMAKE_CURRENT_LIST_DIR}/ffmpeg/${ANDROID_ABI})
set(FFMPEG_INCLUDE_DIR ${FFMPEG_ROOT}/include)
set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/lib)

target_include_directories(
    dsre_audio
    PRIVATE
    ${FFMPEG_INCLUDE_DIR}
)

add_library(avformat SHARED IMPORTED)
set_target_properties(
    avformat
    PROPERTIES
    IMPORTED_LOCATION
    ${FFMPEG_LIB_DIR}/libavformat.so
)

add_library(avcodec SHARED IMPORTED)
set_target_properties(
    avcodec
    PROPERTIES
    IMPORTED_LOCATION
    ${FFMPEG_LIB_DIR}/libavcodec.so
)

add_library(avutil SHARED IMPORTED)
set_target_properties(
    avutil
    PROPERTIES
    IMPORTED_LOCATION
    ${FFMPEG_LIB_DIR}/libavutil.so
)

add_library(swresample SHARED IMPORTED)
set_target_properties(
    swresample
    PROPERTIES
    IMPORTED_LOCATION
    ${FFMPEG_LIB_DIR}/libswresample.so
)
add_library(swscale SHARED IMPORTED)
set_target_properties(
    swscale
    PROPERTIES
    IMPORTED_LOCATION
    ${FFMPEG_LIB_DIR}/libswscale.so
)


target_link_libraries(
    dsre_audio
    avformat
    avcodec
    swresample
    swscale
    avutil
    m
    z
    dl
    log
)

target_compile_options(
    dsre_audio
    PRIVATE
    -Wall
    -Wextra
    -Wno-unused-label
    -Wno-unused-parameter
)

