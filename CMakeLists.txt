cmake_minimum_required(VERSION 3.20)
project(nxnative_asahi VERSION 0.1.0 LANGUAGES C ASM)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(NXNATIVE_BUILD_TESTS "Build the Asahi port's tests" ON)
option(NXNATIVE_BUILD_HOST "Build the guest-execution host (needs a game profile)" OFF)
option(NXNATIVE_WERROR "Treat warnings as errors" OFF)
set(NXNATIVE_GAME "" CACHE STRING "Base game title ID for a host build (e.g. 010015100B514000)")
set(NXNATIVE_VK_VALIDATION "OFF" CACHE STRING "Enable Vulkan validation layers by default (ON/OFF)")

# The generated profile header is needed by the host, the NVN replay and the
# 'profile' command; generate it as soon as a title ID is selected.
set(GAME_HEADER_DIR "")
if(NXNATIVE_GAME)
  set(GAME_HEADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen/${NXNATIVE_GAME})
  add_custom_command(
    OUTPUT ${GAME_HEADER_DIR}/game_profile.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${GAME_HEADER_DIR}
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/dm/games/game.py header ${NXNATIVE_GAME} ${GAME_HEADER_DIR}/game_profile.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/dm/games/${NXNATIVE_GAME}/game.tsv
            ${CMAKE_CURRENT_SOURCE_DIR}/dm/games/game.py
    COMMENT "Generating game profile header"
  )
  add_custom_target(game_profile DEPENDS ${GAME_HEADER_DIR}/game_profile.h)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(NXNATIVE_WERROR)
  add_compile_options(-Werror)
endif()
add_compile_definitions(_GNU_SOURCE)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  add_compile_definitions(NX_ARCH_AARCH64=1)
else()
  add_compile_definitions(NX_ARCH_AARCH64=0)
endif()

