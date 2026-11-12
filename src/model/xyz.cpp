// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/i18n.h"
#include "model/document.h"
#include <cmath>

// The XYZ dialect of MolForma: an element with "@" is a ghost atom, D and T are hydrogen isotopes, and a
// fifth column gives an isotope (a whole number) or a mass.
namespace mf {
QString Document::xyz() const {
    QString s = QString::number(atoms.size()) + "\nMolForma | " + formula() + " | angstrom\n";
    for (const auto &a : atoms)
        s += QString("%1 %2 %3 %4%5\n")
                 .arg((a.ghost ? "@" : "") + a.element)
                 .arg(a.position.x(), 0, 'f', 10)
                 .arg(a.position.y(), 0, 'f', 10)
                 .arg(a.position.z(), 0, 'f', 10)
                 .arg(a.isotope    ? " " + QString::number(a.isotope)
                      : a.mass > 0 ? " " + QString::number(a.mass, 'g', 15)
                                   : "");
    return s;
}
Document Document::fromXyz(const QString &text) {
    auto lines = text.split('\n');
    if (lines.size() < 2)
        fail(tr("XYZには原子数とコメント行が必要です。"));
    bool ok = false;
    int n = lines[0].trimmed().toInt(&ok);
    if (!ok || n < 0 || n > 100000 || lines.size() < n + 2)
        fail(tr("XYZの原子数が不正です。"));
    Document d;
    for (int i = 0; i < n; ++i) {
        auto w = lines[i + 2].simplified().split(' ');
        if (w.size() < 4 || w.size() > 5)
            fail(tr("XYZ %1行目の形式が不正です。").arg(i + 3));
        Atom a;
        a.element = w[0];
        a.ghost = a.element.startsWith('@');
        if (a.ghost)
            a.element.remove(0, 1);
        if (a.element == "D" || a.element == "T") {
            a.isotope = a.element == "D" ? 2 : 3;
            a.element = "H";
        }
        try {
            element(a.element);
        } catch (const std::exception &e) {
            fail(tr("XYZ %1行目: %2").arg(QString::number(i + 3), QString::fromUtf8(e.what())));
        }
        for (int j = 0; j < 3; ++j) {
            a.position[j] = w[j + 1].toDouble(&ok);
            if (!ok || !std::isfinite(a.position[j]))
                fail(tr("XYZ %1行目の座標は有限の数値にしてください: %2")
                         .arg(QString::number(i + 3), lines[i + 2].trimmed()));
        }
        if (w.size() == 5) {
            int mass = w[4].toInt(&ok);
            if (ok && mass > 0)
                a.isotope = mass;
            else {
                a.mass = w[4].toDouble(&ok);
                if (!ok || a.mass <= 0 || !std::isfinite(a.mass))
                    fail(tr("XYZ %1行目の質量が不正です: %2")
                             .arg(QString::number(i + 3), lines[i + 2].trimmed()));
            }
        }
        d.atoms.append(a);
    }
    for (int i = n + 2; i < lines.size(); ++i)
        if (!lines[i].trimmed().isEmpty())
            fail(tr("複数フレームのXYZです。1フレームのファイルを指定してください。"));
    d.inferBonds();
    return d;
}
} // namespace mf
