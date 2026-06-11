cmake_minimum_required(VERSION 3.18)
project(MotionBlur LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_ANDROID_STL_TYPE c++_shared)

cmake_policy(SET CMP0169 OLD)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -fexceptions -w")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -fvisibility=hidden -ffunction-sections -fdata-sections -w")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections,--strip-all -s")

include(FetchContent)
FetchContent_Declare(
    preloader
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG main
    SOURCE_SUBDIR "nonexistent"
)
FetchContent_Populate(preloader)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ImGui
    ${CMAKE_SOURCE_DIR}/src/ImGui/backends
    ${preloader_SOURCE_DIR}/src
)

set(SOURCES
    src/main.cpp
    src/ImGui/imgui.cpp
    src/ImGui/imgui_draw.cpp
    src/ImGui/imgui_tables.cpp
    src/ImGui/imgui_widgets.cpp
    src/ImGui/backends/imgui_impl_opengl3.cpp
    src/ImGui/backends/imgui_impl_android.cpp
)

add_library(MotionBlur SHARED ${SOURCES})

target_link_libraries(MotionBlur
    ${preloader_SOURCE_DIR}/lib/ARM64/libGlossHook.a
    log android EGL GLESv3 GLESv2
    c++_shared
)
