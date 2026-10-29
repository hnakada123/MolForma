#pragma once
#include "model/document.h"
#include <optional>

namespace mf {
inline constexpr int MinimumAtomCharge = -99, MaximumAtomCharge = 20, MaximumAtomRadical = 8;
// Absent fields stay unchanged. isotope=0 clears an isotope/custom mass; mass must be positive.
// File readers retain their compatibility rules; all interactive edits use these rules.
struct AtomProperties {
    std::optional<int> charge, radical, isotope;
    std::optional<double> mass;
    std::optional<bool> ghost, fixed;
    std::optional<QString> name;
};
AtomProperties atomMassInput(const QString &text);
Atom withAtomProperties(Atom atom, const AtomProperties &properties);
// Validate the whole selection before changing any atom.
void setAtomProperties(Document &document, const Ids &ids, const AtomProperties &properties);
} // namespace mf
