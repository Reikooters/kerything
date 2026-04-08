// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_FILERECORD_H
#define KERYTHING_FILERECORD_H

#pragma pack(push, 1)

struct FileRecord {
    quint64 fsIndex;         // File System Index (MFT Index / Inode Number)
    quint32 parentRecordIdx; // Index in the 'records' vector (NOT MFT index)
    quint64 size;
    quint64 modificationTime;
    quint32 nameOffset;      // Offset into the global string pool
    quint16 nameLen;
    quint8 flags;            // bit 0 = isDir, bit 1 = isSymlink
};

#pragma pack(pop)

inline constexpr quint8 FileRecord_IsDir     = 0x01;
inline constexpr quint8 FileRecord_IsSymlink = 0x02;

#endif // KERYTHING_FILERECORD_H