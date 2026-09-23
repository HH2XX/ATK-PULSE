cmake_minimum_required(VERSION 3.15)
project(atk-tray LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SOURCES
    src/main.cpp
    res/app.rc
)

add_executable(atk-tray WIN32 ${SOURCES})

target_compile_definitions(atk-tray PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(atk-tray PRIVATE
    setupapi
    hid
    user32
    gdi32
    shell32
    advapi32
)

if(MSVC)
    target_compile_options(atk-tray PRIVATE /W4 /O2 /utf-8 /MT)
    target_link_options(atk-tray PRIVATE /SUBSYSTEM:WINDOWS)
elseif(MINGW)
    target_compile_options(atk-tray PRIVATE -O3 -Wall)
    target_link_options(atk-tray PRIVATE -mwindows -municode -s -static)
endif()