# ---------------------------------------------------------------------------
# Dependencies (system only)
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# Platform layer
# ---------------------------------------------------------------------------
add_library(nxnative_platform STATIC
  src/platform/linux/nx_platform.c
  src/platform/linux/nx_compat.c
  src/platform/linux/nx_audio_sdl.c
  src/platform/linux/nx_window_sdl.c
)
target_include_directories(nxnative_platform PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/host
)
target_link_libraries(nxnative_platform PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# Memory layer (16 KiB host pages / 4 KiB guest pages)
# ---------------------------------------------------------------------------
add_library(nxnative_memory STATIC
  src/memory/nx_mem.c
)
target_include_directories(nxnative_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nxnative_memory PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Native Vulkan device + WSI
# ---------------------------------------------------------------------------
add_library(nxnative_vk STATIC
  src/graphics/vulkan/nx_vk.c
)
target_include_directories(nxnative_vk PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/third_party/vulkan-headers/include
)
target_link_libraries(nxnative_vk PUBLIC nxnative_platform Vulkan::Vulkan Threads::Threads)

# ---------------------------------------------------------------------------
# Vulkan API interception for titles that ship a Vulkan loader.
# The bulk of the entry points are generated forwarding stubs (see
# tools/gen_vk_entries.py); vk_api.c holds the handful that need translation.
# ---------------------------------------------------------------------------
set(VK_ENTRIES_C ${CMAKE_CURRENT_BINARY_DIR}/generated/vk_entries.c)
set(VK_ENTRIES_S ${CMAKE_CURRENT_BINARY_DIR}/generated/vk_entries.S)
add_custom_command(
  OUTPUT ${VK_ENTRIES_C} ${VK_ENTRIES_S}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_entries.py
          --names ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/vkapi/vk_names.txt
          --special ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/vkapi/vk_special.txt
          --out-c ${VK_ENTRIES_C} --out-s ${VK_ENTRIES_S}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_vk_entries.py
          ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/vkapi/vk_names.txt
          ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/vkapi/vk_special.txt
  COMMENT "Generating Vulkan entry stubs"
)
add_library(nxnative_vkapi STATIC
  src/graphics/vkapi/vk_api.c
  ${VK_ENTRIES_C}
  ${VK_ENTRIES_S}
)
target_include_directories(nxnative_vkapi PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/third_party/vulkan-headers/include
)
target_link_libraries(nxnative_vkapi PUBLIC nxnative_vk nxnative_platform nxnative_memory Threads::Threads ${CMAKE_DL_LIBS})

# ---------------------------------------------------------------------------
# NVN interface + Vulkan replay. It needs a game profile for the generated
# constants; the platform/Vulkan/guest-Vulkan targets do not depend on it.
# ---------------------------------------------------------------------------
add_library(nxnative_shader STATIC
  src/graphics/nvn/nvn_spv.c
  src/graphics/nvn/nvn_spv_depth.c
  src/graphics/nvn/nvn_spv_alpha.c
  src/graphics/nvn/nvn_json.c
)
target_include_directories(nxnative_shader PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/nvn
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/third_party/vulkan-headers/include
)
target_link_libraries(nxnative_shader PUBLIC nxnative_platform m)

if(NXNATIVE_GAME)
  add_subdirectory(src/horizon)
endif()

if(NXNATIVE_GAME)
add_library(nxnative_nvn STATIC
  dm/gfx/nvn/hle_nvn.c
  dm/gfx/nvn/nvn_capture.c
  dm/gfx/tex/nvn_deswizzle.c
  src/graphics/nvn/nvn_vk.c
  src/graphics/nvn/nvn_thunks_linux.S
  src/graphics/nvn/nvn_entry_linux.S
)
target_include_directories(nxnative_nvn PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/host
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/gfx
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/gfx/tex
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/gfx/nvn
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/gfx/nvn/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/horizon
  ${CMAKE_CURRENT_SOURCE_DIR}/src/horizon/compat
  ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux
  ${CMAKE_CURRENT_SOURCE_DIR}/dm/third_party/vulkan-headers/include
  ${CMAKE_CURRENT_SOURCE_DIR}/tests
  ${GAME_HEADER_DIR}
)
target_compile_definitions(nxnative_nvn PRIVATE $<$<BOOL:${NXNATIVE_BUILD_TESTS}>:NXNATIVE_TEST_SHADERS=1>)
target_compile_options(nxnative_nvn PRIVATE $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_SOURCE_DIR}/src/horizon/linux_compat.h>)
target_link_libraries(nxnative_nvn PUBLIC nxnative_core nxnative_vk nxnative_shader nxnative_memory nxnative_platform Threads::Threads m ${CMAKE_DL_LIBS})
# Ninja's depfile misses the included implementation block; make it explicit.
set_source_files_properties(src/graphics/nvn/nvn_vk.c PROPERTIES OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/nvn/nvn_vk_draw.inc;${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/nvn/nvn_vk_state.h")
endif()

# ---------------------------------------------------------------------------
# Graphical launcher
# ---------------------------------------------------------------------------
add_executable(nxnative-launcher src/launcher/main.c)
target_compile_definitions(nxnative-launcher PRIVATE
  NXNATIVE_DEFAULT_GAMES_DIR=""
  NXNATIVE_VERSION="0.1.0")
target_link_libraries(nxnative-launcher PRIVATE ${CMAKE_DL_LIBS})
# Path buffers are bounded by the game record's own limits; snprintf truncation
# is deliberate and harmless here.
target_compile_options(nxnative-launcher PRIVATE -Wno-format-truncation)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_executable(nxnative-asahi src/frontend/main.c)
target_link_libraries(nxnative-asahi PRIVATE nxnative_vkapi nxnative_vk nxnative_memory nxnative_platform)
if(NXNATIVE_GAME)
  target_compile_definitions(nxnative-asahi PRIVATE NXNATIVE_HAS_PROFILE=1)
  target_include_directories(nxnative-asahi PRIVATE ${GAME_HEADER_DIR})
  add_dependencies(nxnative-asahi game_profile)
endif()
if(NXNATIVE_BUILD_HOST)
  # HLE services register themselves in ELF sections (swhle/swforce/swsf); a
  # static archive would drop objects nothing references by symbol and their
  # guest-visible services would silently disappear. Link the whole host.
  target_compile_definitions(nxnative-asahi PRIVATE NXNATIVE_BUILD_HOST=1)
  target_link_libraries(nxnative-asahi PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nxnative_core>"
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,nxnative_nvn>")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(NXNATIVE_BUILD_TESTS)
  enable_testing()

  add_executable(t_memory_16k tests/t_memory_16k.c)
  target_link_libraries(t_memory_16k PRIVATE nxnative_memory nxnative_platform)
  add_test(NAME memory_16k COMMAND t_memory_16k)

  add_executable(t_vulkan_smoke tests/t_vulkan_smoke.c)
  target_link_libraries(t_vulkan_smoke PRIVATE nxnative_vk nxnative_memory)
  add_test(NAME vulkan_smoke COMMAND t_vulkan_smoke)

  add_test(NAME launcher_list COMMAND nxnative-launcher --list)
  set_tests_properties(launcher_list PROPERTIES ENVIRONMENT "NXNATIVE_GAMES_DIR=${CMAKE_SOURCE_DIR}/dm/games")
  add_test(NAME launcher_ui COMMAND nxnative-launcher --self-test)
  set_tests_properties(launcher_ui PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
  add_test(NAME launcher_saves COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_saves.sh $<TARGET_FILE:nxnative-launcher>)
  add_test(NAME launcher_settings COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_launcher_settings.sh $<TARGET_FILE:nxnative-launcher>)

  add_executable(t_shader_cache tests/t_shader_cache.c)
  target_link_libraries(t_shader_cache PRIVATE nxnative_shader ${CMAKE_DL_LIBS})
  add_test(NAME shader_cache COMMAND t_shader_cache)

  add_executable(t_audio_pcm tests/t_audio_pcm.c)
  target_link_libraries(t_audio_pcm PRIVATE nxnative_platform)
  add_test(NAME audio_pcm COMMAND t_audio_pcm)

  add_executable(t_vk_window tests/t_vk_window.c)
  target_link_libraries(t_vk_window PRIVATE nxnative_vk nxnative_memory nxnative_platform m)
  add_test(NAME vk_window COMMAND t_vk_window)
  set_tests_properties(vk_window PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen")

  add_executable(t_vk_present tests/t_vk_present.c)
  target_link_libraries(t_vk_present PRIVATE nxnative_vk nxnative_memory nxnative_platform m)
  add_test(NAME vk_present COMMAND t_vk_present)

  if(NXNATIVE_GAME)
    add_executable(t_audio_out tests/t_audio_out.c)
    target_link_libraries(t_audio_out PRIVATE
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,nxnative_core>"
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,nxnative_nvn>")
    add_test(NAME audio_out COMMAND t_audio_out)

    add_executable(t_nvn_depth_zero tests/t_nvn_depth_zero.c)
    target_link_libraries(t_nvn_depth_zero PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_depth_zero COMMAND t_nvn_depth_zero)

    add_executable(t_nvn_msaa tests/t_nvn_msaa.c)
    target_link_libraries(t_nvn_msaa PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_msaa COMMAND t_nvn_msaa)

    add_executable(t_nvn_depth tests/t_nvn_depth.c)
    target_link_libraries(t_nvn_depth PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_depth COMMAND t_nvn_depth)

    add_executable(t_nvn_depth_geometry tests/t_nvn_depth_geometry.c)
    target_link_libraries(t_nvn_depth_geometry PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_depth_geometry COMMAND t_nvn_depth_geometry)

    add_executable(t_nvn_alpha tests/t_nvn_alpha.c)
    target_link_libraries(t_nvn_alpha PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_alpha COMMAND t_nvn_alpha)

    add_executable(t_nvn_logicop tests/t_nvn_logicop.c)
    target_link_libraries(t_nvn_logicop PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_logicop COMMAND t_nvn_logicop)

    add_executable(t_nvn_advblend tests/t_nvn_advblend.c)
    target_link_libraries(t_nvn_advblend PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_advblend COMMAND t_nvn_advblend)

    add_executable(t_nvn_compute_image tests/t_nvn_compute_image.c)
    target_link_libraries(t_nvn_compute_image PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_compute_image COMMAND t_nvn_compute_image)

    add_executable(t_nvn_cube tests/t_nvn_cube.c)
    target_link_libraries(t_nvn_cube PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_cube COMMAND t_nvn_cube)

    add_executable(t_nvn_3d tests/t_nvn_3d.c)
    target_link_libraries(t_nvn_3d PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_3d COMMAND t_nvn_3d)

    add_executable(t_nvn_array tests/t_nvn_array.c)
    target_link_libraries(t_nvn_array PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_array COMMAND t_nvn_array)

    add_executable(t_nvn_tbuf tests/t_nvn_tbuf.c)
    target_link_libraries(t_nvn_tbuf PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_tbuf COMMAND t_nvn_tbuf)

    add_executable(t_nvn_tess tests/t_nvn_tess.c)
    target_link_libraries(t_nvn_tess PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_tess COMMAND t_nvn_tess)

    add_executable(t_nvn_geometry tests/t_nvn_geometry.c)
    target_link_libraries(t_nvn_geometry PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_geometry COMMAND t_nvn_geometry)

    add_executable(t_nvn_geometry_topo tests/t_nvn_geometry_topo.c)
    target_link_libraries(t_nvn_geometry_topo PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_geometry_topo COMMAND t_nvn_geometry_topo)

    add_executable(t_nvn_indexed tests/t_nvn_indexed.c)
    target_link_libraries(t_nvn_indexed PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_indexed COMMAND t_nvn_indexed)

    add_executable(t_nvn_viewport tests/t_nvn_viewport.c)
    target_link_libraries(t_nvn_viewport PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_viewport COMMAND t_nvn_viewport)

    add_executable(t_nvn_drawtex tests/t_nvn_drawtex.c)
    target_link_libraries(t_nvn_drawtex PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_drawtex COMMAND t_nvn_drawtex)

    add_executable(t_nvn_readback tests/t_nvn_readback.c)
    target_link_libraries(t_nvn_readback PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_readback COMMAND t_nvn_readback)

    add_executable(t_nvn_view_swizzle tests/t_nvn_view_swizzle.c)
    target_link_libraries(t_nvn_view_swizzle PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_view_swizzle COMMAND t_nvn_view_swizzle)

    add_executable(t_nvn_copy tests/t_nvn_copy.c)
    target_link_libraries(t_nvn_copy PRIVATE nxnative_nvn)
    add_test(NAME nvn_copy COMMAND t_nvn_copy)

    add_executable(t_nvn_compute tests/t_nvn_compute.c)
    target_link_libraries(t_nvn_compute PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_compute COMMAND t_nvn_compute)

    add_executable(t_nvn_texture tests/t_nvn_texture.c)
    target_link_libraries(t_nvn_texture PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_texture COMMAND t_nvn_texture)

    add_executable(t_nvn_tex_ubo tests/t_nvn_tex_ubo.c)
    target_link_libraries(t_nvn_tex_ubo PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_tex_ubo COMMAND t_nvn_tex_ubo)

    add_executable(t_nvn_draw tests/t_nvn_draw.c)
    target_link_libraries(t_nvn_draw PRIVATE nxnative_nvn ${CMAKE_DL_LIBS})
    add_test(NAME nvn_draw COMMAND t_nvn_draw)

    add_executable(t_nvn_vk tests/t_nvn_vk.c)
    target_link_libraries(t_nvn_vk PRIVATE nxnative_nvn)
    add_test(NAME nvn_vk COMMAND t_nvn_vk)
  endif()

  add_executable(t_vkapi_smoke tests/t_vkapi_smoke.c)
  target_link_libraries(t_vkapi_smoke PRIVATE nxnative_vkapi)
  target_include_directories(t_vkapi_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  add_test(NAME vkapi_smoke COMMAND t_vkapi_smoke)
endif()

if(NXNATIVE_BUILD_HOST)
  set(SYNTH_GAME_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth-game)
  add_custom_command(
    OUTPUT ${SYNTH_GAME_DIR}/main.patched.bin ${SYNTH_GAME_DIR}/main.fixups.tsv
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_synth_module.py ${SYNTH_GAME_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_synth_module.py
    COMMENT "Generating the synthetic guest module"
  )
  set(SYNTH_VK_GAME_DIR ${CMAKE_CURRENT_BINARY_DIR}/synth-vk-game)
  add_custom_command(
    OUTPUT ${SYNTH_VK_GAME_DIR}/main.patched.bin ${SYNTH_VK_GAME_DIR}/main.fixups.tsv
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_synth_module.py --vulkan ${SYNTH_VK_GAME_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/make_synth_module.py
    COMMENT "Generating the synthetic Vulkan guest module"
  )
  add_custom_target(synth_game DEPENDS
    ${SYNTH_GAME_DIR}/main.patched.bin ${SYNTH_GAME_DIR}/main.fixups.tsv
    ${SYNTH_VK_GAME_DIR}/main.patched.bin ${SYNTH_VK_GAME_DIR}/main.fixups.tsv)
  add_dependencies(nxnative-asahi synth_game)
  add_test(NAME synth_guest
           COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy $<TARGET_FILE:nxnative-asahi> run ${SYNTH_GAME_DIR} main)
  set_tests_properties(synth_guest PROPERTIES PASS_REGULAR_EXPRESSION "nnMain returned")
  add_test(NAME synth_vk_guest
           COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=dummy $<TARGET_FILE:nxnative-asahi> run ${SYNTH_VK_GAME_DIR} main)
  set_tests_properties(synth_vk_guest PROPERTIES PASS_REGULAR_EXPRESSION "vulkan: instance created")
  # The offscreen video driver gives SDL a real window object and a Vulkan
  # surface, so these two runs exercise the windowed front end and the guest's
  # Vulkan interception with a live surface while remaining headless.
  add_test(NAME synth_guest_window
           COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=offscreen $<TARGET_FILE:nxnative-asahi> run ${SYNTH_GAME_DIR} main)
  set_tests_properties(synth_guest_window PROPERTIES PASS_REGULAR_EXPRESSION "guest returned")
  add_test(NAME synth_vk_guest_window
           COMMAND ${CMAKE_COMMAND} -E env SDL_VIDEODRIVER=offscreen $<TARGET_FILE:nxnative-asahi> run ${SYNTH_VK_GAME_DIR} main)
  set_tests_properties(synth_vk_guest_window PROPERTIES PASS_REGULAR_EXPRESSION "vulkan: instance created")
endif()

# ---------------------------------------------------------------------------
# Optional host build (guest execution). The existing host is per-game; this
# target is added only when the profile is selected at configure time.
# ---------------------------------------------------------------------------
if(NXNATIVE_BUILD_HOST)
  if(NOT NXNATIVE_GAME)
    message(FATAL_ERROR "NXNATIVE_BUILD_HOST=ON needs -DNXNATIVE_GAME=<base title ID>")
  endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS nxnative-asahi nxnative-launcher RUNTIME DESTINATION bin)
install(FILES packaging/nxnative-asahi.desktop DESTINATION share/applications)
install(FILES README.md CONTRIBUTING.md LICENSE
              docs/ASAHI_ARCHITECTURE.md docs/ASAHI_STATUS.md docs/BUILDING.md
              docs/COMPATIBILITY.md docs/MEMORY_MODEL.md docs/GRAPHICS.md
              docs/TROUBLESHOOTING.md
        DESTINATION share/doc/nxnative-asahi)
