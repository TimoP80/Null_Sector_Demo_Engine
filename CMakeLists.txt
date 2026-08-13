# ---------------------------------------------------------------------------
# NULL SECTOR // DEMO ENGINE - data-driven demo engine
#
# Targets:
#   ns_framework  static lib - GL-free framework core (scripting, timeline,
#                scene graph, animation, json, assets, file watcher, rigs)
#   ns_fw_tests  unit tests for the framework core (no GL needed)
#   ns_demo      the demo app (engine + effects + DemoApp + GLFW)
#   ns_editor    standalone editor application using the same DemoEditor/runtime
#   ns_shader_ai standalone AI-assisted GLSL generation and preview workstation
#
# Options:
#   -DNS_BUILD_APP=OFF   build only the framework + tests (no GLFW needed)
#   -DNS_BUILD_TESTS=OFF skip the unit tests
#
# GLFW is found via find_package, else fetched from GitHub at configure time.
# miniaudio / stb are header-only, vendored in third_party/.
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.20)
project(NS_DemoEngine VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NS_BUILD_APP "Build the demo application (requires GLFW)" ON)
option(NS_BUILD_TESTS "Build framework unit tests" ON)

# absolute baked paths for runtime resource resolution (overridable at runtime
# via the NULLSECTOR_*_DIR environment variables - see src/engine/paths.hpp)
set(NS_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(NS_ASSET_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/assets")
set(NS_DATA_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/data")

# --- framework core (GL-free) ---------------------------------------------------
add_library(ns_framework STATIC
  src/framework/core/json.cpp
  src/framework/core/ffmpegpipe.cpp
  src/framework/script/scriptparser.cpp
  src/framework/script/nsdwriter.cpp
  src/editor/document.cpp
  src/framework/script/scriptengine.cpp
  src/framework/timeline/timelineeditor.cpp
  src/framework/anim/animation.cpp
  src/framework/scene/scenegraph.cpp
  src/framework/camera/camerarig.cpp
  src/framework/resources/filewatcher.cpp
  src/framework/resources/assetmanager.cpp
  src/framework/vfs/vfs.cpp
  src/framework/vfs/directoryfs.cpp
  src/framework/vfs/nspack.cpp
  src/framework/vfs/packagefs.cpp
  src/framework/vfs/miniz/miniz.c
  src/framework/vfs/miniz/miniz_tdef.c
  src/framework/vfs/miniz/miniz_tinfl.c
  src/shadertoy_convert.cpp
)
set_source_files_properties(
  src/framework/vfs/miniz/miniz.c
  src/framework/vfs/miniz/miniz_tdef.c
  src/framework/vfs/miniz/miniz_tinfl.c
  PROPERTIES COMPILE_DEFINITIONS "MINIZ_NO_ARCHIVE_APIS"
)
target_include_directories(ns_framework PUBLIC src)
target_compile_definitions(ns_framework PUBLIC
  NULLSECTOR_SHADER_DIR="${NS_SHADER_DIR}"
  NULLSECTOR_ASSET_DIR="${NS_ASSET_DIR}"
  NULLSECTOR_DATA_DIR="${NS_DATA_DIR}"
)
if(MSVC)
  target_compile_options(ns_framework PRIVATE /W3)
else()
  target_compile_options(ns_framework PRIVATE -Wall -Wno-unused-parameter)
endif()

# --- unit tests ------------------------------------------------------------------
if(NS_BUILD_TESTS)
  add_executable(ns_fw_tests tests/fw_test.cpp)
  target_link_libraries(ns_fw_tests PRIVATE ns_framework)
  if(NOT MSVC)
    target_compile_options(ns_fw_tests PRIVATE -Wall -Wno-unused-parameter)
  endif()
endif()

# --- demo app ---------------------------------------------------------------------
if(NS_BUILD_APP)
  find_package(glfw3 QUIET)
  if(NOT glfw3_FOUND)
    message(STATUS "glfw3 not found - fetching GLFW 3.4 from GitHub")
    include(FetchContent)
    FetchContent_Declare(glfw
      URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
  endif()

  # --- Dear ImGui (docking branch, vendored in third_party/imgui) ------------------
  add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(imgui PUBLIC third_party third_party/imgui)
  target_link_libraries(imgui PUBLIC glfw)
  if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
  else()
    target_compile_options(imgui PRIVATE -w)
  endif()

  # Runtime sources shared by the player and standalone editor. Keep editor
  # implementation files out of ns_demo so the distributed player contains
  # only playback, rendering, packaging, and runtime asset functionality.
  set(NS_RUNTIME_SOURCES
    # engine (low-level GL layer)
    src/engine/assets.cpp
    src/engine/audio.cpp
    src/engine/degraded.cpp
    src/engine/gl.cpp
    src/engine/postprocess.cpp
    src/engine/shader.cpp
    src/engine/shadercheck.cpp
    src/engine/ubo.cpp
    src/engine/video.cpp
    # effects (registered content)
    src/effects/greetings.cpp
    src/effects/intro.cpp
    src/effects/network.cpp
    src/effects/scene.cpp
    src/effects/splash.cpp
    src/effects/tunnel.cpp
    # app (data-driven director)
    src/app/appassets.cpp
    src/app/effectreg.cpp
    src/app/model.cpp
    src/app/packer.cpp
    src/app/modelcheck.cpp
    src/app/poststack.cpp
    src/app/prodcheck.cpp
    src/app/shadermanager.cpp
    src/app/shadertoy.cpp
    src/app/shadertoycheck.cpp
    src/app/demoapp.cpp
  )

  set(NS_EDITOR_SOURCES
    src/editor/editor.cpp
    src/editor/document.cpp
    src/editor/editor_document.cpp
    src/editor/editor_curves.cpp
    src/editor/editor_markers.cpp
    src/editor/editor_export.cpp
    src/editor/packaging.cpp
    src/editor/exportmp4.cpp
    src/editor/shaderlab.cpp
  )

  # The standalone editor reuses the same runtime sources and adds only the
  # authoring layer. ns_demo is deliberately runtime-only.
  add_executable(ns_demo src/main.cpp ${NS_RUNTIME_SOURCES})
  add_executable(ns_editor src/main.cpp src/editor_main.cpp ${NS_RUNTIME_SOURCES} ${NS_EDITOR_SOURCES})

  foreach(target ns_demo ns_editor)
    target_include_directories(${target} PRIVATE third_party)
    target_link_libraries(${target} PRIVATE ns_framework glfw)
    target_compile_definitions(${target} PRIVATE
      NULLSECTOR_SHADER_DIR="${NS_SHADER_DIR}"
      NULLSECTOR_ASSET_DIR="${NS_ASSET_DIR}"
      NULLSECTOR_DATA_DIR="${NS_DATA_DIR}"
    )
    if(WIN32)
      target_link_libraries(${target} PRIVATE opengl32 comdlg32)
    elseif(UNIX AND NOT APPLE)
      find_package(X11 QUIET)
      target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
    endif()
  endforeach()
  target_link_libraries(ns_editor PRIVATE imgui)
  target_compile_definitions(ns_demo PRIVATE NULLSECTOR_RUNTIME_ONLY=1)
  target_compile_definitions(ns_editor PRIVATE NULLSECTOR_STANDALONE_EDITOR=1)

  # Standalone AI shader workstation. It reuses the real engine Shader/VFS
  # path and Dear ImGui, but is intentionally separate from both playback and
  # the production editor.
  add_executable(ns_shader_ai
    src/shader_ai_main.cpp
    src/shader_ai.cpp
    src/engine/audio.cpp
    src/engine/gl.cpp
    src/engine/shader.cpp
  )
  target_include_directories(ns_shader_ai PRIVATE third_party)
  target_link_libraries(ns_shader_ai PRIVATE ns_framework glfw imgui)
  target_compile_definitions(ns_shader_ai PRIVATE
    NULLSECTOR_SHADER_DIR="${NS_SHADER_DIR}"
    NULLSECTOR_ASSET_DIR="${NS_ASSET_DIR}"
    NULLSECTOR_DATA_DIR="${NS_DATA_DIR}"
  )
  if(WIN32)
    target_link_libraries(ns_shader_ai PRIVATE opengl32 comdlg32 winhttp crypt32)
  elseif(UNIX AND NOT APPLE)
    target_link_libraries(ns_shader_ai PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()
