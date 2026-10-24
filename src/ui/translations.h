// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class QCoreApplication;
namespace mf {
// MolForma's interface is Japanese: Qt's own dialogs and standard buttons follow it. Qt's qtbase
// translation is used when installed; built-in texts cover the standard buttons without it.
void installTranslations(QCoreApplication &app);
} // namespace mf
