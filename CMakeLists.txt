# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

cmake_minimum_required(VERSION 4.1)
project(kerything)

set(CMAKE_CXX_STANDARD 26)

# Qt and KDE require these for code generation (MOC, UI, Resources)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find TBB for parallel algorithms
find_package(TBB REQUIRED)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# Find KDE Frameworks components
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

find_package(KF6 REQUIRED COMPONENTS
        WidgetsAddons
        CoreAddons
        Solid
        KIO
        XmlGui
)

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Check for both ext2fs and com_err
pkg_check_modules(EXT2FS REQUIRED ext2fs com_err)

# 1. THE HELPER (The disk scanner - runs as root via pkexec)
add_executable(kerything-scanner-helper
        main_helper.cpp
        ScannerEngine.h
        scanners/NtfsScannerEngine.cpp
        scanners/NtfsScannerEngine.h
        scanners/Ext4ScannerEngine.cpp
        scanners/Ext4ScannerEngine.h
        ScannerUtils.cpp
        ScannerUtils.h
        Version.h
)

# Link the libraries and include directories found by pkg-config
target_link_libraries(kerything-scanner-helper
        PRIVATE
        ${EXT2FS_LIBRARIES} # Lib for parsing EXT4 inodes
)
target_include_directories(kerything-scanner-helper PRIVATE ${EXT2FS_INCLUDE_DIRS})

# 2. THE GUI (The user interface - runs as normal user)
add_executable(kerything
        main.cpp
        FileModel.cpp
        FileModel.h
        GuiUtils.cpp
        GuiUtils.h
        MainWindow.cpp
        MainWindow.h
        PartitionDialog.cpp
        PartitionDialog.h
        ScannerEngine.h # Need the structs for the DB
        ScannerManager.cpp
        ScannerManager.h
        Version.h
)

target_link_libraries(kerything
        PRIVATE
        TBB::tbb # Parallelism
        Qt6::Widgets # GUI widgets
        KF6::WidgetsAddons # TODO: Check whether this can be removed
        KF6::CoreAddons # TODO: Check whether this can be removed
        KF6::Solid # For querying list of partitions
        KF6::KIOWidgets # TODO: Check whether this can be removed
        KF6::KIOFileWidgets # For KDE right-click context menu
        KF6::KIOGui # TODO: Check whether this can be removed
        KF6::XmlGui # for KDE About dialog
)

include(KDEInstallDirs)
include(ECMInstallIcons)

# Install the binaries
install(TARGETS kerything kerything-scanner-helper DESTINATION ${KDE_INSTALL_BINDIR})

# Install the icon
# KDE looks for icons in specific size folders (hicolor theme)
ecm_install_icons(ICONS icons/256-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS icons/48-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS icons/32-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)
ecm_install_icons(ICONS icons/16-apps-kerything.png DESTINATION ${KDE_INSTALL_ICONDIR} THEME hicolor)

# Install the .desktop file
install(PROGRAMS net.reikooters.kerything.desktop DESTINATION ${KDE_INSTALL_APPDIR})

# Install the Polkit policy
install(FILES net.reikooters.kerything.policy DESTINATION ${KDE_INSTALL_DATADIR}/polkit-1/actions)

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