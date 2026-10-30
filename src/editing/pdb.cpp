// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/pdb.h"
#include "model/document.h"
#include "model/i18n.h"
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QSet>

namespace mf {
bool waterResidue(const QString &code) {
    static const QSet<QString> waters{"HOH", "WAT", "H2O", "DOD", "D2O", "TIP", "TIP3", "TIP4", "SPC", "SOL"};
    return waters.contains(code);
}
bool MacromoleculeSelection::keeps(const QString &chain, ResidueKind kind) const {
    return (chains.isEmpty() || chains.contains(chain)) && (kind == ResidueKind::Polymer  ? polymer
                                                            : kind == ResidueKind::Ligand ? ligands
                                                                                          : water);
}
namespace {
bool atomRecord(const QString &line) {
    return line.startsWith("ATOM  ") || line.startsWith("HETATM");
}
QString chainOf(const QString &line) {
    return line.mid(21, 1).trimmed();
}
// The residue names of each chain's sequence (SEQRES): a modified residue of the chain is a HETATM record
// that belongs to the polymer.
QHash<QString, QSet<QString>> sequences(const QStringList &lines) {
    QHash<QString, QSet<QString>> out;
    for (const auto &line : lines)
        if (line.startsWith("SEQRES"))
            for (const auto &code : line.mid(19).split(' ', Qt::SkipEmptyParts))
                out[line.mid(11, 1).trimmed()].insert(code);
    return out;
}
ResidueKind kindOf(const QString &line, const QHash<QString, QSet<QString>> &sequences) {
    const auto code = line.mid(17, 3).trimmed();
    if (waterResidue(code))
        return ResidueKind::Water;
    if (line.startsWith("ATOM  ") || sequences.value(chainOf(line)).contains(code))
        return ResidueKind::Polymer;
    return ResidueKind::Ligand;
}
QStringList models(const QStringList &lines) {
    QStringList out;
    for (const auto &line : lines)
        if (line.startsWith("MODEL "))
            out << line.mid(10, 4).trimmed();
    if (out.isEmpty())
        out << "1";
    out.removeDuplicates();
    return out;
}
} // namespace
MacromoleculeContents pdbContents(const QString &text) {
    const auto lines = text.split('\n');
    MacromoleculeContents contents;
    contents.models = models(lines);
    // COMPND: MOL_ID blocks of MOLECULE and CHAIN tokens, continued over the lines.
    QString compound;
    for (const auto &line : lines)
        if (line.startsWith("COMPND"))
            compound += ' ' + line.mid(10).trimmed();
    QHash<QString, QString> names;
    QString molecule;
    for (const auto &token : compound.split(';')) {
        const auto key = token.section(':', 0, 0).trimmed(), value = token.section(':', 1).trimmed();
        if (key == "MOL_ID")
            molecule.clear();
        else if (key == "MOLECULE")
            molecule = value;
        else if (key == "CHAIN")
            for (const auto &chain : value.split(','))
                names[chain.trimmed()] = molecule;
    }
    const auto sequence = sequences(lines);
    bool first = !text.contains(QRegularExpression("(^|\n)MODEL "));
    QSet<QString> residues;
    QHash<QString, int> chains;
    for (const auto &line : lines) {
        if (line.startsWith("MODEL "))
            first = line.mid(10, 4).trimmed() == contents.models.first();
        if (!atomRecord(line))
            continue;
        contents.altlocs = contents.altlocs || !line.mid(16, 1).trimmed().isEmpty();
        if (!first || residues.contains(line.mid(17, 10)))
            continue;
        residues.insert(line.mid(17, 10)); // Name, chain, number and insertion code.
        const auto chain = chainOf(line);
        auto it = chains.find(chain);
        if (it == chains.end()) {
            it = chains.insert(chain, contents.chains.size());
            contents.chains.append({chain, names.value(chain), 0, {}});
        }
        const auto kind = kindOf(line, sequence);
        if (const auto code = line.mid(17, 3).trimmed();
            kind != ResidueKind::Polymer && !contents.chains[*it].groups.contains(code))
            contents.chains[*it].groups << code;
        switch (kind) {
        case ResidueKind::Polymer:
            ++contents.polymers;
            ++contents.chains[*it].residues;
            break;
        case ResidueKind::Ligand:
            ++contents.ligands;
            break;
        case ResidueKind::Water:
            ++contents.waters;
        }
    }
    return contents;
}
QString preparePdb(const QString &text, const MacromoleculeSelection &selection) {
    const auto lines = text.split('\n');
    const auto sequence = sequences(lines);
    const bool hasModels = text.contains(QRegularExpression("(^|\n)MODEL "));
    int current = hasModels ? 0 : selection.model, inModel = 0;
    QStringList chosen;
    QHash<QString, QMap<QString, double>> occupancies;
    for (const auto &l : lines) {
        if (l.startsWith("MODEL "))
            current = l.mid(10, 4).trimmed().toInt();
        if (l.startsWith("ENDMDL"))
            current = 0;
        if (current != selection.model)
            continue;
        if (atomRecord(l)) {
            ++inModel;
            if (!selection.keeps(chainOf(l), kindOf(l, sequence)))
                continue;
            if (const auto alt = l.mid(16, 1); !alt.trimmed().isEmpty())
                occupancies[l.mid(21, 6)][alt] += l.mid(54, 6).toDouble();
        }
        chosen << l;
    }
    QHash<QString, QString> preferred;
    for (auto it = occupancies.begin(); it != occupancies.end(); ++it) {
        double best = -1;
        for (auto a = it.value().begin(); a != it.value().end(); ++a)
            if (a.value() > best) {
                best = a.value();
                preferred[it.key()] = a.key();
            }
    }
    QString out;
    QSet<int> serials;
    for (auto l : chosen) {
        // Open Babel takes a blank line, as the end of a file without END, for a broken file.
        if (l.startsWith("MODEL") || l.startsWith("ENDMDL") || l.trimmed().isEmpty())
            continue;
        if (atomRecord(l)) {
            const auto alt = l.mid(16, 1),
                       want = selection.altloc.isEmpty() ? preferred[l.mid(21, 6)] : selection.altloc;
            if (!alt.trimmed().isEmpty() && alt != want)
                continue;
            if (l.size() > 16)
                l[16] = ' ';
            serials.insert(l.mid(6, 5).trimmed().toInt());
        }
        // Open Babel drops a CONECT record that names an atom left out, with a warning.
        if (l.startsWith("CONECT")) {
            bool kept = true;
            for (int i = 6; i < 31 && i < l.size(); i += 5)
                if (const auto serial = l.mid(i, 5).trimmed(); !serial.isEmpty())
                    kept = kept && serials.contains(serial.toInt());
            if (!kept)
                continue;
        }
        out += l + '\n';
    }
    if (inModel && serials.isEmpty())
        fail(tr("選んだ鎖・成分に原子がありません。"));
    return out;
}
} // namespace mf
