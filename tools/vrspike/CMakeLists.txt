# vrspike: Phase 0 of the VR support - a 32-bit OpenXR + Direct3D 11 session
# on the installed runtime (main.cpp).  Built with the tools
# (-DGT3PC_BUILD_TOOLS=ON) or alone.
cmake_minimum_required(VERSION 3.25)
if(NOT PROJECT_NAME)
    project(vrspike LANGUAGES C CXX)
    set(GT3PC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
    set(CMAKE_CXX_STANDARD 17)
endif()
include(${GT3PC_ROOT}/cmake/openxr.cmake)
add_executable(vrspike main.cpp)
target_link_libraries(vrspike PRIVATE gt3_openxr d3d11 dxgi)
add_custom_command(TARGET vrspike POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:OpenXR::openxr_loader> $<TARGET_FILE_DIR:vrspike>)
