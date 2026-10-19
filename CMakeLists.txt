cmake_minimum_required(VERSION 3.20)
project(PictogramParser VERSION 1.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(PictogramParser WIN32
    src/main.cpp
    src/image_proc.cpp
    src/image_proc.h
    src/image_io.cpp
    src/image_io.h
    res/app.rc
)

# Rebuild the resource object when the icon or manifest changes.
set_source_files_properties(res/app.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest")

target_compile_definitions(PictogramParser PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0601)

if(MINGW)
    target_compile_options(PictogramParser PRIVATE -Wall -Wextra -Wno-unused-parameter -municode)
    # -municode selects wWinMain; -static avoids shipping libstdc++/libgcc DLLs.
    target_link_options(PictogramParser PRIVATE -municode -static)
endif()

target_link_libraries(PictogramParser PRIVATE
    gdiplus comctl32 comdlg32 msimg32 shell32 ole32 gdi32 user32)
