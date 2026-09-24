cmake_minimum_required(VERSION 3.16)
project(IOMonitor VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE)

# The S.M.A.R.T. layer needs the Windows 10 API surface: the NVMe storage
# protocol query and StorageAdapterProtocolSpecificProperty are only declared
# for _WIN32_WINNT >= 0x0A00.  Without this MinGW compiles against the oldest
# API set it supports and the declarations go missing.
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

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
    src/IopsMonitor.cpp
    src/SmartDataModel.cpp
    # CrystalDiskInfo S.M.A.R.T. port (replaces the smartmontools readers)
    src/CdiSmart.cpp
    src/CdiSmartSupport.cpp
    src/CdiSmartIo.cpp
    src/CdiSmartFill.cpp
    src/CdiSmartSsd.cpp
    src/CdiSmartStatus.cpp
    src/CdiSmartNvmeInterp.cpp
    src/CdiAttributeName.cpp
    src/CdiSmartAttributeTable.cpp
    # GUI-facing data model + adapter
    src/SmartCdiAdapter.cpp
    src/SmartMonitor.cpp
    src/SmartOverlayWindow.cpp
    src/SmartDebugWindow.cpp
)

set(HEADERS
    src/Monitor.h
    src/Display.h
    src/Recorder.h
    src/OverlayWindow.h
    src/IopsMonitor.h
    src/SmartDataModel.h
    src/CdiSmart.h
    src/CdiSmartDetail.h
    src/CdiAttributeName.h
    src/SmartCdiAdapter.h
    src/SmartMonitor.h
    src/SmartOverlayWindow.h
    src/SmartDebug.h
    src/SmartDebugWindow.h
)

add_executable(iomonitor ${SOURCES} ${HEADERS})
target_include_directories(iomonitor PRIVATE src)
target_link_libraries(iomonitor PRIVATE pdh psapi setupapi)

set_target_properties(iomonitor PROPERTIES
    WIN32_EXECUTABLE OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC AND CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(iomonitor PRIVATE /O2 /GL)
    target_link_options(iomonitor PRIVATE /LTCG)
endif()
