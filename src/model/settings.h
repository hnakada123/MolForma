// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QSettings>

namespace mf {
// The format of the user's preferences, QSettings(SettingsFormat, QSettings::UserScope, "MolForma",
// "MolForma"). Windows and macOS keep them in an INI file (under %APPDATA%; main() puts it under
// ~/Library/Preferences on macOS) instead of the registry or CFPreferences, which ignore
// QSettings::setPath, so that setPath moves them too (tests, XDG_CONFIG_HOME in main()).
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
constexpr auto SettingsFormat = QSettings::IniFormat;
#else
constexpr auto SettingsFormat = QSettings::NativeFormat;
#endif
} // namespace mf
