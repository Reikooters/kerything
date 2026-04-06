// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#ifndef KERYTHING_SOCKETFRAMER_H
#define KERYTHING_SOCKETFRAMER_H

#include <QtCore/QByteArray>

#include "Protocol.h"

class SocketFramer {
public:
    void append(const QByteArray& data);
    bool tryTake(Protocol::MessageFrame& frame);
    [[nodiscard]] bool hasBufferedData() const;

private:
    QByteArray m_buffer;
};

#endif //KERYTHING_SOCKETFRAMER_H
