// SPDX-License-Identifier: GPL-2.0-or-later
#include "nwchem/nwchem_catalog.h"
#include "foundation/i18n.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_import.h"
#include <QRegularExpression>
#include <algorithm>
#include <array>
namespace mf {
namespace nwchem_detail {
const QVector<LibraryBasis> &libraryBases() {
    static const QVector<LibraryBasis> list{
        {"sto-3g", "STO-3G", {{1, 53}}, ""},
        {"3-21g", "3-21G", {{1, 55}}, ""},
        {"6-31g", "6-31G", {{1, 30}}, ""},
        {"6-31g*", "6-31G*", {{1, 36}}, ""},
        {"6-31g**", "6-31G**", {{1, 30}}, ""},
        {"6-31+g*", "6-31+G*", {{1, 20}}, ""},
        {"6-31++g**", "6-31++G**", {{1, 20}}, ""},
        {"6-311g**", "6-311G**", {{1, 20}, {31, 36}, {53, 53}}, ""},
        {"6-311+g**", "6-311+G**", {{1, 20}}, ""},
        {"6-311++g**", "6-311++G**", {{1, 20}}, ""},
        {"cc-pvdz", "cc-pVDZ", {{1, 18}, {20, 36}}, ""},
        {"cc-pvtz", "cc-pVTZ", {{1, 18}, {20, 36}}, ""},
        {"cc-pvqz", "cc-pVQZ", {{1, 18}, {20, 36}}, ""},
        {"aug-cc-pvdz", "aug-cc-pVDZ", {{1, 18}, {21, 36}}, ""},
        {"aug-cc-pvtz", "aug-cc-pVTZ", {{1, 18}, {21, 36}}, ""},
        {"def2-svp", "def2-SVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-svpd", "def2-SVPD", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvp", "def2-TZVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvpp", "def2-TZVPP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-tzvpd", "def2-TZVPD", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"def2-qzvp", "def2-QZVP", {{1, 57}, {72, 86}}, "def2-ecp"},
        {"lanl2dz ecp",
         QT_TR_NOOP("LANL2DZ（Na以降ECP）"),
         {{1, 1}, {3, 57}, {72, 83}, {92, 94}},
         "lanl2dz ecp"},
        {"x2c-svpall", QT_TR_NOOP("x2c-SVPall（相対論・全電子）"), {{1, 86}}, "", true},
        {"x2c-tzvpall", QT_TR_NOOP("x2c-TZVPall（相対論・全電子）"), {{1, 86}}, "", true},
        {"cc-pvdz-dk", QT_TR_NOOP("cc-pVDZ-DK（相対論）"), {{1, 18}, {21, 36}}, "", true},
        {"cc-pvtz-dk", QT_TR_NOOP("cc-pVTZ-DK（相対論）"), {{1, 18}, {21, 36}, {39, 46}}, "", true},
        {"sarc-zora", QT_TR_NOOP("SARC-ZORA（La–Rn・Ac–Lr、相対論）"), {{57, 86}, {89, 103}}, "", true},
        {"sarc-dkh", QT_TR_NOOP("SARC-DKH（La–Rn・Ac–Lr、相対論）"), {{57, 86}, {89, 103}}, "", true}};
    return list;
}
const LibraryBasis *libraryBasis(const QString &name) {
    for (const auto &b : libraryBases())
        if (name == b.name)
            return &b;
    return nullptr;
}
// Core electrons replaced by the basis' ECP ("nelec" in NWChem's def2-ecp and lanl2dz_ecp).
int ecpCore(const QString &basis, int z) {
    const auto *b = libraryBasis(basis);
    if (!b || !*b->ecp)
        return 0;
    static const QMap<QString, QVector<std::array<int, 3>>> cores{
        {"def2-ecp", {{37, 54, 28}, {55, 57, 46}, {72, 86, 60}}},
        {"lanl2dz ecp",
         {{11, 29, 10},
          {30, 30, 18},
          {31, 47, 28},
          {48, 48, 36},
          {49, 57, 46},
          {72, 79, 60},
          {80, 81, 68},
          {82, 83, 78},
          {92, 94, 78}}}};
    for (const auto &[lo, hi, core] : cores.value(b->ecp))
        if (z >= lo && z <= hi)
            return core;
    return 0;
}
// The pasted basis and ECP of settings ("basis_custom", "ecp_custom"), read once for each text.
Pasted pasted(const QJsonObject &settings) {
    thread_local QHash<QString, NwchemPastedBasis> read[2];
    const auto get = [](const QString &text, bool ecp) {
        if (text.trimmed().isEmpty())
            return NwchemPastedBasis{};
        auto &cache = read[ecp];
        if (const auto it = cache.constFind(text); it != cache.cend())
            return *it;
        if (cache.size() > 16)
            cache.clear();
        return *cache.insert(text, nwchemPastedBasis(text, ecp));
    };
    return {get(settings["basis_custom"].toString(), false), get(settings["ecp_custom"].toString(), true)};
}
// The library basis of an element: its per-element override, otherwise the common basis.
QString elementBasis(const QJsonObject &settings, const QString &symbol) {
    const auto overrides = settings["basis_overrides"].toObject();
    return overrides.contains(symbol) ? overrides[symbol].toString() : settings["basis"].toString();
}
// Core electrons an ECP replaces: a pasted ECP's "nelec"; none for an element with a pasted basis and
// no pasted ECP (the library's ECP belongs to the library's basis); otherwise the library basis' ECP.
int coreElectrons(const QJsonObject &settings, const Pasted &p, const QString &symbol, int z) {
    if (const auto it = p.ecp.cores.constFind(symbol); it != p.ecp.cores.cend())
        return *it;
    return p.basis.lines.contains(symbol) ? 0 : ecpCore(elementBasis(settings, symbol), z);
}
int atomicCoreOrbitals(int z) {
    // NWChem FREEZE ATOMIC table. Ghosts and ECP centers are excluded by the caller.
    return z <= 2 ? 0 : z <= 10 ? 1 : z <= 18 ? 5 : z <= 36 ? 9 : z <= 54 ? 18 : 27;
}
// Coverage of the curated NWChem basis library entries (including ghost centers).
bool basisCovers(const QString &basis, int z) {
    const auto *b = libraryBasis(basis);
    return b && std::any_of(b->elements.begin(), b->elements.end(),
                            [z](const auto &range) { return z >= range.first && z <= range.second; });
}
// DFT functionals offered in the GUI. Dispersion corrections are listed only where NWChem 7.3.1
// prints Grimme's published parameters: it has none for M06*-D3(BJ) and SCAN/r2SCAN-D3, and its
// B3PW91-D3(BJ) and PW6B95-D3 values disagree with Grimme's. Meta-GGA and range-separated
// functionals have no analytic Hessian in NWChem (it falls back to finite differences), and its
// TDDFT stops for meta-GGA kernels ("d2 not coded").
const QVector<Functional> &functionals() {
    static const QVector<Functional> list{
        {"b3lyp", "B3LYP", "b3lyp", "", true, true, false},
        {"pbe", "PBE", "xpbe96 cpbe96", "", true, true, false},
        {"pbe0", "PBE0", "pbe0", "", true, true, false},
        {"blyp", "BLYP", "becke88 lyp", "", true, true, false},
        {"bp86", "BP86", "becke88 perdew86", "", true, true, false},
        {"b3pw91", "B3PW91", "b3pw91", "", true, false, false},
        {"bhlyp", "BHLYP", "bhlyp", "", true, true, false},
        {"tpss", QT_TR_NOOP("TPSS（meta-GGA）"), "xtpss03 ctpss03", "", true, true, true, true},
        {"scan", QT_TR_NOOP("SCAN（meta-GGA）"), "scan", "", false, true, true, true},
        {"r2scan", QT_TR_NOOP("r²SCAN（meta-GGA）"), "r2scan", "", false, true, true, true},
        {"m06-l", QT_TR_NOOP("M06-L（meta-GGA）"), "m06-l", "", true, false, true, true},
        {"m06", "M06", "m06", "", true, false, true, true},
        {"m06-2x", "M06-2X", "m06-2x", "", true, false, true, true},
        {"pw6b95", "PW6B95", "pw6b95", "", false, true, true, true},
        {"cam-b3lyp", QT_TR_NOOP("CAM-B3LYP（長距離補正）"), "xcamb88 1.00 lyp 0.81 vwn_5 0.19 hfexch 1.00",
         "cam 0.33 cam_alpha 0.19 cam_beta 0.46", true, true, true},
        {"wb97x-d3", QT_TR_NOOP("ωB97X-D3（D3を含む長距離補正）"), "wb97x-d3", "", false, false, true},
        {"custom", QT_TR_NOOP("任意のxc指定"), "", "", false, false, true}};
    return list;
}
const Functional *functional(const QString &value) {
    for (const auto &f : functionals())
        if (value == f.value)
            return &f;
    return nullptr;
}
// Pople's 6-31G family is defined with Cartesian d shells (6d); the other library bases are
// conventionally used with spherical harmonics (5d/7f), as in Gaussian's defaults.
bool cartesianByConvention(const QString &basis) {
    return basis == "3-21g" || basis.startsWith("6-31g") || basis.startsWith("6-31+") ||
           basis.startsWith("6-31++");
}
// Coverage of Weigend's universal Coulomb fitting set in the NWChem library.
bool fittingCovers(int z) {
    return (z >= 1 && z <= 57) || (z >= 72 && z <= 86);
}
} // namespace nwchem_detail
using namespace nwchem_detail;
NwchemPastedBasis nwchemPastedBasis(const QString &text, bool ecp) {
    NwchemPastedBasis result;
    if (text.size() > 500000) {
        result.error = ecp ? tr("貼り付けたECPは500000文字以内にしてください。")
                           : tr("貼り付けた基底は500000文字以内にしてください。");
        return result;
    }
    static const QRegularExpression number(R"(^[+-]?(\d+\.?\d*|\.\d+)([eEdD][+-]?\d+)?$)");
    static const QStringList shells{"s", "p", "d", "f", "g", "h", "i", "k", "sp", "l"},
        ecpShells{"ul", "s", "p", "d", "f", "g", "h", "i"};
    QString current;     // The element of the shell being read.
    bool numbers = true; // The shell begun last has its lines of numbers.
    int headerLine = 0;
    const auto problem = [&](int line, const QString &why) {
        if (result.error.isEmpty())
            result.error =
                (ecp ? tr("貼り付けたECPの%1行目: %2") : tr("貼り付けた基底の%1行目: %2")).arg(line).arg(why);
    };
    const auto lines = text.split('\n');
    for (int i = 0; i < lines.size() && result.error.isEmpty(); ++i) {
        const auto raw = lines[i];
        if (raw.contains(QRegularExpression("[\\x00-\\x08\\x0b-\\x1f\\x7f]"))) {
            problem(i + 1, tr("制御文字を含めないでください。"));
            break;
        }
        const auto line = raw.section('#', 0, 0).simplified();
        if (line.isEmpty())
            continue;
        const auto words = line.split(' ');
        const auto first = words[0].toLower();
        // The block around the entries, as the Basis Set Exchange writes it.
        if (first == "end" || (!ecp && first == "basis") || (ecp && first == "ecp")) {
            if (!numbers)
                problem(headerLine, tr("殻の後に数値の行がありません。"));
            current.clear();
            numbers = true;
            continue;
        }
        if (std::all_of(words.begin(), words.end(),
                        [&](const QString &w) { return number.match(w).hasMatch(); })) {
            if (current.isEmpty())
                problem(i + 1, tr("数値の行の前に、元素と殻の行が必要です。"));
            else if (ecp ? words.size() != 3 : words.size() < 2)
                problem(i + 1, ecp ? tr("ECPの数値の行は3個の数値（rの次数・指数・係数）です。")
                                   : tr("基底の数値の行は指数と係数の2個以上の数値です。"));
            else {
                result.lines[current] << "    " + words.join(' ');
                numbers = true;
            }
            continue;
        }
        // "<element> <shell>": the element as its symbol (NWChem takes either case).
        const auto symbol = words[0].left(1).toUpper() + words[0].mid(1).toLower();
        bool known = true;
        try {
            element(symbol);
        } catch (const std::exception &) {
            known = false;
        }
        if (!known || words[0].size() > 2) {
            problem(i + 1, tr("「%1」は元素記号ではありません。").arg(words[0]));
            break;
        }
        if (!numbers)
            problem(headerLine, tr("殻の後に数値の行がありません。"));
        const auto kind = words.value(1).toLower();
        if (!ecp) {
            if (words.size() != 2 || !shells.contains(kind)) {
                problem(i + 1, tr("「元素 殻」（例: C S）の行が必要です。"));
                break;
            }
            result.lines[symbol] << QString("  %1 %2").arg(symbol, kind.toUpper());
            numbers = false;
        } else if (words.size() == 3 && kind == "nelec") {
            bool ok;
            const int core = words[2].toInt(&ok);
            if (!ok || core < 0 || core >= element(symbol).number) {
                problem(i + 1, tr("nelecは0以上で原子番号より小さい整数にしてください。"));
                break;
            }
            result.cores[symbol] = core;
            result.lines[symbol] << QString("  %1 nelec %2").arg(symbol).arg(core);
        } else if (words.size() == 2 && ecpShells.contains(kind)) {
            result.lines[symbol] << QString("  %1 %2").arg(symbol, kind == "ul" ? kind : kind.toUpper());
            numbers = false;
        } else {
            problem(i + 1, tr("「元素 nelec 数」「元素 ul」「元素 殻」のいずれかの行が必要です。"));
            break;
        }
        headerLine = i + 1;
        current = symbol;
        if (!result.elements.contains(symbol))
            result.elements << symbol;
    }
    if (!numbers)
        problem(headerLine, tr("殻の後に数値の行がありません。"));
    if (ecp && result.error.isEmpty())
        for (const auto &symbol : std::as_const(result.elements))
            if (!result.cores.contains(symbol)) {
                result.error = tr("貼り付けたECPの元素 %1 に「%1 nelec 数」の行がありません。").arg(symbol);
                break;
            }
    if (!result.error.isEmpty()) {
        result.lines.clear();
        result.cores.clear();
    }
    return result;
}
QString nwchemFunctionalXc(const QString &value) {
    const auto *f = functional(value);
    return f ? QString(f->xc) : QString();
}
QString nwchemBasisLabel(const QJsonObject &settings) {
    // The usual spelling of the choices (def2-SVP, cc-pVDZ), without their notes ("（…）" of the
    // source label); others as stored.
    const auto name = [](const QString &value) {
        const auto *b = libraryBasis(value);
        return b ? QString(b->label).section(QChar(0xFF08), 0, 0) : value;
    };
    QString label = name(settings["basis"].toString());
    const auto overrides = settings["basis_overrides"].toObject();
    const auto p = pasted(settings);
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        if (!p.basis.lines.contains(it.key()))
            label = tr("%1、%2: %3").arg(label, it.key(), name(it.value().toString()));
    if (!p.basis.lines.isEmpty())
        label = tr("%1、貼り付け: %2").arg(label, p.basis.elements.join(tr("・")));
    if (!p.ecp.lines.isEmpty())
        label = tr("%1、貼り付けたECP: %2").arg(label, p.ecp.elements.join(tr("・")));
    return label;
}
QString nwchemFunctionalLabel(const QJsonObject &settings) {
    if (settings["functional"] == "custom")
        return "xc " + settings["xc_custom"].toString().simplified();
    const auto *f = functional(settings["functional"].toString());
    return f ? QString(f->value).toUpper() : settings["functional"].toString().toUpper();
}
bool nwchemNumericalFrequencies(const Document &d) {
    const auto s = nwchemSettings(d);
    const NwchemCalculation calc(s);
    const auto *f = functional(s["functional"].toString());
    // NWChem's analytic Hessian covers neither solvation, relativistic Hamiltonians nor CD fitting.
    if (s["numerical_frequencies"].toBool() || calc.method == NwchemMethod::Mp2 || s["reference"] == "rohf" ||
        (calc.dft() && f && f->numericalHessian) ||
        (s["reference"] == "auto" && calc.method == NwchemMethod::Hf && d.multiplicity > 1) ||
        s["solvent"] != "none" || s["relativistic"] != "none" || (calc.dft() && s["ri_j"].toBool()))
        return true;
    const auto p = pasted(s);
    for (const auto &atom : d.atoms)
        if (!atom.ghost) {
            try {
                if (coreElectrons(s, p, atom.element, element(atom.element).number))
                    return true;
            } catch (const std::exception &) {
                // Validation reports unknown atoms separately.
            }
        }
    return false;
}
} // namespace mf
