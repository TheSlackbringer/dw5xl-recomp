# Issue 054: the ioplift CTest's build-time tools, built for the build machine. A cross build (Windows on Linux, CI's
# windows-build) cannot run the programs it builds, so it builds this project with the build machine's own compiler
# and runs these copies instead (app/CMakeLists.txt). Same sources as the app's dw5xl_ioplift and
# dw5xl_ioplift_testgen; only ps2xIOP besides the standard library.
cmake_minimum_required(VERSION 3.21)
project(dw5xl_host_tools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DW5XL_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PS2RECOMP_DIR "${DW5XL_APP_DIR}/../external/PS2Recomp")
add_subdirectory("${PS2RECOMP_DIR}/ps2xIOP" ps2xIOP EXCLUDE_FROM_ALL)

add_executable(dw5xl_ioplift "${DW5XL_APP_DIR}/setup/ioplift/ioplift_main.cpp"
    "${DW5XL_APP_DIR}/setup/ioplift/iop_translator.cpp" "${DW5XL_APP_DIR}/setup/ioplift/iop_generate.cpp")
target_include_directories(dw5xl_ioplift PRIVATE "${DW5XL_APP_DIR}/setup/ioplift" "${PS2RECOMP_DIR}/ps2xIOP/include"
    "${PS2RECOMP_DIR}/ps2xIOP/src")
target_link_libraries(dw5xl_ioplift PRIVATE ps2_iop)

add_executable(dw5xl_ioplift_testgen "${DW5XL_APP_DIR}/tests/ioplift_testgen.cpp")
