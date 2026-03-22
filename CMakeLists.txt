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
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

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
