// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_FILESYSTEMCONSTANTS_H
#define KERYTHING_FILESYSTEMCONSTANTS_H

#include <QtGlobal>

namespace FilesystemConstants {

    /*
     * Btrfs root-directory object id. (BTRFS_FIRST_FREE_OBJECTID)
     *
     * Btrfs object ids are only unique within a root/subvolume. The root directory
     * of a Btrfs root/subvolume is conventionally object id 256.
     */
    inline constexpr quint64 BtrfsFirstFreeObjectId = 256;

} // namespace FilesystemConstants

#endif // KERYTHING_FILESYSTEMCONSTANTS_H