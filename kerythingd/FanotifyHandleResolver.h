// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHINGD_FANOTIFYHANDLERESOLVER_H
#define KERYTHINGD_FANOTIFYHANDLERESOLVER_H

#include <QByteArray>
#include <QString>
#include <QtGlobal>

struct ResolvedFanotifyHandle {
    bool ok = false;

    quint64 inode = 0;
    quint64 size = 0;
    qint64 modificationTime = 0;
    quint32 mode = 0;
    bool isDirectory = false;
    bool isSymlink = false;

    QString errorText;
};

class FanotifyHandleResolver {
public:
    explicit FanotifyHandleResolver(QString mountPoint);
    ~FanotifyHandleResolver();

    [[nodiscard]] ResolvedFanotifyHandle resolveObjectHandle(
        const QByteArray& handleBytes,
        qint32 handleType
    ) const;

    [[nodiscard]] ResolvedFanotifyHandle resolveChildByParentHandleAndName(
        const QByteArray& parentHandleBytes,
        qint32 parentHandleType,
        const QString& name
    ) const;

private:
    [[nodiscard]] int openMountFd(QString* errorText = nullptr) const;
    [[nodiscard]] int openHandle(
        const QByteArray& handleBytes,
        qint32 handleType,
        QString* errorText = nullptr
    ) const;

    QString mountPoint_;
    mutable int mountFd_ = -1;
};

#endif // KERYTHINGD_FANOTIFYHANDLERESOLVER_H