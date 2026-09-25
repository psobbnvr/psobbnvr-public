cmake_minimum_required(VERSION 3.21)
project(psobbvr_mod LANGUAGES C CXX)

# PsoBB.exe is 32-bit; everything injected into it must be Win32.
# Configure with:  cmake -S mod -B mod/build -G "Visual Studio 17 2022" -A Win32
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This project must be built 32-bit (configure with -A Win32).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The git revision every log opens with, captured at configure time:
# re-run the configure step before a release build.
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE PSOBBVR_GIT_REV
                OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT PSOBBVR_GIT_REV)
    set(PSOBBVR_GIT_REV "unknown")
endif()
add_compile_definitions(PSOBBVR_GIT_REV="${PSOBBVR_GIT_REV}")

# Static CRT so the DLL has no runtime dependencies inside the game folder.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# Our fork of crosire/d3d8to9 (BSD-2): the game's d3d8.dll, translating to a
# D3D9 device we own. The resolution override, stereo and VR live here.
# Uses the minhook target above.
add_subdirectory(d3d8to9)

add_library(dinput8 SHARED
    src/dinput8/dllmain.cpp
    src/dinput8/registry_redirect.cpp
    src/dinput8/dinput_proxy.cpp
    src/dinput8/overlay.cpp
    src/common/game_memory.cpp
    src/common/log.cpp
    src/dinput8/dinput8.def
)
target_include_directories(dinput8 PRIVATE src d3d8to9/source)
target_link_libraries(dinput8 PRIVATE minhook user32 dxguid)

set_target_properties(dinput8 PROPERTIES OUTPUT_NAME "dinput8" PREFIX "")

# PSOBB VR Options, the settings program: controller bindings, the game's
# option.exe settings and the VR settings. Plain Win32, static CRT.
add_executable(psobbvr_options WIN32 bindings_editor/main.cpp)
target_include_directories(psobbvr_options PRIVATE d3d8to9/source src)
target_link_libraries(psobbvr_options PRIVATE user32 gdi32 comctl32 comdlg32 shell32 advapi32)
