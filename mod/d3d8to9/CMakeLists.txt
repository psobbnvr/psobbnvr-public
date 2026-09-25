cmake_minimum_required(VERSION 3.12)
project(d3d8to9 CXX)

set(D3D8TO9_SOURCE
  source/d3d8.hpp
  source/d3d8to9.cpp
  source/d3d8to9.hpp
  source/d3d8to9_base.cpp
  source/d3d8to9_device.cpp
  source/d3d8to9_index_buffer.cpp
  source/d3d8to9_surface.cpp
  source/d3d8to9_swap_chain.cpp
  source/d3d8to9_texture.cpp
  source/d3d8to9_vertex_buffer.cpp
  source/d3d8to9_volume.cpp
  source/d3d8types.cpp
  source/d3d8types.hpp
  source/d3dx9.hpp
  source/interface_query.cpp
  source/interface_query.hpp
  source/psobbvr_probe.hpp
  source/psobbvr_bindings_core.hpp
  source/psobbvr_bindings.hpp
  source/psobbvr_resolution.hpp
  source/psobbvr_stereo.hpp
  source/psobbvr_d3d9ex.hpp
  source/psobbvr_vr.hpp
  source/psobbvr_vr_openvr.cpp
  source/psobbvr_vr_openxr.cpp
  source/psobbvr_log.hpp
  res/d3d8.def
  res/d3d8to9.rc
)

set(CMAKE_CXX_STANDARD 17)

option(D3D8TO9_STATIC "Build as a static library" OFF)

if(D3D8TO9_STATIC)
  add_library(d3d8to9 STATIC ${D3D8TO9_SOURCE})
else()
  add_library(d3d8to9 SHARED ${D3D8TO9_SOURCE})
  set_target_properties(d3d8to9 PROPERTIES PREFIX "")
  set_target_properties(d3d8to9 PROPERTIES OUTPUT_NAME "d3d8")
endif()

target_compile_definitions(
  d3d8to9
  PRIVATE
    $<$<CONFIG:Debug>:D3D_DEBUG_INFO;_DEBUG>
    $<$<CONFIG:RelWithDebInfo>:D3D_DEBUG_INFO;_DEBUG>
    $<$<CONFIG:Release>:D3D8TO9NOLOG>
    $<$<CONFIG:MinSizeRel>:D3D8TO9NOLOG>
)

if(MSVC)
  target_compile_options(d3d8to9 PRIVATE /W3)
  target_link_options(d3d8to9 PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/res/d3d8.def)
else()
  target_compile_options(d3d8to9 PRIVATE -Wall -Wno-delete-non-virtual-dtor -Wno-unknown-pragmas)
  target_link_options(d3d8to9 PRIVATE -Wl,--enable-stdcall-fixup)
endif()

# psobbvr: OpenVR (vendored SDK) for the OpenVR backend. Delay-loaded so a
# missing openvr_api.dll degrades to "VR unavailable" instead of the game
# failing to start; the DLL is deployed next to the game exe.
target_include_directories(d3d8to9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr/include)
# psobbvr: OpenXR (vendored SDK) for the in-process backend; delay-loaded
# for the same reason as openvr_api.dll. dxgi for LUID-matched device
# creation.
target_include_directories(d3d8to9 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr_sdk/include)
# psobbvr: minhook (target from the parent) for the game-function hooks.
target_link_libraries(d3d8to9 d3d9 d3d11 dxgi minhook
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openvr/lib/win32/openvr_api.lib
  ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/openxr_sdk/lib/win32/openxr_loader.lib
  delayimp)
target_link_options(d3d8to9 PRIVATE /DELAYLOAD:openvr_api.dll /DELAYLOAD:openxr_loader.dll)
