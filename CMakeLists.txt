cmake_minimum_required(VERSION 3.16)
project(rta VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Multimedia)
qt_standard_project_setup()

qt_add_executable(rta src/main.cpp)
target_link_libraries(rta PRIVATE Qt6::Widgets Qt6::Multimedia)

if(WIN32)
    # GUI subsystem (no console window). Note: `rta.exe --selftest` output is
    # only visible when redirected, e.g. `rta.exe --selftest > out.txt`.
    set_target_properties(rta PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

if(APPLE)
    set_target_properties(rta PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in")
endif()
