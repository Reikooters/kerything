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

# Find TBB for parallel algorithms
find_package(TBB REQUIRED)

# Find Qt6 components
find_package(Qt6 REQUIRED COMPONENTS
        Widgets
        Core
        Network
)

option(KERYTHING_WITH_KF6 "Use KDE Frameworks 6, including KDE KIO for opening files" ON)

if(KERYTHING_WITH_KF6)
    # Find KDE Frameworks components
    find_package(ECM REQUIRED NO_MODULE)
    set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

    find_package(KF6 REQUIRED COMPONENTS
            CoreAddons
            I18n
            KIO
            XmlGui
    )
endif()

# Find PkgConfig
find_package(PkgConfig REQUIRED)

# Check for both ext2fs and com_err
pkg_check_modules(EXT2FS REQUIRED ext2fs com_err)

# libblkid (util-linux) for device inventory in the daemon
pkg_check_modules(BLKID REQUIRED blkid)

# libudev for monitoring device changes (USB plugged in,
# partitions appearing/disappearing, filesystem label changes, etc.
pkg_check_modules(UDEV REQUIRED libudev)

# libmount (util-linux) for monitoring mount table changes
pkg_check_modules(MOUNT REQUIRED mount)

add_executable(kerything
        kerything/main.cpp
        kerything/AppController.cpp
        kerything/AppController.h
        kerything/DaemonClient.cpp
        kerything/DaemonClient.h
        kerything/DevicePickerDialog.cpp
        kerything/DevicePickerDialog.h
        kerything/DevicePreferenceChange.h
        kerything/FileModel.cpp
        kerything/FileModel.h
        kerything/GuiUtils.cpp
        kerything/GuiUtils.h
        kerything/IndexController.cpp
        kerything/IndexController.h
        kerything/MainWindow.cpp
        kerything/MainWindow.h
        kerything/Preferences.cpp
        kerything/Preferences.h
        kerything/PreferencesDialog.cpp
        kerything/PreferencesDialog.h
        kerything/SearchResultTableView.cpp
        kerything/SearchResultTableView.h
        kerything/SingleInstanceServer.cpp
        kerything/SingleInstanceServer.h
        shared/BlockDevice.h
        shared/FileRecord.h
        shared/Protocol.h
        shared/SocketFramer.cpp
        shared/SocketFramer.h
)

add_executable(kerythingd
        kerythingd/main.cpp
        kerythingd/BlockDeviceHelper.cpp
        kerythingd/BlockDeviceHelper.h
        kerythingd/ClientConnection.cpp
        kerythingd/ClientConnection.h
        kerythingd/DeviceChangeMonitor.cpp
        kerythingd/DeviceChangeMonitor.h
        kerythingd/ScanJob.h
        kerythingd/ScannerHelper.cpp
        kerythingd/ScannerHelper.h
        kerythingd/ScannerWorker.cpp
        kerythingd/ScannerWorker.h
        kerythingd/Server.cpp
        kerythingd/Server.h
        kerythingd/ThrottledProgressReporter.h
        kerythingd/scanners/Ext4ScannerEngine.cpp
        kerythingd/scanners/Ext4ScannerEngine.h
        kerythingd/scanners/NtfsScannerEngine.cpp
        kerythingd/scanners/NtfsScannerEngine.h
        shared/BlockDevice.h
        shared/FileRecord.h
        shared/Protocol.h
        shared/SocketFramer.cpp
        shared/SocketFramer.h
)

if(KERYTHING_WITH_KF6)
    target_compile_definitions(kerything PRIVATE KERYTHING_WITH_KF6)
endif()

target_compile_definitions(kerything PRIVATE QT_NO_KEYWORDS)

target_compile_definitions(kerythingd PRIVATE QT_NO_KEYWORDS)

target_link_libraries(kerything
        PRIVATE
        TBB::tbb # Parallelism
        Qt6::Widgets
        Qt6::Network
)

if(KERYTHING_WITH_KF6)
    target_link_libraries(kerything
            PRIVATE
            KF6::CoreAddons
            KF6::KIOGui
            KF6::Service
            KF6::XmlGui
    )
endif()

target_link_libraries(kerythingd
        PRIVATE
        Qt6::Core
        Qt6::Network
        ${BLKID_LIBRARIES}
        ${EXT2FS_LIBRARIES} # Lib for parsing EXT4 inodes
        ${UDEV_LIBRARIES}
        ${MOUNT_LIBRARIES}
)

target_include_directories(kerything PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_include_directories(kerythingd
        PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/shared"
        ${EXT2FS_INCLUDE_DIRS}
        ${UDEV_INCLUDE_DIRS}
        ${MOUNT_INCLUDE_DIRS}
)

target_compile_options(kerythingd
        PRIVATE
        ${EXT2FS_CFLAGS_OTHER}
        ${BLKID_CFLAGS_OTHER}
        ${UDEV_CFLAGS_OTHER}
        ${MOUNT_CFLAGS_OTHER}
)

target_link_options(kerythingd
        PRIVATE
        ${EXT2FS_LDFLAGS_OTHER}
        ${BLKID_LDFLAGS_OTHER}
        ${UDEV_LDFLAGS_OTHER}
        ${MOUNT_LDFLAGS_OTHER}
)

if(KERYTHING_WITH_KF6)
    include(KDEInstallDirs)
    include(ECMInstallIcons)
endif()

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
