cmake_minimum_required(VERSION 3.21)
project(Ksipl VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Core Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core Widgets Network)

add_library(ksipl_core
    src/protocol.cpp src/protocol.h
    src/dictionary.cpp src/dictionary.h
    src/completion.cpp src/completion.h
    src/service.cpp src/service.h
    src/client.cpp src/client.h
)
target_link_libraries(ksipl_core PUBLIC Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Network)
target_include_directories(ksipl_core PUBLIC src)
add_library(ksipl_ui src/editor.cpp src/editor.h src/window.cpp src/window.h)
target_link_libraries(ksipl_ui PUBLIC ksipl_core Qt${QT_VERSION_MAJOR}::Widgets)
add_executable(ksipl WIN32 src/main.cpp)
target_link_libraries(ksipl PRIVATE ksipl_ui)
if(MSVC)
    target_compile_options(ksipl_core PUBLIC /utf-8 /W4)
else()
    target_compile_options(ksipl_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(ksipl_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Test)
    add_executable(ksipl_tests tests/tests.cpp)
    target_link_libraries(ksipl_tests PRIVATE ksipl_ui Qt${QT_VERSION_MAJOR}::Test)
    target_compile_definitions(ksipl_tests PRIVATE KSIPL_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(ksipl_tests ksipl)
    add_test(NAME ksipl_tests COMMAND ksipl_tests -platform offscreen -o "${CMAKE_CURRENT_BINARY_DIR}/test-results.txt,txt")
    set_tests_properties(ksipl_tests PROPERTIES TIMEOUT 90)
endif()

install(TARGETS ksipl RUNTIME DESTINATION bin)
install(DIRECTORY dicts DESTINATION bin)
