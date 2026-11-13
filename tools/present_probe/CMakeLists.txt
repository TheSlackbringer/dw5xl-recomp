# DW5XL-DEBUG (issue 049, P2.9 step 1): a test window, not the game. A project of its own with a build folder of its
# own, so the game's program is not rebuilt by it. See README.md.
cmake_minimum_required(VERSION 3.20)
project(dw5xl_present_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(DW5XL_PARALLEL_GS_DIR "" CACHE PATH "The paraLLEl-GS checkout the game is built with (PS2X_PARALLEL_GS_SOURCE_DIR)")
set(DW5XL_GLFW_DIR "" CACHE PATH "GLFW 3.4 sources: raylib's src/external/glfw")
if(NOT EXISTS "${DW5XL_PARALLEL_GS_DIR}/gs/gs_interface.cpp")
    message(FATAL_ERROR "Set DW5XL_PARALLEL_GS_DIR to the paraLLEl-GS checkout the game is built with")
endif()
if(NOT EXISTS "${DW5XL_GLFW_DIR}/include/GLFW/glfw3.h")
    message(FATAL_ERROR "Set DW5XL_GLFW_DIR to raylib's src/external/glfw")
endif()

# The renderer library, set up as the game's build does (ps2xRuntime/cmake/VulkanGs.cmake), with one difference:
# on Windows the presenter's way through DXGI is compiled in, so that the probe can try both.
set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
set(GRANITE_TOOLS OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERDOC_CAPTURE OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
if(WIN32)
    set(GRANITE_VULKAN_DXGI_INTEROP ON CACHE BOOL "" FORCE)
else()
    set(GRANITE_VULKAN_DXGI_INTEROP OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory("${DW5XL_PARALLEL_GS_DIR}" "${CMAKE_BINARY_DIR}/parallel-gs" EXCLUDE_FROM_ALL)

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DW5XL_GLFW_DIR}" "${CMAKE_BINARY_DIR}/glfw" EXCLUDE_FROM_ALL)

add_executable(dw5xl_present_probe present_probe.cpp)
target_link_libraries(dw5xl_present_probe PRIVATE parallel-gs glfw)
if(WIN32)
    target_compile_definitions(dw5xl_present_probe PRIVATE NOMINMAX)
    target_link_libraries(dw5xl_present_probe PRIVATE winmm dwmapi)
endif()
