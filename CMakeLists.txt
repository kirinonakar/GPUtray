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
    src/StartupTask.cpp
    src/resources.rc
    src/GpuTray.manifest
)

add_executable(${PROJECT_NAME} WIN32 ${SOURCES})

# MSVC security hardening and deterministic release builds
if(MSVC)
    # Keep the static CRT so the portable executable has no VC runtime dependency.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(${PROJECT_NAME} PRIVATE 
        /GS /guard:cf /sdl /ZH:SHA_256 /utf-8
        $<$<CONFIG:Release>:/O2 /GL /Gy /Brepro>
    )
    target_link_options(${PROJECT_NAME} PRIVATE 
        /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /GUARD:CF /OPT:REF /OPT:ICF
        $<$<CONFIG:Release>:/LTCG /INCREMENTAL:NO /Brepro>
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
    advapi32.lib
    secur32.lib
    taskschd.lib
)

# Release distributors can provide a trusted Authenticode certificate thumbprint
# without storing certificate material in this repository. build.bat also honors
# the GPUTRAY_SIGN_CERT_THUMBPRINT environment variable through this cache default.
set(GPUTRAY_SIGN_CERT_THUMBPRINT "$ENV{GPUTRAY_SIGN_CERT_THUMBPRINT}" CACHE STRING
    "SHA-1 thumbprint of the Authenticode certificate used to sign GpuTray.exe")
if(MSVC AND GPUTRAY_SIGN_CERT_THUMBPRINT)
    find_program(GPUTRAY_SIGNTOOL_EXECUTABLE signtool)
    if(NOT GPUTRAY_SIGNTOOL_EXECUTABLE)
        message(FATAL_ERROR "GPUTRAY_SIGN_CERT_THUMBPRINT is set, but signtool.exe was not found")
    endif()
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND "${GPUTRAY_SIGNTOOL_EXECUTABLE}" sign /fd SHA256
                /sha1 "${GPUTRAY_SIGN_CERT_THUMBPRINT}"
                /tr "http://timestamp.digicert.com" /td SHA256
                "$<TARGET_FILE:${PROJECT_NAME}>"
        COMMENT "Authenticode-signing GpuTray.exe"
        VERBATIM
    )
endif()

# Use Unicode
add_definitions(-DUNICODE -D_UNICODE)
