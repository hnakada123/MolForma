// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_parsing.h"
#include "model/i18n.h"
#include "model/numbers.h"
#include "results/results.h"
#include <QFile>
#include <QRegularExpression>
#include <charconv>
#include <cmath>

namespace mf::result_detail {
// Distinct warnings, and not too many to show: a log can repeat the same trouble on every line.
void warn(ResultData &r, const QString &text) {
    if (r.warnings.size() < 100 && !r.warnings.contains(text))
        r.warnings << text;
}
// The words of a line between ASCII whitespace, as a split at "\\s+" gives them (logs have millions).
QStringList words(QStringView line) {
    const auto space = [](QChar c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
    QStringList out;
    for (qsizetype i = 0; i < line.size();) {
        while (i < line.size() && space(line[i]))
            ++i;
        const qsizetype start = i;
        while (i < line.size() && !space(line[i]))
            ++i;
        if (i > start)
            out << line.mid(start, i - start).toString();
    }
    return out;
}
// A number of a result file. Plain decimal numbers (nearly all) are read without a QString; the
// others, such as Fortran exponents ("1.5D-03") or a leading '+', as fortranNumber reads them.
std::optional<double> resultNumber(QStringView text) {
    char ascii[64];
    if (text.size() > 0 && text.size() < qsizetype(sizeof ascii)) {
        qsizetype n = 0;
        while (n < text.size() && text[n].unicode() < 128) {
            ascii[n] = char(text[n].unicode());
            ++n;
        }
        double value;
        if (n == text.size())
            if (const auto [end, error] = std::from_chars(ascii, ascii + n, value);
                error == std::errc() && end == ascii + n)
                return std::isfinite(value) ? std::optional(value) : std::nullopt;
    }
    return fortranNumber(text.toString());
}
double number(QStringView text) {
    if (const auto n = resultNumber(text))
        return *n;
    fail(tr("結果ファイルの数値が不正です: %1").arg(text.left(80)));
}
int integer(QStringView text, int lo, int hi) {
    const double n = number(text);
    if (n < lo || n > hi || n != std::floor(n))
        fail(tr("結果ファイルの整数・個数が範囲外です: %1").arg(text.left(80)));
    return int(n);
}
Vec point(const QStringList &w, int start, double scale) {
    if (w.size() < start + 3)
        fail(tr("結果ファイルの座標が不足しています。"));
    const Vec p(number(w[start]), number(w[start + 1]), number(w[start + 2]));
    if (p.cwiseAbs().maxCoeff() > 1e6)
        fail(tr("結果ファイルの座標が範囲外です。"));
    return p * scale;
}
QString symbol(int z) {
    for (const auto &e : elements())
        if (e.number == z)
            return e.symbol;
    fail(tr("このビューアで扱えない原子番号です: %1").arg(z));
}
Atom atom(QString tag, const Vec &p) {
    Atom a;
    if (tag.startsWith('@')) {
        a.ghost = true;
        tag.remove(0, 1);
    }
    if (tag.startsWith("bq", Qt::CaseInsensitive)) {
        a.ghost = true;
        tag = tag.mid(2);
    }
    static const QRegularExpression symbol("^([A-Za-z]{1,2})[0-9_]*$");
    const auto match = symbol.match(tag);
    if (!match.hasMatch())
        fail(tr("未対応の原子タグです: %1").arg(tag.left(80)));
    tag = match.captured(1).toLower();
    tag[0] = tag[0].toUpper();
    a.element = element(tag).symbol;
    a.position = p;
    return a;
}
void addFrame(ResultData &r, Document d, const QString &label) {
    if (d.atoms.isEmpty() || d.atoms.size() > maxAtoms || r.frames.size() >= maxFrames)
        fail(tr("構造数または原子数が対応範囲外です（2000構造・2000原子まで）。"));
    qsizetype total = d.atoms.size();
    for (const auto &frame : r.frames)
        total += frame.molecule.atoms.size();
    if (total > 200000)
        fail(tr("構造列が大きすぎます（合計20万原子まで）。"));
    d.inferBonds();
    r.frames.append({std::move(d), label});
}
QByteArray boundedRead(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    if (f.size() > maxBytes)
        fail(tr("結果ファイルは128 MiBまで読み込めます。ログを分割するか格子を小さくしてください。"));
    auto data = f.read(maxBytes + 1);
    if (data.size() > maxBytes)
        fail(tr("読込中に結果ファイルが128 MiBを超えました。"));
    return data;
}
double logNumber(const QString &text) {
    return resultNumber(text).value_or(std::numeric_limits<double>::quiet_NaN());
}

} // namespace mf::result_detail
