// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>
#include <stdexcept>
namespace mf {
[[noreturn]] inline void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}
} // namespace mf
