cmake_minimum_required(VERSION 3.22)
project(CrimeTrainer VERSION 0.5.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CrimeTrainer is a Windows-only project.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build CrimeTrainer as x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(CrimeTrainer WIN32
    launcher/main.cpp
    launcher/Canvas.cpp
    launcher/GameLink.cpp
    launcher/Settings.cpp
)

target_include_directories(CrimeTrainer PRIVATE include launcher)
target_compile_definitions(CrimeTrainer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CrimeTrainer PRIVATE user32 gdi32 shell32)

add_library(CrimeTrainerHook SHARED
    hook/dllmain.cpp
    hook/MonoApi.cpp
    hook/MethodPatcher.cpp
    hook/GameController.cpp
)

target_include_directories(CrimeTrainerHook PRIVATE include hook)
target_compile_definitions(CrimeTrainerHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(CrimeTrainerHook PRIVATE user32)

set_target_properties(CrimeTrainer CrimeTrainerHook PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(CrimeTrainer PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(CrimeTrainerHook PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(CrimeTrainer PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(CrimeTrainerHook PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS CrimeTrainer CrimeTrainerHook
    RUNTIME DESTINATION .
    LIBRARY DESTINATION .
)
