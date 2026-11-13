// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_advanced.h"
#include "test_support.h"

// Calculation settings of a document, for the tests of NWChem input (test_nwchem) and of the screens that
// edit it (test_calculation, test_advanced, test_input_preview, test_output).

// Sets one of the document's NWChem settings as stored, without a check.
inline void set(mf::Document &d, const QString &key, const QJsonValue &value) {
    auto s = d.data["nwchem"].toObject();
    s[key] = value;
    d.data["nwchem"] = s;
}
// The same, as the tests of the advanced options and of the output files name it.
inline void base(mf::Document &d, const QString &key, const QJsonValue &value) {
    set(d, key, value);
}
inline bool errorContains(const mf::Document &d, const QString &text) {
    return mf::validateNwchem(d).errors.join('\n').contains(text);
}
inline const mf::AdvancedOption &option(const QString &key) {
    for (const auto &o : mf::nwchemAdvancedOptions())
        if (o.key == key)
            return o;
    qFatal("Unknown option");
}
// Sets advanced options.
inline void values(mf::Document &d, const QJsonObject &a) {
    for (auto it = a.begin(); it != a.end(); ++it)
        mf::setNwchemAdvanced(d, it.key(), it.value());
}
// Asks for both kinds of output files.
inline void both(mf::Document &d) {
    mf::setNwchemAdvanced(d, "cube_output", true);
    mf::setNwchemAdvanced(d, "molden_output", true);
}
