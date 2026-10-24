#include "results/molden_orbitals.h"
#include "editing/coordinates.h"
#include <QRegularExpression>
#include <cmath>
#include <numbers>

namespace mf {
namespace {
double real(QString text) {
    text.replace('D', 'E', Qt::CaseInsensitive);
    bool ok;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value))
        fail("MOLDEN基底の数値が不正です。");
    return value;
}
int count(const QString &text, int maximum) {
    const double n = real(text);
    if (n < 1 || n > maximum || n != std::floor(n))
        fail("MOLDEN基底の原子番号・個数が範囲外です。");
    return int(n);
}
double factorial(int n) {
    double f = 1;
    for (int i = 2; i <= n; ++i)
        f *= i;
    return f;
}
double oddFactorial(int n) {
    double f = 1;
    for (int i = n; i > 1; i -= 2)
        f *= i;
    return f;
}
using Powers = std::array<int, 3>;
QVector<QVector<MoldenTerm>> angularFunctions(int l, bool spherical) {
    QVector<QVector<MoldenTerm>> result;
    if (!spherical || l < 2) {
        static const QVector<QVector<Powers>> order{
            {{0, 0, 0}},
            {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
            {{2, 0, 0}, {0, 2, 0}, {0, 0, 2}, {1, 1, 0}, {1, 0, 1}, {0, 1, 1}},
            {{3, 0, 0},
             {0, 3, 0},
             {0, 0, 3},
             {1, 2, 0},
             {2, 1, 0},
             {2, 0, 1},
             {1, 0, 2},
             {0, 1, 2},
             {0, 2, 1},
             {1, 1, 1}},
            {{4, 0, 0},
             {0, 4, 0},
             {0, 0, 4},
             {3, 1, 0},
             {3, 0, 1},
             {1, 3, 0},
             {0, 3, 1},
             {1, 0, 3},
             {0, 1, 3},
             {2, 2, 0},
             {2, 0, 2},
             {0, 2, 2},
             {2, 1, 1},
             {1, 2, 1},
             {1, 1, 2}}};
        for (const auto &p : order[l])
            result.append({{p, std::sqrt(oddFactorial(2 * l - 1) /
                                         (oddFactorial(2 * p[0] - 1) * oddFactorial(2 * p[1] - 1) *
                                          oddFactorial(2 * p[2] - 1)))}});
        return result;
    }
    // Real solid harmonics r^l P_l^m(z/r) exp(i m phi), without the Condon-Shortley phase.
    // Their squared angular integrals equal that of x^l, matching the radial normalization below.
    for (int m = 0; m <= l; ++m) {
        for (int imaginary = 0; imaginary < (m ? 2 : 1); ++imaginary) {
            QMap<Powers, double> terms;
            const double scale = m ? std::sqrt(2 * factorial(l - m) / factorial(l + m)) : 1;
            for (int k = 0; k <= (l - m) / 2; ++k) {
                const double legendre =
                    (k % 2 ? -1 : 1) * factorial(2 * l - 2 * k) /
                    (std::pow(2., l) * factorial(k) * factorial(l - k) * factorial(l - 2 * k - m));
                for (int y = imaginary; y <= m; y += 2) {
                    const double complex =
                        ((y / 2) % 2 ? -1 : 1) * factorial(m) / (factorial(y) * factorial(m - y));
                    for (int x2 = 0; x2 <= k; ++x2)
                        for (int y2 = 0; y2 <= k - x2; ++y2) {
                            const int z2 = k - x2 - y2;
                            terms[{m - y + 2 * x2, y + 2 * y2, l - m - 2 * k + 2 * z2}] +=
                                scale * legendre * complex * factorial(k) /
                                (factorial(x2) * factorial(y2) * factorial(z2));
                        }
                }
            }
            QVector<MoldenTerm> polynomial;
            for (auto it = terms.begin(); it != terms.end(); ++it)
                if (std::abs(it.value()) > 1e-14)
                    polynomial.append({it.key(), it.value()});
            result << polynomial;
        }
    }
    return result;
}
double primitiveNorm(double exponent, int l) {
    return std::pow(2 * exponent / std::numbers::pi, .75) *
           std::sqrt(std::pow(4 * exponent, l) / oddFactorial(2 * l - 1));
}
double shellNorm(const MoldenShell &s, bool primitiveNormalized) {
    double sum = 0;
    const int l = s.angularMomentum;
    for (const auto &a : s.primitives)
        for (const auto &b : s.primitives) {
            const double p = a.exponent + b.exponent;
            sum += a.coefficient * b.coefficient *
                   (primitiveNormalized ? primitiveNorm(a.exponent, l) * primitiveNorm(b.exponent, l) : 1) *
                   std::pow(std::numbers::pi / p, 1.5) * oddFactorial(2 * l - 1) / std::pow(2 * p, l);
        }
    return sum;
}
QVector<QVector<MoldenTerm>> orbitalPolynomials(const MoldenWavefunction &w, int orbital) {
    if (orbital < 0 || orbital >= w.coefficients.size())
        fail("MOLDENの軌道番号が範囲外です。");
    QVector<QVector<MoldenTerm>> result;
    int ao = 0;
    for (const auto &shell : w.shells) {
        QMap<Powers, double> terms;
        for (const auto &f : shell.functions) {
            const double c = w.coefficients[orbital][ao++];
            for (const auto &term : f)
                terms[term.powers] += c * term.coefficient;
        }
        QVector<MoldenTerm> polynomial;
        for (auto it = terms.begin(); it != terms.end(); ++it)
            if (it.value() != 0)
                polynomial.append({it.key(), it.value()});
        result << polynomial;
    }
    return result;
}
double evaluate(const MoldenWavefunction &w, const QVector<QVector<MoldenTerm>> &polynomials,
                const Vec &bohr) {
    double value = 0;
    for (int i = 0; i < w.shells.size(); ++i) {
        const auto &s = w.shells[i];
        if (polynomials[i].isEmpty())
            continue;
        const Vec r = bohr - s.center;
        const double r2 = r.squaredNorm();
        double radial = 0;
        for (const auto &p : s.primitives)
            radial += p.coefficient * std::exp(-p.exponent * r2);
        double powers[3][5];
        for (int a = 0; a < 3; ++a) {
            powers[a][0] = 1;
            for (int n = 1; n <= s.angularMomentum; ++n)
                powers[a][n] = powers[a][n - 1] * r[a];
        }
        double angular = 0;
        for (const auto &t : polynomials[i])
            angular +=
                t.coefficient * powers[0][t.powers[0]] * powers[1][t.powers[1]] * powers[2][t.powers[2]];
        value += radial * angular;
    }
    if (!std::isfinite(value))
        fail("MOLDENの軌道評価が有限値になりません。");
    return value;
}
} // namespace
MoldenWavefunction moldenWavefunction(const QMap<QString, QStringList> &blocks, const Document &atoms,
                                      const QVector<QVector<double>> &coefficients, const QString &mode) {
    if (atoms.atoms.isEmpty() || !blocks.contains("GTO") || coefficients.isEmpty())
        fail("軌道の形状表示にはMOLDENの[Atoms]・[GTO]・[MO]が必要です。");
    MoldenWavefunction w;
    QMap<QChar, int> angular;
    const QRegularExpression angularHeader("^(?:[0-9]+[DFG])+$"), angularToken("([0-9]+)([DFG])");
    for (auto it = blocks.cbegin(); it != blocks.cend(); ++it) {
        if (!angularHeader.match(it.key()).hasMatch())
            continue;
        auto matches = angularToken.globalMatch(it.key());
        while (matches.hasNext()) {
            const auto match = matches.next();
            const QChar type = match.captured(2)[0];
            const int n = match.captured(1).toInt();
            const int l = QString("spDFG").indexOf(type);
            if ((n != 2 * l + 1 && n != (l + 1) * (l + 2) / 2) ||
                (angular.contains(type) && angular[type] != n))
                fail("MOLDENの球面／Cartesian指定が不正または矛盾しています。");
            angular[type] = n;
        }
    }
    const bool sphericalD = angular.value('D', 6) == 5;
    const bool sphericalF = angular.value('F', blocks.contains("5D") ? 7 : 10) == 7;
    const bool sphericalG = angular.value('G', 15) == 9;
    const auto lines = blocks["GTO"];
    int atom = -1, basisCount = 0, primitiveCount = 0;
    QSet<int> centers;
    for (int i = 0; i < lines.size();) {
        auto row = lines[i++].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (row.isEmpty())
            continue;
        if (row[0][0].isDigit()) {
            atom = count(row[0], atoms.atoms.size()) - 1;
            if (row.size() > 2 || (row.size() == 2 && real(row[1]) != 0) || centers.contains(atom))
                fail("MOLDEN基底の中心指定が不正または重複しています。");
            centers.insert(atom);
            continue;
        }
        const auto label = row[0].toLower();
        const int l = QString("spdfg").indexOf(label);
        if (atom < 0 || row.size() < 2 || row.size() > 3 || (label != "sp" && (label.size() != 1 || l < 0)))
            fail("MOLDEN基底はs・p・sp・d・f・gに対応します。中心番号・シェル指定を確認してください。");
        const int n = count(row[1], 100);
        if (row.size() == 3)
            real(row[2]); // Obsolete scale field; NWChem writes 0, standard writers use 1.
        MoldenShell s;
        s.center = atoms.atoms[atom].position / BohrToAngstrom;
        s.angularMomentum = label == "sp" ? 0 : l;
        auto p = s;
        p.angularMomentum = 1;
        for (int j = 0; j < n; ++j) {
            if (i >= lines.size())
                fail("MOLDEN基底のプリミティブが不足しています。");
            const auto primitive = lines[i++].split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (primitive.size() != (label == "sp" ? 3 : 2))
                fail("MOLDEN基底の指数・収縮係数が不足しています。");
            const double exponent = real(primitive[0]);
            if (exponent < 1e-12 || exponent > 1e12)
                fail("MOLDEN基底の指数が範囲外です。");
            s.primitives.append({exponent, real(primitive[1])});
            if (label == "sp")
                p.primitives.append({exponent, real(primitive[2])});
        }
        const auto add = [&](MoldenShell shell) {
            const int l = shell.angularMomentum;
            shell.functions = angularFunctions(l, l == 2 ? sphericalD : l == 3 ? sphericalF : sphericalG);
            basisCount += shell.functions.size();
            primitiveCount += shell.primitives.size();
            if (basisCount > 4000 || primitiveCount > 10000)
                fail("MOLDENの軌道表示は4000基底・10000プリミティブまでです。");
            w.shells << shell;
        };
        add(s);
        if (label == "sp")
            add(p);
    }
    if (w.shells.isEmpty())
        fail("MOLDENの[GTO]に基底がありません。");
    if (qint64(basisCount) * coefficients.size() > 4000000)
        fail("MOLDENのMO係数はゼロ成分を含めて400万個までです。");
    w.coefficients = coefficients;
    for (auto &orbital : w.coefficients) {
        if (orbital.size() > basisCount || orbital.isEmpty())
            fail("MOLDENの基底数とMO係数の番号が一致しません。");
        orbital.resize(basisCount); // Omitted AO entries in sparse files are zero.
    }
    QString normalization = mode;
    if (normalization == "auto") {
        double standard = 0, embedded = 0;
        for (const auto &s : w.shells) {
            standard += std::abs(std::log(std::max(1e-300, shellNorm(s, true))));
            embedded += std::abs(std::log(std::max(1e-300, shellNorm(s, false))));
        }
        standard /= w.shells.size();
        embedded /= w.shells.size();
        if (std::min(standard, embedded) > .05 ||
            (std::max(standard, embedded) > .001 && std::abs(standard - embedded) < .02))
            fail("基底係数の正規化規約を判定できません。「軌道」の正規化を明示して再表示してください。");
        normalization = embedded < standard ? "nwchem" : "standard";
    }
    if (normalization != "standard" && normalization != "nwchem")
        fail("MOLDENの正規化規約が不正です。");
    w.normalization = normalization;
    for (auto &s : w.shells) {
        if (normalization == "standard")
            for (auto &p : s.primitives)
                p.coefficient *= primitiveNorm(p.exponent, s.angularMomentum);
        const double norm = shellNorm(s, false);
        if (!std::isfinite(norm) || norm < 1e-100)
            fail("MOLDENの収縮基底を正規化できません。");
        for (auto &p : s.primitives)
            p.coefficient /= std::sqrt(norm);
    }
    return w;
}
double MoldenWavefunction::value(int orbital, const Vec &angstrom) const {
    return evaluate(*this, orbitalPolynomials(*this, orbital), angstrom / BohrToAngstrom);
}
QVector<double> moldenOrbitalGrid(const MoldenWavefunction &w, int orbital, const Vec &origin,
                                  const Vec &step, const std::array<int, 3> &grid) {
    qint64 points = 1, primitives = 0;
    for (int n : grid) {
        if (n < 2 || n > 101)
            fail("MOLDENの格子点数が範囲外です。");
        points *= n;
    }
    for (const auto &s : w.shells)
        primitives += s.primitives.size();
    // The separable evaluation takes about 1 ns per point and primitive: 1–2 s at this bound.
    if (points * primitives > 1000000000)
        fail("軌道表示の計算量が大きすぎます。格子の分割数を小さくしてください。");
    const auto polynomials = orbitalPolynomials(w, orbital);
    QVector<double> values(points, 0.);
    auto *output = values.data();
    for (int shell = 0; shell < w.shells.size(); ++shell) {
        const auto &s = w.shells[shell];
        const auto &polynomial = polynomials[shell];
        if (polynomial.isEmpty())
            continue;
        // On an axis-aligned grid exp(-a*r^2) separates into three 1D tables.
        // Reuse exponentials and coordinate powers without screening or reducing grid precision.
        std::array<std::array<std::array<double, 5>, 101>, 3> powers;
        for (int axis = 0; axis < 3; ++axis)
            for (int i = 0; i < grid[axis]; ++i) {
                const double r = (origin[axis] + step[axis] * i) / BohrToAngstrom - s.center[axis];
                powers[axis][i][0] = 1;
                powers[axis][i][1] = r;
                for (int n = 2; n <= s.angularMomentum; ++n)
                    powers[axis][i][n] = powers[axis][i][n - 1] * r;
            }
        using AxisValues = std::array<std::array<double, 101>, 3>;
        QVector<AxisValues> exponentials(s.primitives.size());
        for (int p = 0; p < s.primitives.size(); ++p)
            for (int axis = 0; axis < 3; ++axis)
                for (int i = 0; i < grid[axis]; ++i) {
                    const double r = powers[axis][i][1];
                    exponentials[p][axis][i] = std::exp(-s.primitives[p].exponent * r * r);
                }
        const auto *exps = exponentials.constData();
        for (int x = 0; x < grid[0]; ++x)
            for (int y = 0; y < grid[1]; ++y) {
                std::array<double, 101> radial{};
                for (int p = 0; p < s.primitives.size(); ++p) {
                    const double xy = s.primitives[p].coefficient * exps[p][0][x] * exps[p][1][y];
                    for (int z = 0; z < grid[2]; ++z)
                        radial[z] += xy * exps[p][2][z];
                }
                std::array<double, 5> angular{};
                for (const auto &t : polynomial)
                    angular[t.powers[2]] +=
                        t.coefficient * powers[0][x][t.powers[0]] * powers[1][y][t.powers[1]];
                const int offset = (x * grid[1] + y) * grid[2];
                for (int z = 0; z < grid[2]; ++z) {
                    double a = angular[s.angularMomentum];
                    for (int n = s.angularMomentum - 1; n >= 0; --n)
                        a = a * powers[2][z][1] + angular[n];
                    output[offset + z] += radial[z] * a;
                }
            }
    }
    for (double value : values)
        if (!std::isfinite(value))
            fail("MOLDENの軌道評価が有限値になりません。");
    return values;
}
int frontierOrbital(const QVector<ResultOrbital> &orbitals, const QString &spin, bool lumo) {
    int found = -1;
    for (int i = 0; i < orbitals.size(); ++i) {
        const auto &o = orbitals[i];
        if (o.spin != spin || (lumo ? o.occupation > 1e-8 : o.occupation <= 1e-8))
            continue;
        if (found < 0 || (lumo ? o.energy < orbitals[found].energy : o.energy >= orbitals[found].energy))
            found = i;
    }
    return found;
}
} // namespace mf
