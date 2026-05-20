
cmake_minimum_required(VERSION 3.22.1)

project("menu")

add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/Dobby/libs/arm64-v8a/libdobby.a)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(imgui)
include_directories(imgui/backends)
include_directories(Includes)
include_directories(Dobby)
include_directories(Core)
include_directories(Engine)
include_directories(Dumper)
include_directories(UI)
include_directories(Font)

add_library(${CMAKE_PROJECT_NAME} SHARED
        imgui/imgui.cpp
        imgui/imgui_draw.cpp
        imgui/imgui_tables.cpp
        imgui/imgui_widgets.cpp
        imgui/backends/imgui_impl_android.cpp
        imgui/backends/imgui_impl_opengl3.cpp
        Includes/Utility.cpp
        Engine/SafeMemory.cpp
        Engine/SafeMemoryDiag.cpp
        Engine/ObjectArrayDiag.cpp
        Engine/OffsetFinder.cpp
        Engine/PatternScanner.cpp
        Engine/EnvDiag.cpp
        Core/ObjectArray.cpp
        Core/NameArray.cpp
        Engine/NameArrayDiag.cpp
        Engine/StructOffsetFinder.cpp
        Engine/StructOffsetDiag.cpp
        Dumper/SDKDumper.cpp
        Dumper/DumperUI.cpp
        UI/MainPanel.cpp
        native-lib.cpp)

set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden)

target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -s)

target_link_libraries(${CMAKE_PROJECT_NAME}
        dobby
        EGL
        GLESv3
        android
        log)
