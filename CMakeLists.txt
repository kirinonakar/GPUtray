cmake_minimum_required(VERSION 3.10)
project(GpuTray)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
set(SOURCES
    src/main.cpp
    src/GpuMonitor.cpp
    src/TrayIcon.cpp
    src/GraphPopup.cpp
    src/resources.rc
    src/GpuTray.manifest
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

# MSVC Specific hardening flags to reduce false positives
if(MSVC)
    # /MT: Static CRT, /GS: Stack protection, /guard:cf: Control flow guard, /ZH:SHA_256: Hashing
    # /GL: Whole Program Optimization, /O2: Speed
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${PROJECT_NAME} PRIVATE 
        /GS /guard:cf /ZH:SHA_256
        $<$<CONFIG:Release>:/O2 /Oi /GL /Gy>
    )
    target_link_options(${PROJECT_NAME} PRIVATE 
        /DYNAMICBASE /NXCOMPAT /GUARD:CF /OPT:REF /OPT:ICF
        $<$<CONFIG:Release>:/LTCG /INCREMENTAL:NO>
    )
endif()

# Include directory
target_include_directories(${PROJECT_NAME} PRIVATE include)

# Libraries
target_link_libraries(${PROJECT_NAME} PRIVATE
    pdh.lib
    user32.lib
    gdi32.lib
    shell32.lib
    gdiplus.lib
    ole32.lib
    oleaut32.lib
    wbemuuid.lib
    dxgi.lib
    d3d11.lib
)

# Use Unicode
add_definitions(-DUNICODE -D_UNICODE)
