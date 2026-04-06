// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026  Reikooters <https://github.com/Reikooters>

#include "SocketFramer.h"

void SocketFramer::append(const QByteArray& data)
{
    m_buffer.append(data);
}

bool SocketFramer::tryTake(Protocol::MessageFrame& frame)
{
    return Protocol::tryParseFrame(m_buffer, frame);
}

bool SocketFramer::hasBufferedData() const
{
    return !m_buffer.isEmpty();
}