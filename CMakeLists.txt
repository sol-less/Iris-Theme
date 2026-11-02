cmake_minimum_required(VERSION 3.20)
project(iris-theme LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui)
qt_standard_project_setup()

qt_add_executable(iris-theme
    src/main.cpp
    src/color.cpp src/color.h
    src/toml.cpp src/toml.h
    src/render.cpp src/render.h
)
target_link_libraries(iris-theme PRIVATE Qt6::Core Qt6::Gui)

install(TARGETS iris-theme RUNTIME DESTINATION bin)