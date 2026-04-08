# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

cmake_minimum_required(VERSION 4.2)
project(kerything)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt and KDE require these for code generation (MOC, UI, Resources)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS
        Widgets
        Core
        Network
)

# Find KDE Frameworks components
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

add_executable(kerything
        kerything/main.cpp
        kerything/AppController.cpp
        kerything/AppController.h
        kerything/DaemonClient.cpp
        kerything/DaemonClient.h
        kerything/MainWindow.cpp
        kerything/MainWindow.h
        kerything/SingleInstanceServer.cpp
        kerything/SingleInstanceServer.h
        shared/FileRecord.h
        shared/Protocol.h
        shared/SocketFramer.cpp
        shared/SocketFramer.h
)

add_executable(kerythingd
        kerythingd/main.cpp
        kerythingd/ClientConnection.cpp
        kerythingd/ClientConnection.h
        kerythingd/ScanJob.h
        kerythingd/ScannerHelper.cpp
        kerythingd/ScannerHelper.h
        kerythingd/ScannerWorker.cpp
        kerythingd/ScannerWorker.h
        kerythingd/Server.cpp
        kerythingd/Server.h
        kerythingd/scanners/NtfsScannerEngine.cpp
        kerythingd/scanners/NtfsScannerEngine.h
        shared/FileRecord.h
        shared/Protocol.h
        shared/SocketFramer.cpp
        shared/SocketFramer.h
)

target_link_libraries(kerything
        PRIVATE
        Qt6::Widgets
        Qt6::Network
)

target_link_libraries(kerythingd
        PRIVATE
        Qt6::Core
        Qt6::Network
)

target_include_directories(kerything PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_include_directories(kerythingd PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared")

include(KDEInstallDirs)
include(ECMInstallIcons)

# Install the binaries
install(TARGETS kerything DESTINATION ${KDE_INSTALL_BINDIR})

# Install the icon
# KDE looks for icons in specific size folders (hicolor theme)
ecm_install_icons(ICONS kerything/icons/256-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS kerything/icons/48-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS kerything/icons/32-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS kerything/icons/16-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)

# Install the .desktop file
install(PROGRAMS net.reikooters.kerything.desktop DESTINATION ${KDE_INSTALL_APPDIR})

# Install the license file
install(FILES LICENSE DESTINATION share/licenses/kerything)

# Add an uninstall target
if(NOT TARGET uninstall)
    configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
            IMMEDIATE @ONLY)

    add_custom_target(uninstall
            COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
endif()
