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
add_dependencies(nxnative_core game_profile)
