# OpenXR (Apache-2.0 / MIT), the VR support's one third-party library
# (src/platform/vr.cpp).  The Khronos OpenXR-SDK source of a pinned release
# is downloaded once into third_party/, checked against its SHA-256, and its
# loader is built here as a 32-bit DLL (the game is 32-bit x86: the loader
# finds the runtime registered for 32-bit programs - SteamVR registers one
# beside its 64-bit one).  gt3pc.exe delay-loads openxr_loader.dll, copied
# next to it, and runs without VR when the DLL or a runtime is missing.
set(GT3PC_OPENXR_VERSION 1.1.65)
set(GT3PC_OPENXR_SHA256 6271009b5295563eaf290a09e7deefe1a31d04357154e36e4e8ee067be263b45)
set(GT3PC_OPENXR_DIR ${GT3PC_ROOT}/third_party/OpenXR-SDK-release-${GT3PC_OPENXR_VERSION})

if(NOT TARGET gt3_openxr)
    if(NOT EXISTS ${GT3PC_OPENXR_DIR}/CMakeLists.txt)
        set(zip ${CMAKE_BINARY_DIR}/OpenXR-SDK-${GT3PC_OPENXR_VERSION}.zip)
        message(STATUS "Downloading OpenXR-SDK ${GT3PC_OPENXR_VERSION} for VR")
        file(DOWNLOAD
            https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-${GT3PC_OPENXR_VERSION}.zip
            ${zip} EXPECTED_HASH SHA256=${GT3PC_OPENXR_SHA256} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "OpenXR-SDK ${GT3PC_OPENXR_VERSION} could not be downloaded: ${status}")
        endif()
        file(ARCHIVE_EXTRACT INPUT ${zip} DESTINATION ${GT3PC_ROOT}/third_party)
        file(REMOVE ${zip})
    endif()

    # Only the loader: no API layers, tests or sample programs.
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_ALL_EXTENSIONS OFF CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER ON CACHE BOOL "" FORCE)
    add_subdirectory(${GT3PC_OPENXR_DIR} ${CMAKE_BINARY_DIR}/openxr EXCLUDE_FROM_ALL)

    add_library(gt3_openxr INTERFACE)
    target_link_libraries(gt3_openxr INTERFACE OpenXR::headers OpenXR::openxr_loader delayimp)
    target_link_options(gt3_openxr INTERFACE /DELAYLOAD:openxr_loader.dll)
endif()
