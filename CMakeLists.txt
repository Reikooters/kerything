# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

cmake_minimum_required(VERSION 4.2)
project(kerything)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt requires these for code generation (MOC, UI, Resources)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS
        Widgets
        Network
)

add_executable(kerything
        kerything/main.cpp
        kerything/AppController.cpp
        kerything/AppController.h
        kerything/MainWindow.cpp
        kerything/MainWindow.h
        kerything/SingleInstanceServer.cpp
        kerything/SingleInstanceServer.h
)

target_link_libraries(kerything
        PRIVATE
        Qt6::Widgets
        Qt6::Network
)