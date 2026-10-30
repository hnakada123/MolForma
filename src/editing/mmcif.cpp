// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/mmcif.h"
#include "model/i18n.h"
#include <QHash>
#include <cctype>
#include <cstring>
#include <optional>

namespace mf {
namespace {
struct Token {
    QByteArrayView text; // Into the file's text, without quotes.
    bool quoted = false; // Quoted strings and text fields are values whatever they spell.
    int line = 0;
};
// The tokens of a CIF 1.1 file: tags, values and the reserved words (data_, loop_ ...).
class Tokens {
  public:
    explicit Tokens(const QByteArray &text) : begin(text.constData()), p(begin), end(begin + text.size()) {}
    bool next(Token &token) {
        for (; p < end; ++p) {
            if (*p == '#')
                while (p + 1 < end && p[1] != '\n')
                    ++p;
            else if (*p == '\n')
                ++line;
            else if (!space(*p))
                break;
        }
        if (p == end)
            return false;
        token = {{}, false, line};
        const char *start = p;
        if (*p == ';' && (p == begin || p[-1] == '\n')) {
            // A text field runs to the next line that starts with a semicolon.
            do {
                p = static_cast<const char *>(std::memchr(p, '\n', end - p));
                if (!p)
                    fail(tr("mmCIFの%1行目から始まるテキストが閉じていません。").arg(token.line));
                ++line;
            } while (++p == end || *p != ';');
            token.text = QByteArrayView(start + 1, p - 1 - (start + 1));
            token.quoted = true;
            ++p;
            return true;
        }
        if (*p == '\'' || *p == '"') {
            // The closing quote is the one followed by white space: 'O5'' reads O5'.
            const char quote = *p++;
            while (!(p < end && *p == quote && (p + 1 == end || space(p[1])))) {
                if (p == end || *p == '\n')
                    fail(tr("mmCIFの%1行目の引用符が閉じていません。").arg(token.line));
                ++p;
            }
            token.text = QByteArrayView(start + 1, p - (start + 1));
            token.quoted = true;
            ++p;
            return true;
        }
        while (p < end && !space(*p))
            ++p;
        token.text = QByteArrayView(start, p - start);
        return true;
    }

