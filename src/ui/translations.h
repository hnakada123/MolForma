// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
class QCoreApplication;
namespace mf {
// MolForma's interface is Japanese or English (model/i18n.h): MOLFORMA_LANGUAGE ("ja" or "en") if
// set, else the language chosen in Preferences ("ui/language"), else the system's language.
// Qt's own dialogs and standard buttons follow it: in Japanese, Qt's qtbase translation is used
// when installed and built-in texts cover the standard buttons without it. The choice is put in
// MOLFORMA_LANGUAGE for the worker processes.
void installTranslations(QCoreApplication &app);
// Worker processes speak the language their parent passed down, Japanese without one.
void inheritTranslations();
} // namespace mf
