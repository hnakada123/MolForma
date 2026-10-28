// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSettings>

namespace mf {
// The format of the user's preferences, QSettings(SettingsFormat, QSettings::UserScope, "MolForma",
// "MolForma"). Windows keeps them in an INI file under %APPDATA% instead of the registry, so that
// QSettings::setPath moves them there too (tests, XDG_CONFIG_HOME in main()).
#ifdef Q_OS_WIN
constexpr auto SettingsFormat = QSettings::IniFormat;
#else
constexpr auto SettingsFormat = QSettings::NativeFormat;
#endif
} // namespace mf
