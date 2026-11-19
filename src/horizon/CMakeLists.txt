# Linux guest-execution host. The sources are the proven nxnative host files;
# this target supplies the small platform compatibility layer and the ELF entry
# assembly. Apple-only sinks are replaced in src/platform/linux.
set(HOST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/host)
set(GFX_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/gfx)

add_library(nxnative_core STATIC
  ${HOST_DIR}/main.c
  ${HOST_DIR}/loader.c
  ${HOST_DIR}/hle.c
  ${HOST_DIR}/thread.c
  ${HOST_DIR}/hle_svc.c
  ${HOST_DIR}/hle_thread.c
  ${HOST_DIR}/hle_os.c
  ${HOST_DIR}/hle_time.c
  ${HOST_DIR}/hle_fs.c
  ${HOST_DIR}/hle_ro.c
  ${HOST_DIR}/hle_sf.c
  ${HOST_DIR}/hle_misc.c
  ${HOST_DIR}/hle_table.c
  ${HOST_DIR}/hle_sdk16.c
  ${HOST_DIR}/hle_sdkinit.c
  ${HOST_DIR}/hle_guestthread.c
  ${HOST_DIR}/hle_haptics.c
  ${CMAKE_SOURCE_DIR}/src/platform/linux/nx_host_stubs.c
  ${HOST_DIR}/hle_bigstack.c
  ${HOST_DIR}/hle_count.c
  ${HOST_DIR}/hle_fdstat.c
  ${HOST_DIR}/hle_oe.c
  ${HOST_DIR}/hle_offline.c
  ${HOST_DIR}/hle_account.c
  ${HOST_DIR}/hle_friends.c
  ${HOST_DIR}/hle_mii.c
  ${HOST_DIR}/hle_uecvar.c
  ${HOST_DIR}/hle_fpslock.c
  ${HOST_DIR}/hle_net.c
  ${HOST_DIR}/hle_nvdrv.c
  ${HOST_DIR}/hle_glslc.c
  ${HOST_DIR}/hle_oodle.c
  ${HOST_DIR}/save_sync.c
  ${HOST_DIR}/crash.c
  ${HOST_DIR}/hle_audren.c
  ${HOST_DIR}/hle_audren_sv.c
  ${HOST_DIR}/hle_audio_linux.c
  ${HOST_DIR}/hle_touch.c
  ${HOST_DIR}/hle_hid.c
  ${HOST_DIR}/hle_swkbd.c
  ${HOST_DIR}/hle_web.c
  ${GFX_DIR}/hle_nv.c
  ${GFX_DIR}/hle_vi.c
  ${GFX_DIR}/nvn/nvn_tex.c
  ${GFX_DIR}/nvn/nvn_dirty.c
  ${CMAKE_CURRENT_SOURCE_DIR}/entry_linux.S
  ${CMAKE_SOURCE_DIR}/src/platform/linux/nx_front.c
)

target_include_directories(nxnative_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/host
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/gfx/nvn/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/gfx/nvn
  ${CMAKE_CURRENT_SOURCE_DIR}/../../dm/third_party/vulkan-headers/include
  ${GAME_HEADER_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/compat
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/dm/third_party/libvpx
  ${CMAKE_SOURCE_DIR}/dm/third_party/dav1d/include
  ${CMAKE_SOURCE_DIR}/src/platform/linux
)
target_compile_definitions(nxnative_core PUBLIC NX_LINUX=1)
target_compile_options(nxnative_core PRIVATE $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/linux_compat.h>)
target_link_libraries(nxnative_core PUBLIC nxnative_platform nxnative_memory nxnative_vkapi nxnative_vk Threads::Threads m ${CMAKE_DL_LIBS})
# The movie library (the SDK's movie::Extractor / movie::Decoder, hle_movie.c)
# needs a video decoder. On Linux that is libavcodec: build the host's movie
# take-over and its H.264 / AAC back end when the headers are found (Fedora's
# libavcodec-free-devel provides them under /usr/include/ffmpeg), and leave
# movies out of the host otherwise. NXNATIVE_AVCODEC_INCLUDE_DIR and the two
# library paths can be given for a tree that is not installed system-wide.
option(NXNATIVE_MOVIES "Build the movie library take-over (needs libavcodec headers)" ON)
if(NXNATIVE_MOVIES AND NOT APPLE)
  find_path(NXNATIVE_AVCODEC_INCLUDE_DIR libavcodec/avcodec.h
            HINTS ${NXNATIVE_FFMPEG_INCLUDE_DIR} $ENV{NXNATIVE_FFMPEG_INCLUDE_DIR}
            PATH_SUFFIXES ffmpeg)
  find_library(NXNATIVE_AVCODEC_LIBRARY NAMES avcodec)
  find_library(NXNATIVE_AVUTIL_LIBRARY NAMES avutil)
  find_library(NXNATIVE_VPX_LIBRARY NAMES vpx)
  if(NOT NXNATIVE_VPX_LIBRARY)
    find_file(NXNATIVE_VPX_LIBRARY NAMES libvpx.so.9 libvpx.so.6 libvpx.so)
  endif()
  if(NXNATIVE_AVCODEC_INCLUDE_DIR AND NXNATIVE_AVCODEC_LIBRARY AND NXNATIVE_AVUTIL_LIBRARY AND NXNATIVE_VPX_LIBRARY)
    target_sources(nxnative_core PRIVATE
      ${HOST_DIR}/hle_movie.c
      ${HOST_DIR}/hle_movie_ffmpeg.c)
    target_include_directories(nxnative_core PRIVATE ${NXNATIVE_AVCODEC_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/dm/third_party/stb_vorbis)
    target_link_libraries(nxnative_core PUBLIC ${NXNATIVE_AVCODEC_LIBRARY} ${NXNATIVE_AVUTIL_LIBRARY} ${NXNATIVE_VPX_LIBRARY})
    target_compile_definitions(nxnative_core PUBLIC NXNATIVE_MOVIES=1)
    message(STATUS "movies: libavcodec ${NXNATIVE_AVCODEC_LIBRARY}, headers ${NXNATIVE_AVCODEC_INCLUDE_DIR}")
  else()
    message(STATUS "movies: libavcodec headers not found (install libavcodec-free-devel); the host plays no movies")
  endif()
endif()

add_dependencies(nxnative_core game_profile)
