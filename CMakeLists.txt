cmake_minimum_required(VERSION 3.16)
project(IOMonitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE)

if(MSVC)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    add_compile_options(/utf-8)
elseif(MINGW)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
    add_link_options(-municode -static-libgcc -static-libstdc++)
endif()

set(SOURCES
    src/main.cpp
    src/Monitor.cpp
    src/Display.cpp
    src/Recorder.cpp
    src/OverlayWindow.cpp
)

set(HEADERS
    src/Monitor.h
    src/Display.h
    src/Recorder.h
    src/OverlayWindow.h
)

add_executable(iomonitor ${SOURCES} ${HEADERS})
target_include_directories(iomonitor PRIVATE src)
target_link_libraries(iomonitor PRIVATE pdh psapi)

set_target_properties(iomonitor PROPERTIES
    WIN32_EXECUTABLE OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(iomonitor PRIVATE /O2 /GL)
    target_link_options(iomonitor PRIVATE /LTCG)
endif()
