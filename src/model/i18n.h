// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

namespace mf {
// User-visible text is written in Japanese, the source language: as tr("…") where it is used, or
// QT_TR_NOOP("…") in tables shown through translated(). The English catalog
// (assets/translations/en.json) maps each source text to its translation; test_translations
// checks that it covers every marked text and that no Japanese literal is left unmarked.
// tr() has the signature of QObject::tr, which the catalog serves alike: a plural form is picked
// by n, and %n becomes n.
QString tr(const char *source, const char *disambiguation = nullptr, int n = -1);
QString translated(const QString &source);
// Whether the interface is English. The language is chosen once at startup, before any window
// is built (ui/translations.h); widgets and cached texts are not translated again.
bool english();
// Installs or removes the English catalog in the running application.
void setEnglish(bool on);
} // namespace mf
