// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_codec.h"
#include "model/i18n.h"
#include "results/result_parsing.h"
#include "results/results.h"
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <cmath>

namespace mf {
namespace result_detail {
QByteArray resultBytes(const ResultData &r) {
    QByteArray bytes;
    QDataStream s(&bytes, QIODevice::WriteOnly);
    s.setVersion(QDataStream::Qt_6_4);
    s << QString("MolFormaResult8") << r.source << r.format << r.status << r.log << r.warnings
      << qint32(r.outcome);
    s << r.orbitalBasis << qint32(r.selectedOrbital) << qint32(r.orbitalFrame) << qint32(r.orbitalDivisions)
      << r.orbitalError << r.orbitalNormalization << r.normalizationRequest;
    auto doc = [&](const Document &d) { s << QJsonDocument(d.toJson()).toJson(QJsonDocument::Compact); };
    auto vec = [&](const Vec &v) { s << v.x() << v.y() << v.z(); };
    s << qint32(r.frames.size());
    for (const auto &f : r.frames) {
        doc(f.molecule);
        s << f.label;
    }
    s << qint32(r.energies.size());
    for (const auto &e : r.energies)
        s << e.method << e.hartree << qint32(e.line);
    s << qint32(r.orbitals.size());
    for (const auto &o : r.orbitals)
        s << o.symmetry << o.spin << o.energy << o.occupation << qint32(o.number);
    doc(r.vibrationGeometry);
    s << qint32(r.modes.size());
    for (const auto &m : r.modes) {
        s << m.frequency << m.intensity << qint32(m.displacement.size());
        for (const auto &v : m.displacement)
            vec(v);
    }
    s << qint32(r.surface.size());
    for (const auto &t : r.surface) {
        vec(t.a);
        vec(t.b);
        vec(t.c);
        s << t.positive;
        for (const auto &normal : {t.na, t.nb, t.nc})
            s << float(normal.x()) << float(normal.y()) << float(normal.z());
    }
    s << r.datasets << qint32(r.dataset) << r.isovalue << r.minimum << r.maximum;
    for (int n : r.grid)
        s << qint32(n);
    s << qint32(r.excitations.size());
    for (const auto &x : r.excitations) {
        s << qint32(x.root) << x.spin << x.symmetry << x.ev << x.oscillator << x.s2
          << qint32(x.contributions.size());
        for (const auto &c : x.contributions)
            s << qint32(c.from) << qint32(c.to) << c.spin << c.coefficient << c.deexcitation;
    }
    s << qint32(r.steps.size());
    for (const auto &t : r.steps)
        s << qint32(t.step) << t.energy << t.deltaE << t.gmax << t.grms << t.xrms << t.xmax << t.seconds;
    s << QJsonDocument(r.properties).toJson(QJsonDocument::Compact);
    vec(r.gridOrigin);
    for (int i = 0; i < 9; ++i)
        s << r.gridAxes.data()[i];
    s << qint32(r.values.size());
    for (double value : r.values)
        s << value;
    if (s.status() != QDataStream::Ok)
        fail(tr("結果の転送データを保存できません。"));
    return bytes;
}
ResultData resultFromBytes(const QByteArray &bytes) {
    QDataStream s(bytes);
    s.setVersion(QDataStream::Qt_6_4);
    QString magic;
    s >> magic;
    if (magic != "MolFormaResult8")
        fail(tr("結果の転送形式が不正です。"));
    ResultData r;
    s >> r.source >> r.format >> r.status >> r.log >> r.warnings;
    qint32 outcome = 0;
    s >> outcome;
    if (outcome < int(ResultOutcome::NotApplicable) || outcome > int(ResultOutcome::Error))
        fail(tr("結果の転送形式が不正です。"));
    r.outcome = ResultOutcome(outcome);
    qint32 orbital, orbitalFrame, divisions;
    s >> r.orbitalBasis >> orbital >> orbitalFrame >> divisions >> r.orbitalError >> r.orbitalNormalization >>
        r.normalizationRequest;
    r.selectedOrbital = orbital;
    r.orbitalFrame = orbitalFrame;
    r.orbitalDivisions = divisions;
    auto count = [&](int max) {
        qint32 n;
        s >> n;
        if (s.status() != QDataStream::Ok || n < 0 || n > max)
            fail(tr("結果の転送件数が不正です。"));
        return n;
    };
    auto doc = [&] {
        QByteArray b;
        s >> b;
        return Document::fromJson(parseJson(b));
    };
    auto vec = [&] {
        double x, y, z;
        s >> x >> y >> z;
        return Vec(x, y, z);
    };
    for (int n = count(maxFrames); n--;) {
        ResultFrame f;
        f.molecule = doc();
        s >> f.label;
        r.frames << f;
    }
    for (int n = count(20000); n--;) {
        ResultEnergy e;
        qint32 line;
        s >> e.method >> e.hartree >> line;
        e.line = line;
        r.energies << e;
    }
    for (int n = count(20000); n--;) {
        ResultOrbital o;
        qint32 number;
        s >> o.symmetry >> o.spin >> o.energy >> o.occupation >> number;
        o.number = number;
        r.orbitals << o;
    }
    r.vibrationGeometry = doc();
    for (int n = count(6000); n--;) {
        ResultMode m;
        s >> m.frequency >> m.intensity;
        for (int k = count(maxAtoms); k--;)
            m.displacement << vec();
        r.modes << m;
    }
    for (int n = count(maxTriangles); n--;) {
        SurfaceTriangle t;
        t.a = vec();
        t.b = vec();
        t.c = vec();
        s >> t.positive;
        for (auto *normal : {&t.na, &t.nb, &t.nc}) {
            float x, y, z;
            s >> x >> y >> z;
            *normal = Vec(x, y, z);
            if (!normal->allFinite())
                normal->setZero();
        }
        r.surface << t;
    }
    qint32 dataset;
    s >> r.datasets >> dataset >> r.isovalue >> r.minimum >> r.maximum;
    r.dataset = dataset;
    for (int &n : r.grid) {
        qint32 value;
        s >> value;
        n = value;
    }
    for (int n = count(2000); n--;) {
        ResultExcitation x;
        qint32 root;
        s >> root >> x.spin >> x.symmetry >> x.ev >> x.oscillator >> x.s2;
        x.root = root;
        for (int k = count(ResultExcitation::maxContributions); k--;) {
            ResultContribution c;
            qint32 from, to;
            s >> from >> to >> c.spin >> c.coefficient >> c.deexcitation;
            c.from = from;
            c.to = to;
            x.contributions << c;
        }
        r.excitations << x;
    }
    for (int n = count(20000); n--;) {
        ResultStep t;
        qint32 step;
        s >> step >> t.energy >> t.deltaE >> t.gmax >> t.grms >> t.xrms >> t.xmax >> t.seconds;
        t.step = step;
        r.steps << t;
    }
    QByteArray properties;
    s >> properties;
    r.properties = QJsonDocument::fromJson(properties).object();
    r.gridOrigin = vec();
    for (int i = 0; i < 9; ++i)
        s >> r.gridAxes.data()[i];
    for (int n = count(maxPoints); n--;) {
        double value;
        s >> value;
        if (!std::isfinite(value))
            fail(tr("結果ファイルの数値が不正です: %1").arg(value));
        r.values.append(value);
    }
    if (s.status() != QDataStream::Ok || !s.atEnd())
        fail(tr("結果の転送データが不完全です。"));
    return r;
}
} // namespace result_detail
using namespace result_detail;
void saveResultData(const QString &path, const ResultData &r) {
    writeFile(path, resultBytes(r));
}
ResultData loadResultData(const QString &path) {
    return resultFromBytes(boundedRead(path));
}

} // namespace mf