  private:
    static bool space(char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    }
    const char *begin, *p, *end;
    int line = 1;
};
QByteArray lower(QByteArrayView text) {
    return text.toByteArray().toLower();
}
bool isTag(const Token &t) {
    return !t.quoted && t.text.startsWith('_');
}
bool reserved(const Token &t) {
    if (t.quoted)
        return false;
    const auto word = lower(t.text.left(7));
    for (const char *w : {"data_", "loop_", "save_", "global_", "stop_"})
        if (word.startsWith(w))
            return true;
    return false;
}
// The unquoted . (inapplicable) and ? (unknown) are no value.
QByteArrayView value(const Token &t) {
    return !t.quoted && (t.text == "." || t.text == "?") ? QByteArrayView() : t.text;
}
// The columns of the _atom_site table; -1 where one is missing.
struct Columns {
    explicit Columns(const QList<QByteArray> &tags) {
        const auto find = [&](const char *name) {
            return int(tags.indexOf(QByteArray("_atom_site.") + name));
        };
        element = find("type_symbol");
        authName = find("auth_atom_id");
        labelName = find("label_atom_id");
        authResidue = find("auth_comp_id");
        labelResidue = find("label_comp_id");
        authChain = find("auth_asym_id");
        labelChain = find("label_asym_id");
        authNumber = find("auth_seq_id");
        labelNumber = find("label_seq_id");
        insertion = find("pdbx_pdb_ins_code");
        altloc = find("label_alt_id");
        x = find("cartn_x");
        y = find("cartn_y");
        z = find("cartn_z");
        occupancy = find("occupancy");
        charge = find("pdbx_formal_charge");
        model = find("pdbx_pdb_model_num");
        QStringList missing;
        for (const auto &[column, name] : {std::pair{x, "Cartn_x"},
                                           {y, "Cartn_y"},
                                           {z, "Cartn_z"},
                                           {element, "type_symbol"},
                                           {std::max(authName, labelName), "label_atom_id"}})
            if (column < 0)
                missing << QString("_atom_site.%1").arg(name);
        if (!missing.isEmpty())
            fail(tr("mmCIFの原子の表に %1 がありません。").arg(missing.join(", ")));
    }
    int element, authName, labelName, authResidue, labelResidue, authChain, labelChain, authNumber,
        labelNumber, insertion, altloc, x, y, z, occupancy, charge, model;
};
MmcifAtom atom(const Columns &c, const QVector<QByteArrayView> &row, int line) {
    const auto at = [&](int column) { return column < 0 ? QByteArrayView() : row[column]; };
    const auto either = [&](int author, int label) {
        const auto v = at(author);
        return QString::fromUtf8(v.isEmpty() ? at(label) : v);
    };
    const auto number = [&](int column, const char *name, double missing) {
        const auto v = at(column);
        if (v.isEmpty())
            return missing;
        bool ok;
        const double n = v.toDouble(&ok);
        if (!ok)
            fail(tr("mmCIFの%1行目の %2 が数値ではありません: %3")
                     .arg(line)
                     .arg(QString("_atom_site.%1").arg(name), QString::fromUtf8(v)));
        return n;
    };
    MmcifAtom a;
    for (char ch : at(c.element)) {
        if (!std::isalpha(static_cast<unsigned char>(ch)))
            break; // A charge may follow, as in O1-.
        a.element += a.element.isEmpty() ? QChar(ch).toUpper() : QChar(ch).toLower();
    }
    a.name = either(c.authName, c.labelName);
    a.residue = either(c.authResidue, c.labelResidue);
    a.chain = either(c.authChain, c.labelChain);
    const bool author = !at(c.authNumber).isEmpty();
    a.number = int(number(author ? c.authNumber : c.labelNumber, author ? "auth_seq_id" : "label_seq_id", 0));
    a.insertion = QString::fromUtf8(at(c.insertion));
    a.altloc = QString::fromUtf8(at(c.altloc));
    for (int column : {c.x, c.y, c.z})
        if (at(column).isEmpty())
            fail(tr("mmCIFの%1行目の原子に座標がありません。").arg(line));
    a.position = Vec(number(c.x, "Cartn_x", 0), number(c.y, "Cartn_y", 0), number(c.z, "Cartn_z", 0));
    a.occupancy = number(c.occupancy, "occupancy", 1);
    a.charge = int(number(c.charge, "pdbx_formal_charge", 0));
    a.model = int(number(c.model, "pdbx_PDB_model_num", 1));
    return a;
}
} // namespace
QVector<MmcifAtom> mmcifAtoms(const QByteArray &text) {
    Tokens tokens(text);
    Token token;
    QVector<MmcifAtom> atoms;
    QList<QByteArray> itemTags; // _atom_site items outside a loop: a table of one atom.
    QVector<QByteArrayView> itemValues;
    int itemLine = 0;
    bool block = false, more = tokens.next(token);
    while (more) {
        if (isTag(token)) {
            const auto tag = lower(token.text);
            const int line = token.line;
            if (!(more = tokens.next(token)) || isTag(token) || reserved(token))
                fail(tr("mmCIFの%1行目の %2 に値がありません。").arg(line).arg(QString::fromUtf8(tag)));
            if (tag.startsWith("_atom_site.")) {
                itemLine = itemLine ? itemLine : line;
                itemTags << tag;
                itemValues << value(token);
            }
            more = tokens.next(token);
        } else if (!reserved(token)) {
            fail(tr("mmCIFの%1行目の値に項目名がありません。").arg(token.line));
        } else if (lower(token.text).startsWith("loop_")) {
            const int line = token.line;
            QList<QByteArray> tags;
            while ((more = tokens.next(token)) && isTag(token))
                tags << lower(token.text);
            if (tags.isEmpty())
                fail(tr("mmCIFの%1行目の loop_ に項目名がありません。").arg(line));
            std::optional<Columns> columns;
            if (tags.first().startsWith("_atom_site."))
                columns.emplace(tags);
            QVector<QByteArrayView> row;
            int rowLine = 0;
            qsizetype count = 0;
            for (; more && !isTag(token) && !reserved(token); more = tokens.next(token), ++count)
                if (columns) {
                    rowLine = row.isEmpty() ? token.line : rowLine;
                    row << value(token);
                    if (row.size() == tags.size()) {
                        atoms << atom(*columns, row, rowLine);
                        row.clear();
                    }
                }
            if (count % tags.size())
                fail(tr("mmCIFの%1行目の loop_ の値の数が項目の数と合いません。").arg(line));
        } else {
            if (lower(token.text).startsWith("data_")) {
                if (block)
                    break; // Only the first data block, which is the whole entry in PDB files.
                block = true;
            }
            more = tokens.next(token);
        }
    }
    if (!itemTags.isEmpty())
        atoms << atom(Columns(itemTags), itemValues, itemLine);
    if (atoms.isEmpty())
        fail(tr("mmCIFに原子の表（_atom_site）がありません。小分子のCIF（分率座標）は読み込めません。"));
    return atoms;
}
QStringList mmcifModels(const QVector<MmcifAtom> &atoms) {
    QStringList models;
    QSet<int> seen;
    for (const auto &a : atoms)
        if (!seen.contains(a.model)) {
            seen.insert(a.model);
            models << QString::number(a.model);
        }
    return models;
}
QVector<MmcifAtom> selectMmcif(const QVector<MmcifAtom> &atoms, int model, const QString &altloc) {
    const auto residue = [](const MmcifAtom &a) {
        return QString("%1\n%2\n%3").arg(a.chain).arg(a.number).arg(a.insertion);
    };
    QHash<QString, QMap<QString, double>> occupancies;
    for (const auto &a : atoms)
        if (a.model == model && !a.altloc.isEmpty())
            occupancies[residue(a)][a.altloc] += a.occupancy;
    QHash<QString, QString> preferred;
    for (auto it = occupancies.begin(); it != occupancies.end(); ++it) {
        double best = -1;
        for (auto a = it.value().begin(); a != it.value().end(); ++a)
            if (a.value() > best) {
                best = a.value();
                preferred[it.key()] = a.key();
            }
    }
    QVector<MmcifAtom> chosen;
    for (auto a : atoms) {
        if (a.model != model ||
            (!a.altloc.isEmpty() && a.altloc != (altloc.isEmpty() ? preferred[residue(a)] : altloc)))
            continue;
        a.altloc.clear();
        chosen << a;
    }
    if (chosen.isEmpty())
        fail(tr("mmCIFにモデル %1 の原子がありません。").arg(model));
    return chosen;
}
} // namespace mf
