// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/files.h"
#include "model/units.h"
#include "nwchem/history.h"
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_queue.h"
#include "nwchem/result_files.h"
#include "result_support.h"
#include "results/energy_profile.h"
#include "results/molden_orbitals.h"
#include "results/nwchem_log.h"
#include "results/result_files.h"
#include "results/result_loader.h"
#include "results/result_path.h"
#include "results/result_properties.h"
#include "results/result_summary.h"
#include "results/spectrum.h"
#include <Eigen/Geometry>
#include <Eigen/LU>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
using namespace mf;

namespace {
// Equal JSON, with numbers equal to the last few bits: values computed from the read numbers (such as
// the dipole moment by std::hypot) round differently between C++ libraries (libstdc++, libc++, MSVC).
bool sameReading(const QJsonValue &a, const QJsonValue &b) {
    if (a.isDouble() && b.isDouble()) {
        const auto x = a.toDouble(), y = b.toDouble();
        return x == y || std::abs(x - y) <= 1e-13 * std::max(std::abs(x), std::abs(y));
    }
    if (a.isArray() && b.isArray()) {
        const auto p = a.toArray(), q = b.toArray();
        if (p.size() != q.size())
            return false;
        for (qsizetype i = 0; i < p.size(); ++i)
            if (!sameReading(p[i], q[i]))
                return false;
        return true;
    }
    if (a.isObject() && b.isObject()) {
        const auto p = a.toObject(), q = b.toObject();
        if (p.keys() != q.keys())
            return false;
        for (auto i = p.begin(); i != p.end(); ++i)
            if (!sameReading(i.value(), q[i.key()]))
                return false;
        return true;
    }
    return a == b;
}
} // namespace

// What is read from result files, without a window: NWChem logs (energies, structures, paths, modes,
// spectra, properties), XYZ, CUBE and MOLDEN files with the orbitals computed from them, and the energy
// profiles and thermal corrections made of the readings.
class ResultReadersTest : public QObject, ResultFiles {
    Q_OBJECT
    QTemporaryDir settings;
    static Document hydrogen() {
        return Document::fromXyz("1\nH\nH 0 0 0\n");
    }
    // Everything read from an NWChem log, for comparing readings of real logs: the result, the progress
    // of a running job fed line by line and the energy summary of the history.
    static QJsonObject logDigest(const QString &path) {
        const auto r = readResult(path);
        const auto vector = [](const Vec &v) { return QJsonArray{v.x(), v.y(), v.z()}; };
        QJsonArray energies, steps, frames, orbitals, modes, excitations;
        for (const auto &e : r.energies)
            energies << QJsonArray{e.method, e.hartree, e.line};
        for (const auto &s : r.steps)
            steps << QJsonArray{s.step, s.energy, s.deltaE, s.gmax, s.grms, s.xrms, s.xmax, s.seconds};
        for (const auto &f : r.frames) {
            QJsonArray atoms;
            for (const auto &a : f.molecule.atoms)
                atoms << QJsonArray{a.element, a.position.x(), a.position.y(), a.position.z()};
            frames << QJsonObject{{"label", f.label}, {"atoms", atoms}};
        }
        for (const auto &o : r.orbitals)
            orbitals << QJsonArray{o.spin, o.number, o.energy, o.occupation, o.symmetry};
        for (const auto &m : r.modes) {
            QJsonArray displacement;
            for (const auto &v : m.displacement)
                displacement << vector(v);
            modes << QJsonObject{
                {"frequency", m.frequency}, {"intensity", m.intensity}, {"displacement", displacement}};
        }
        for (const auto &x : r.excitations) {
            QJsonArray contributions;
            for (const auto &c : x.contributions)
                contributions << QJsonArray{c.from, c.to, c.spin, c.coefficient, c.deexcitation ? "Y" : "X"};
            excitations << QJsonArray{x.root, x.spin, x.symmetry, x.ev, x.oscillator, x.s2, contributions};
        }
        NwchemLog progress; // As the run panel feeds it.
        for (const auto &line : readFile(path).split('\n'))
            progress.addBytes(line);
        return {{"status", r.status},
                {"outcome", int(r.outcome)},
                {"warnings", QJsonArray::fromStringList(r.warnings)},
                {"properties", r.properties},
                {"energies", energies},
                {"steps", steps},
                {"frames", frames},
                {"orbitals", orbitals},
                {"vibration_atoms", r.vibrationGeometry.atoms.size()},
                {"modes", modes},
                {"excitations", excitations},
                {"progress", progressDigest(progress)},
                {"history", nwchemLogSummary(path)}};
    }
    static QJsonObject progressDigest(const NwchemLog &progress) {
        QJsonArray steps;
        for (const auto &s : progress.optimization().steps)
            steps << QJsonArray{s.step, s.energy};
        return {{"started", progress.started()},
                {"step_task", progress.optimization().steps.isEmpty() ? -1 : progress.optimization().task},
                {"converged_task", (progress.optimization().converged ? progress.optimization().task : -1)},
                {"steps", steps},
                {"thresholds", progress.optimization().thresholds},
                {"energy", progress.energy()},
                {"ended", progress.ended()}};
    }
    // A result as transferred to the window, with the atom IDs of each frame replaced by their numbers
    // (readings of the same file get new IDs).
    QByteArray transferred(ResultData r) {
        const auto canonical = [](Document &d) {
            QHash<QString, QString> renamed;
            for (int i = 0; i < d.atoms.size(); ++i)
                renamed[d.atoms[i].id] = QString("a%1").arg(i);
            d.renameIds(renamed);
            d.data["id"] = "x";
        };
        for (auto &f : r.frames)
            canonical(f.molecule);
        canonical(r.vibrationGeometry);
        const auto path = files.filePath("transferred.bin");
        saveResultData(path, r);
        return readFile(path);
    }
    static QVector<double> recordVibrations(const QJsonObject &record) {
        QVector<double> wavenumbers;
        for (const auto &value : record["vibrations"].toArray())
            wavenumbers << value.toDouble();
        return wavenumbers;
    }
  private slots:
    void initTestCase() {
        QVERIFY(files.isValid());
        QVERIFY(settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope,
                           settings.path()); // MolForma uses NativeFormat.
        qputenv("XDG_CONFIG_HOME", settings.path().toUtf8());
        qputenv("XDG_DATA_HOME", settings.filePath("data").toUtf8());
    }
    void cachedReadings() {
        // The loader's session cache gives what reading the file again gives: for another isovalue,
        // dataset or orbital, and after the file changed or its entries were damaged.
        QTemporaryDir cache;
        QVERIFY(cache.isValid());
        QStringList different;
        const auto both = [&](const QString &path, double iso, int dataset, int orbital = -1) {
            const auto fresh = readResult(path, iso, dataset, orbital);
            if (transferred(readResult(path, iso, dataset, orbital, "auto", 40, cache.path())) !=
                transferred(fresh))
                different << QString("%1 %2 %3 %4")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(iso)
                                 .arg(dataset)
                                 .arg(orbital);
            return fresh;
        };
        const auto cube = save("cached.cube", cubeText(true));
        both(cube, .25, 1);
        const int entries = QDir(cache.path()).entryList(QDir::Files).size();
        QCOMPARE(entries, 1);
        both(cube, .1, 1);
        QCOMPARE(QDir(cache.path()).entryList(QDir::Files).size(), entries); // Read from the entry.
        both(cube, .25, 0);
        const auto molden =
            save("cached.molden", "[Molden Format]\n[Atoms] AU\nH 1 1 0 0 0\nH 2 1 0 0 1.4\n"
                                  "[GTO]\n1 0\ns 1 1.0\n1.0 1.0\n\n2 0\ns 1 1.0\n1.0 1.0\n\n"
                                  "[MO]\nSym= a\nEne= -0.5\nSpin= Alpha\nOccup= 2\n1 0.5\n2 0.5\n"
                                  "Sym= b\nEne= 0.3\nSpin= Alpha\nOccup= 0\n1 0.7\n2 -0.7\n");
        QCOMPARE(both(molden, .03, 0).selectedOrbital, 0);
        QCOMPARE(both(molden, .05, 0, 1).selectedOrbital, 1);
        both(molden, .1, 0, 1);
        both(molden, .03, 0);
        // A rewritten file is read again, and damaged entries are only read around.
        QTest::qWait(5);
        save("cached.molden", readFile(molden).replace("Ene= -0.5", "Ene= -0.6"));
        QCOMPARE(both(molden, .03, 0).orbitals[0].energy, -.6);
        for (const auto &name : QDir(cache.path()).entryList(QDir::Files))
            writeFile(QDir(cache.path()).filePath(name), "damaged");
        both(molden, .05, 0, 1);
        both(cube, .25, 1);
        QVERIFY2(different.isEmpty(), qPrintable(different.join(", ")));
    }
    void progressFromBytes() {
        // The run panel passes over the lines that change no progress without decoding them: it reads
        // the same progress and energy summary as from the whole text.
        const QDir dir(FIXTURE_DIR "/nwchem");
        for (const auto &log : dir.entryList({"*.out"})) {
            NwchemLog text, bytes;
            for (const auto &line : readFile(dir.filePath(log)).split('\n')) {
                text.add(QString::fromUtf8(line));
                bytes.addBytes(line);
            }
            QCOMPARE(progressDigest(bytes), progressDigest(text));
            QCOMPARE(bytes.summary().energy(), text.summary().energy());
            QCOMPARE(bytes.summary().bsse(), text.summary().bsse());
            QCOMPARE(bytes.summary().normalTermination(), text.summary().normalTermination());
        }
        // Total energies are found in any case; other lines are passed over.
        NwchemLog log;
        log.addBytes("         Total DFT ENERGY =     -76.4");
        log.addBytes("  iter       energy          gnorm     gmax       time");
        QCOMPARE(log.energy(), -76.4);
    }
    void logLinesAreReadOnce() {
        // A Raman task after the frequencies keeps the energy of their reference structure, whether the
        // lines come as text or as bytes (the lines passed over are those the summary does not use).
        const QByteArray raman = "NWChem Input Module\nTotal DFT energy = -76.0\nNWChem Input Module\n"
                                 "NWChem Nuclear Hessian and Frequency Analysis\nTotal DFT energy = -75.9\n"
                                 "Total DFT energy = -75.85\nNWChem Input Module\n      Raman Analysis\n"
                                 "Total DFT energy = -75.8\n"
                                 " Total times  cpu:        1.2s     wall:        3.4s\n";
        NwchemLog text, bytes;
        for (const auto &line : raman.split('\n')) {
            text.add(QString::fromUtf8(line));
            bytes.addBytes(line);
        }
        QCOMPARE(text.summary().energy()["hartree"].toDouble(), -75.9);
        QCOMPARE(text.summary().energy()["source"].toString(), QString("frequency_reference"));
        QCOMPARE(bytes.summary().energy(), text.summary().energy());
        QCOMPARE(bytes.energy(), -75.8); // The last one printed, as a running job shows it.
        // The closing line ends the log, for the progress and the summary alike, and gives the wall time.
        QVERIFY(bytes.ended() && bytes.summary().normalTermination());
        QCOMPARE(bytes.summary().wallSeconds(), 3.4);
        NwchemEnergySummary summary;
        summary.add(" Total times  cpu: 1.0s");
        QVERIFY(summary.normalTermination() && !summary.wallSeconds());
        // A total energy is one in any case of its letters, for all that read it.
        summary = {};
        summary.add("         Total DFT ENERGY =     -76.4");
        QCOMPARE(summary.energy()["hartree"].toDouble(), -76.4);
        QVERIFY(NwchemEnergySummary::relevant("         Total DFT ENERGY =     -76.4"));
        QVERIFY(NwchemEnergySummary::relevant("      Raman Analysis"));
        QVERIFY(!NwchemEnergySummary::relevant("  iter       gnorm     gmax       time"));
        // And one whose number does not read is none, for the result as for the progress: an odd line
        // does not reject the log.
        const QString odd = "NWChem\n" + geometry() +
                            " Total SCF energy = -75.5\n Total SCF energy = 1.2.3\n Total DFT energy e\n";
        const auto r = readResult(save("odd-energy.out", odd));
        QCOMPARE(r.energies.size(), 1);
        QCOMPARE(r.energies[0].hartree, -75.5);
        NwchemLog live;
        for (const auto &line : odd.toUtf8().split('\n'))
            live.addBytes(line);
        QCOMPARE(live.energy(), -75.5);
        QCOMPARE(live.summary().energy()["hartree"].toDouble(), -75.5);
        // What a file of NWChem begins with, and the failures its messages tell apart.
        QVERIFY(nwchemOutputStart("          Northwest Computational Chemistry Package (NWChem) 7.3.1\n"));
        QVERIFY(nwchemOutputStart(" argument  1 = input.nw\n\n                NWChem Input Module\n"));
        QVERIFY(!nwchemOutputStart("3\nwater\nO 0 0 0\n"));
        const auto reports = [](const char *line) {
            QVector<NwchemFailure> found;
            for (const auto failure : nwchemFailures)
                if (nwchemLineReports(line, failure))
                    found << failure;
            return found;
        };
        QCOMPARE(reports(" dft energy failed"), (QVector{NwchemFailure::ScfConvergence}));
        QCOMPARE(reports("SCF failed to converge in 30 steps"),
                 (QVector{NwchemFailure::ScfConvergence, NwchemFailure::OptimizationLimit}));
        QCOMPARE(reports("MA_alloc_get: failed"), (QVector{NwchemFailure::Memory}));
        QCOMPARE(reports("failed opening basis file"), (QVector{NwchemFailure::Basis}));
        QVERIFY(reports("SCF converged").isEmpty());
    }
    void realNwchemLogs() {
        // Logs of NWChem 7.3.1 for inputs MolForma generated (tests/fixtures/nwchem/README.md), and what
        // MolForma reads from them. MOLFORMA_UPDATE_GOLDEN=1 records the readings anew.
        const QDir dir(FIXTURE_DIR "/nwchem");
        const auto logs = dir.entryList({"*.out"});
        QVERIFY(logs.size() >= 11);
        for (const auto &log : logs) {
            // As written to the file: values that are not numbers (NaN) become null.
            const auto digest = parseJson(QJsonDocument(logDigest(dir.filePath(log))).toJson());
            const auto golden = dir.filePath(QFileInfo(log).completeBaseName() + ".json");
            if (qEnvironmentVariableIsSet("MOLFORMA_UPDATE_GOLDEN")) {
                writeFile(golden, QJsonDocument(digest).toJson());
                continue;
            }
            const auto expected = parseJson(readFile(golden));
            for (const auto &key : expected.keys())
                QVERIFY2(sameReading(digest[key], expected[key]), qPrintable(log + ": " + key));
            QCOMPARE(digest.keys(), expected.keys());
        }
    }
    void nebPathOfLog() {
        // NWChem 7.3.1's NEB of HCN → HNC (tests/fixtures/nwchem/hcn_neb.out): the path after the last
        // step, with the energies at full precision and the path length; the step limit is no error.
        const auto r = readResult(FIXTURE_DIR "/nwchem/hcn_neb.out");
        const auto neb = r.properties["neb"].toObject();
        const auto points = neb["points"].toArray();
        QCOMPARE(points.size(), 5);
        QCOMPARE(neb["iteration"].toInt(), 3);
        QCOMPARE(neb["converged"].toBool(true), false);
        QCOMPARE(neb["steps"].toArray().size(), 3);
        QCOMPARE(neb["thresholds"].toObject()["gmax"].toDouble(), 0.00045);
        QCOMPARE(points[2].toObject()["energy"].toDouble(), -90.12077085630793);
        double length = -1;
        for (int k = 0; k < points.size(); ++k) {
            const auto p = points[k].toObject();
            QVERIFY(p["distance"].toDouble() > length);
            length = p["distance"].toDouble();
            const auto &frame = r.frames[p["frame"].toInt()];
            QCOMPARE(frame.label, QString("NEB 像 %1").arg(k + 1));
            QCOMPARE(frame.molecule.atoms.size(), 3);
        }
        QVERIFY((r.frames[points[4].toObject()["frame"].toInt()].molecule.atoms[2].position -
                 Vec(0.8, 0, 1.849999))
                    .norm() < 1e-9);
        QCOMPARE(resultOutcome(r), QString("normal"));
        QVERIFY(r.warnings.join('\n').contains("収束していません"));
        // The paths of every step that NWChem also writes to files are left out of the job's files.
        QTemporaryDir job;
        for (const auto *name : {"output.out", "hcn.nebpath_000001.xyz", "hcn.neb_final.xyz"})
            writeFile(QDir(job.path()).filePath(name), "x");
        QCOMPARE(nwchemResultFiles(job.path()), QStringList({"hcn.neb_final.xyz", "output.out"}));
        QVERIFY(resultFiles(job.path()).contains("hcn.nebpath_000001.xyz"));
    }
    void chainedStructuresOfLogs() {
        // The structures a queued calculation can take from a job: the final one, the highest point of a
        // path (an NEB's fixed ends aside) and the ends of an IRC.
        const auto job = [](const QString &log) {
            auto *dir = new QTemporaryDir;
            QFile::copy(FIXTURE_DIR "/nwchem/" + log, dir->filePath("output.out"));
            return std::unique_ptr<QTemporaryDir>(dir);
        };
        const auto neb = job("hcn_neb.out");
        const auto nebResult = readResult(neb->filePath("output.out"));
        auto start = nebResult.frames.first().molecule;
        const auto highest = chainedDocument(start, neb->path(), "highest");
        QCOMPARE(highest.ids(), start.ids());
        QVERIFY((highest.atoms[2].position - Vec(-0.050003, 0, 0.575004)).norm() < 1e-9);
        QVERIFY(
            (chainedDocument(start, neb->path(), "final").atoms[2].position - Vec(0.8, 0, 1.849999)).norm() <
            1e-9);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, chainedDocument(start, neb->path(), "irc_forward"));
        const auto irc = job("nh3_irc.out");
        const auto ircResult = readResult(irc->filePath("output.out"));
        const auto points = ircResult.properties["irc"].toObject()["points"].toArray();
        int forward = -1, backward = -1;
        for (const auto &value : points) {
            const auto p = value.toObject();
            (p["direction"] == "forward" ? forward : backward) = p["frame"].toInt();
        }
        QVERIFY(forward >= 0 && backward >= 0);
        start = ircResult.frames.first().molecule;
        QVERIFY((chainedDocument(start, irc->path(), "irc_forward").atoms[0].position -
                 ircResult.frames[forward].molecule.atoms[0].position)
                    .norm() < 1e-12);
        QVERIFY((chainedDocument(start, irc->path(), "irc_backward").atoms[1].position -
                 ircResult.frames[backward].molecule.atoms[1].position)
                    .norm() < 1e-12);
        // The transition state is the highest point of an IRC.
        const int ts = ircResult.properties["irc"].toObject()["ts_frame"].toInt();
        QVERIFY((chainedDocument(start, irc->path(), "highest").atoms[1].position -
                 ircResult.frames[ts].molecule.atoms[1].position)
                    .norm() < 1e-12);
        auto other = start;
        other.atoms[0].element = "P";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, chainedDocument(other, irc->path(), "final"));
        QTemporaryDir empty;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, chainedDocument(start, empty.path(), "final"));
    }
    void reactionPaths() {
        using Role = ResultPathPoint::Role;
        // No path: nothing to show, nothing highest.
        QCOMPARE(resultPath(ResultData()).kind, ResultPathKind::None);
        QVERIFY(resultPath(ResultData()).points.isEmpty());
        QCOMPARE(resultPath(ResultData()).highest(PathEnds::Included), -1);
        // A scan: its points in their order, against the lowest; highest at an end unless the ends are
        // left out.
        ResultData scan;
        scan.properties["scan"] = QJsonArray{
            QJsonObject{
                {"point", 1}, {"coordinate", "bond 1 2"}, {"value", 0.9}, {"energy", -76.0}, {"frame", 0}},
            QJsonObject{
                {"point", 2}, {"coordinate", "bond 1 2"}, {"value", 1.0}, {"energy", -76.3}, {"frame", 1}},
            QJsonObject{{"point", 3}, {"coordinate", "bond 1 2"}, {"value", 1.1}, {"energy", -76.2}},
            QJsonObject{
                {"point", 4}, {"coordinate", "bond 1 2"}, {"value", 1.2}, {"energy", -76.1}, {"frame", 2}}};
        auto path = resultPath(scan);
        QCOMPARE(path.kind, ResultPathKind::Scan);
        QVERIFY(!path.plane);
        QCOMPARE(path.points.size(), 4);
        QCOMPARE(path.points[1].number, 2);
        QCOMPARE(path.points[1].x, 1.0);
        QCOMPARE(path.points[1].coordinate, QString("bond 1 2"));
        QCOMPARE(path.points[2].frame, -1);
        QVERIFY(std::isnan(path.points[1].y));
        QCOMPARE(path.reference, -76.3);
        QCOMPARE(path.relative(1), 0.0);
        QVERIFY(std::abs(path.relative(0) - 0.3) < 1e-12);
        QCOMPARE(path.highest(PathEnds::Included), 0);
        QCOMPARE(path.highest(PathEnds::Excluded), 2);
        QCOMPARE(path.highest(PathEnds::Excluded, true), 1); // Point 3 has no structure.
        // Two coordinates make a plane.
        ResultData plane;
        plane.properties["scan"] = QJsonArray{QJsonObject{{"point", 1},
                                                          {"coordinate", "bond 1 2"},
                                                          {"value", 0.9},
                                                          {"coordinate2", "angle 2 1 3"},
                                                          {"value2", 104.5},
                                                          {"energy", -76.0}}};
        path = resultPath(plane);
        QVERIFY(path.plane);
        QCOMPARE(path.points[0].y, 104.5);
        QCOMPARE(path.points[0].coordinate2, QString("angle 2 1 3"));
        QCOMPARE(path.highest(PathEnds::Excluded), -1);
        // An NEB: from the start, whose energy is the reference; the ends never count as its highest
        // point even where they are, and equal energies give the first.
        ResultData neb;
        neb.properties["neb"] = QJsonObject{
            {"iteration", 4},
            {"points",
             QJsonArray{QJsonObject{{"bead", 1}, {"energy", -1.0}, {"frame", 5}, {"distance", 0.0}},
                        QJsonObject{{"bead", 2}, {"energy", -1.2}, {"frame", 6}, {"distance", 0.4}},
                        QJsonObject{{"bead", 3}, {"energy", -1.1}, {"frame", 7}, {"distance", 0.9}},
                        QJsonObject{{"bead", 4}, {"energy", -1.1}, {"frame", 8}, {"distance", 1.5}},
                        QJsonObject{{"bead", 5}, {"energy", -0.5}, {"frame", 9}, {"distance", 2.0}}}}};
        path = resultPath(neb);
        QCOMPARE(path.kind, ResultPathKind::Neb);
        QVERIFY(path.lengths);
        QCOMPARE(path.points.first().role, Role::Start);
        QCOMPARE(path.points.last().role, Role::End);
        QCOMPARE(path.points[2].role, Role::Point);
        QCOMPARE(path.points[3].x, 1.5);
        QCOMPARE(path.points[3].frame, 8);
        QCOMPARE(path.reference, -1.0);
        QCOMPARE(path.highest(PathEnds::Excluded), 2);
        QCOMPARE(path.highest(PathEnds::Included), 4);
        // Energies without structures: the images by their number.
        ResultData bare;
        bare.properties["neb"] =
            QJsonObject{{"points", QJsonArray{QJsonObject{{"bead", 1}, {"energy", -1.0}},
                                              QJsonObject{{"bead", 2}, {"energy", -0.9}},
                                              QJsonObject{{"bead", 3}, {"energy", -1.0}}}}};
        path = resultPath(bare);
        QVERIFY(!path.lengths);
        QCOMPARE(path.points[1].x, 2.0);
        QCOMPARE(path.highest(PathEnds::Excluded), 1);
        QCOMPARE(path.highest(PathEnds::Excluded, true), -1);
        // An IRC: the backward points reversed, the transition state (the reference, at 0), the forward
        // points; the log lists forward first.
        ResultData irc;
        const auto point = [](const char *direction, int number, double s, double energy, int frame) {
            return QJsonObject{
                {"direction", direction}, {"point", number}, {"s", s}, {"energy", energy}, {"frame", frame}};
        };
        QJsonObject properties{
            {"points", QJsonArray{point("forward", 1, 0.1, -2.1, 1), point("forward", 2, 0.2, -2.3, 2),
                                  point("backward", 1, 0.1, -2.2, 3), point("backward", 2, 0.2, -2.4, 4)}},
            {"ts_energy", -2.0},
            {"ts_frame", 0}};
        irc.properties["irc"] = properties;
        path = resultPath(irc);
        QCOMPARE(path.kind, ResultPathKind::Irc);
        QVector<int> frames;
        QVector<double> places;
        for (const auto &p : path.points) {
            frames << p.frame;
            places << p.x;
        }
        QCOMPARE(frames, QVector<int>({4, 3, 0, 1, 2}));
        QCOMPARE(places, QVector<double>({-0.2, -0.1, 0, 0.1, 0.2}));
        QCOMPARE(path.points[0].role, Role::Backward);
        QCOMPARE(path.points[0].number, 2);
        QCOMPARE(path.points[2].role, Role::TransitionState);
        QCOMPARE(path.points[4].role, Role::Forward);
        QCOMPARE(path.reference, -2.0);
        QCOMPARE(path.highest(PathEnds::Included), 2);
        QCOMPARE(path.highest(PathEnds::Excluded), 2);
        // Without the transition state the first point of the path is the reference.
        properties.remove("ts_energy");
        properties.remove("ts_frame");
        irc.properties["irc"] = properties;
        path = resultPath(irc);
        QCOMPARE(path.points.size(), 4);
        QCOMPARE(path.reference, -2.4);
        QCOMPARE(path.highest(PathEnds::Included), 2); // Forward 1.
        // The viewer shows a scan before an NEB before an IRC; each kind can still be asked for.
        irc.properties["neb"] = neb.properties["neb"];
        QCOMPARE(resultPath(irc).kind, ResultPathKind::Neb);
        QCOMPARE(resultPath(irc, ResultPathKind::Irc).points.size(), 4);
        QCOMPARE(resultPath(irc, ResultPathKind::Scan).kind, ResultPathKind::None);
        irc.properties["scan"] = scan.properties["scan"];
        QCOMPARE(resultPath(irc).kind, ResultPathKind::Scan);
        // NWChem's logs: the NEB of HCN (five images), the IRC of ammonia through its transition state.
        const auto hcn = readResult(FIXTURE_DIR "/nwchem/hcn_neb.out");
        path = resultPath(hcn);
        QCOMPARE(path.kind, ResultPathKind::Neb);
        QCOMPARE(path.points.size(), 5);
        QCOMPARE(path.highest(PathEnds::Excluded), 2);
        QCOMPARE(path.points[2].energy, -90.12077085630793);
        const auto ammonia = readResult(FIXTURE_DIR "/nwchem/nh3_irc.out");
        path = resultPath(ammonia);
        QCOMPARE(path.kind, ResultPathKind::Irc);
        const int ts = path.highest(PathEnds::Included, true);
        QVERIFY(ts > 0 && ts + 1 < path.points.size());
        QCOMPARE(path.points[ts].role, Role::TransitionState);
        QCOMPARE(path.points[ts].frame, ammonia.properties["irc"].toObject()["ts_frame"].toInt());
        for (int i = 0; i < path.points.size(); ++i) {
            QCOMPARE(path.points[i].role, i < ts   ? Role::Backward
                                          : i > ts ? Role::Forward
                                                   : Role::TransitionState);
            if (i > 0)
                QVERIFY(path.points[i].x > path.points[i - 1].x);
            QVERIFY(path.relative(i) <= 0);
        }
    }
    void logGeometryEnergyAndFailure() {
        auto source = "NWChem\n" + geometry() +
                      " Total SCF energy = -7.4945628203255D+01\n"
                      " Total MP2 energy  -75.0\n Total CCSD(T) energy: -75.1\n"
                      " Optimization converged\n Total times  cpu: 1.0s wall: 1.0s\n";
        auto r = readResult(save("normal.out", source));
        QCOMPARE(r.frames.size(), 1);
        QCOMPARE(r.frames[0].molecule.atoms.size(), 3);
        QCOMPARE(r.frames[0].molecule.bonds.size(), 2);
        QCOMPARE(r.energies.size(), 3);
        QVERIFY(std::abs(r.energies[0].hartree + 74.945628203255) < 1e-12);
        QCOMPARE(r.energies[2].method, "CCSD(T)");
        QVERIFY(r.status.contains("正常終了マーカーあり"));
        QCOMPARE(resultOutcome(r), QString("normal"));
        r = readResult(save("failed.out", source + "An error occurred\nMPI_ABORT\n"));
        QVERIFY(r.status.contains("エラー"));
        QCOMPARE(resultOutcome(r), QString("error"));
        // NWChem 7.3 errquit block (here: an unknown basis name), without any generic error words.
        const QString dashes(" " + QString(72, '-') + "\n");
        r = readResult(save("errquit.out",
                            "NWChem\n" + geometry() + dashes +
                                " bas_tag_lib: failed opening basis file                   0\n" + dashes +
                                dashes + "  current input line : \n" + "    11: task scf energy\n" + dashes +
                                dashes + " There is an error in the specified basis set\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: bas_tag_lib: failed opening basis file"));
        QCOMPARE(resultOutcome(r), QString("error"));
        r = readResult(save("optfail.out", "NWChem\n" + geometry() + dashes +
                                               " scf optimize failed                                    0\n" +
                                               dashes + dashes + "  current input line : \n" +
                                               "    17: task scf optimize\n" + dashes));
        QCOMPARE(r.status, QString("NWChemエラー: scf optimize failed"));
        r = readResult(save("partial.out", "NWChem\n" + geometry() + geometry().chopped(35)));
        QCOMPARE(r.frames.size(), 1);
        QVERIFY(r.status.contains("マーカーなし"));
        QCOMPARE(resultOutcome(r), QString("incomplete"));
        QVERIFY(!r.warnings.isEmpty());
        auto au = geometry();
        au.replace("in angstroms", "in a.u.");
        r = readResult(save("bohr.log", "NWChem\n" + au));
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.x() - .758 * BohrToAngstrom) < 1e-12);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("not.log", "hello")));
    }
    void projectedModes() {
        QString s = "NWChem\n" + geometry() +
                    " (Projected Frequencies expressed in cm-1)\n\n P.Frequency -100 1600\n\n";
        for (int i = 0; i < 9; ++i)
            s += QString(" %1 %2 %3\n").arg(i + 1).arg(i == 0 ? 1 : 0).arg(i == 4 ? .5 : 0);
        s += "\n Normal Eigenvalue || Projected Infra Red Intensities\n Mode [cm**-1] || units units units "
             "units\n ---- ---- || ---- ---- ---- ----\n"
             " 1 -100 || 0.1 0.2 5.5 1.0\n 2 1600 || 0.2 0.3 8.5 1.2\n ----\n";
        const auto r = readResult(save("modes.out", s));
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 8.5);
        const auto moved = resultModeGeometry(r, 0, std::numbers::pi / 2, .3);
        QVERIFY(std::abs(moved.atoms[0].position.x() - .3) < 1e-12);
        QCOMPARE(r.vibrationGeometry.atoms[0].position.x(), 0.0);
        // NWChem prints six modes per table: a longer line is skipped before anything is allocated.
        auto wide = "NWChem\n" + geometry() + " (Projected Frequencies expressed in cm-1)\n\n P.Frequency";
        for (int i = 0; i < 20000; ++i)
            wide += " 1";
        const auto skipped = readResult(save("wide.out", wide + "\n"));
        QVERIFY(skipped.modes.isEmpty());
        QVERIFY(skipped.warnings.contains("未完の振動モード表を読み飛ばしました。"));
    }
    void repeatedTroubleIsReportedBriefly() {
        // A log repeating the same trouble gives each warning once, and a hundred at most.
        auto s = "NWChem\n" + geometry();
        for (int i = 0; i < 2000; ++i)
            s += " Output coordinates in parsecs\n Output coordinates in angstroms\n";
        // Repeated ESP headings without rows do not keep sections waiting for thousands of lines.
        for (int i = 0; i < 20000; ++i)
            s += "    Atom              Coordinates                           Charge\n";
        const auto r = readResult(save("noisy.out", s));
        QCOMPARE(r.frames.size(), 1);
        QVERIFY(r.warnings.size() <= 100);
        QCOMPARE(r.warnings.count("未対応の座標単位を読み飛ばしました。"), 1);
    }
    void xyzAndMolden() {
        auto r = readResult(save("path.xyz", "2\n\nH 0 0 0\nH 0 0 1\n2\nsecond\nH 0 0 0\nH 0 0 2\n"));
        QCOMPARE(r.frames.size(), 2);
        QCOMPARE(r.frames[1].label, "second");
        QVERIFY(resultOutcome(r).isEmpty()); // Only logs have an outcome.
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 readResult(save("truncated.xyz", "2\ntitle\nH 0 0 0\n")));
        r = readResult(save("orbitals.molden", moldenText()));
        QCOMPARE(r.orbitals.size(), 2);
        QCOMPARE(r.orbitals[0].energy, -.5);
        QCOMPARE(r.orbitals[1].spin, "Beta");
        QCOMPARE(r.orbitals[0].number, 1);
        QCOMPARE(r.orbitals[1].number, 1); // Counted within each spin.
        QCOMPARE(r.modes.size(), 2);
        QCOMPARE(r.modes[1].intensity, 20.0);
        QVERIFY(std::abs(r.frames[0].molecule.atoms[1].position.z() - 1.4 * BohrToAngstrom) < 1e-12);
        auto bad = moldenText();
        bad.replace("[Atoms] AU", "[Atoms] unknown");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("bad.molden", bad)));
        bad = moldenText();
        bad.replace("Occup= 2.0", "Occup= nan");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("nan.molden", bad)));
        bad = moldenText();
        bad.replace("vibration 2\n0 0 .1\n0 0 -.1", "vibration 2\n0 0 .1");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("partial.molden", bad)));
    }
    void affineCubeAndDatasets() {
        const auto path = save("affine.cube", cubeText());
        auto r = readResult(path, .25);
        QCOMPARE(r.frames[0].molecule.atoms[0].element, "C");
        QVERIFY(!r.surface.isEmpty());
        Mat axes;
        axes << 1, .2, 0, 0, 1, .3, 0, 0, 1;
        axes *= BohrToAngstrom;
        const Vec origin = Vec(1, 2, 3) * BohrToAngstrom;
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.x() - (t.positive ? .75 : .25)) < 1e-10);
            }
        r = readResult(save("orbitals.cube", cubeText(true)), .25, 1);
        QCOMPARE(r.datasets, QVector<int>({5, 6}));
        for (const auto &t : r.surface)
            for (const Vec &p : {t.a, t.b, t.c}) {
                Vec grid = axes.inverse() * (p - origin);
                QVERIFY(std::abs(grid.y() - (t.positive ? .75 : .25)) < 1e-10);
            }
        auto nval = cubeText(true);
        nval.replace("-1 1 2 3", "1 1 2 3 2");
        nval.replace("2 5 6\n", "");
        QCOMPARE(readResult(save("channels.cube", nval), .25, 1).datasets, QVector<int>({1, 2}));
        QVERIFY(readResult(path, 10).surface.isEmpty());
        for (const auto &bad :
             {cubeText() + "1", cubeText().chopped(8), QString(cubeText()).replace("2 1 0 0", "-2 1 0 0"),
              QString(cubeText()).replace("2 1 0 0", "2000000 1 0 0"),
              QString(cubeText()).replace("0.5", "nan")})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("invalid.cube", bad)));
    }
    void heavyElementResults() {
        for (const auto &symbol : {"Fr", "U", "Nh", "Og"}) {
            const int z = element(symbol).number;
            auto cube = cubeText();
            cube.replace("6 4 1 2 3", QString("%1 %1 1 2 3").arg(z));
            const auto c = readResult(save("heavy.cube", cube), .25);
            QCOMPARE(c.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto molden = QString("[Molden Format]\n[Atoms] Angs\n%1 1 %2 0 0 0\n").arg(symbol).arg(z);
            const auto m = readResult(save("heavy.molden", molden));
            QCOMPARE(m.frames[0].molecule.atoms[0].element, QString(symbol));
            const auto log = QString("NWChem\n Output coordinates in angstroms\n\n"
                                     " No. Tag Charge X Y Z\n ---- ---- ------ ----- ----- -----\n"
                                     " 1 %1 %2.0 0.0 0.0 0.0\n\n")
                                 .arg(symbol)
                                 .arg(z);
            const auto l = readResult(save("heavy.out", log));
            QCOMPARE(l.frames[0].molecule.atoms[0].element, QString(symbol));
        }
        auto invalid = cubeText();
        invalid.replace("6 4 1 2 3", "119 119 1 2 3");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(save("unknown.cube", invalid)));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            readResult(save("unknown.molden", "[Molden Format]\n[Atoms] Angs\nUue 1 119 0 0 0\n")));
    }
    void optimizationStateFollowsItsTask_data() {
        QTest::addColumn<QString>("next");
        QTest::addColumn<bool>("converged");
        QTest::addColumn<int>("steps");
        QTest::addColumn<double>("energy");
        QTest::addColumn<int>("thresholds");
        const QString step = "@ 0 -75.0 0.0 0.06 0.02 0.0 0.0 0.3\n";
        QTest::newRow("another-optimization")
            << "NWChem Input Module\nNWChem Geometry Optimization\n" + step << false << 1 << -75.0 << 0;
        QTest::newRow("without-optimizer-banner")
            << "NWChem Input Module\n" + step << false << 1 << -75.0 << 0;
        QTest::newRow("cut-before-first-step")
            << QString("NWChem Input Module\nNWChem Geometry Optimization\n") << false << 0 << 0.0 << 0;
        QTest::newRow("new-thresholds")
            << "NWChem Input Module\nmaximum gradient threshold (gmax) = 0.001\n" + step << false << 1
            << -75.0 << 1;
        QTest::newRow("new-step-zero-without-input-banner") << step << false << 1 << -75.0 << 2;
        QTest::newRow("later-frequencies")
            << QString("NWChem Input Module\nFrequency Analysis\nTotal DFT energy = -75.5\n") << true << 1
            << -76.0 << 2;
        QTest::newRow("repeated-converged-step-zero")
            << QString("@ 0 -76.0 0.0 0.001 0.001 0.0 0.0 0.3\n") << true << 1 << -76.0 << 2;
    }
    void optimizationStateFollowsItsTask() {
        QFETCH(QString, next);
        QFETCH(bool, converged);
        QFETCH(int, steps);
        QFETCH(double, energy);
        QFETCH(int, thresholds);
        const QString text = "NWChem Input Module\nNWChem Geometry Optimization\n" + geometry() +
                             "maximum gradient threshold (gmax) = 0.00045\n"
                             "rms gradient threshold (grms) = 0.0003\n"
                             "@ 0 -76.0 0.0 0.001 0.001 0.0 0.0 0.3\n"
                             "Optimization converged\n" +
                             next;
        const auto result = readResult(save("successive-optimizations.out", text));
        NwchemLog live;
        for (const auto &line : text.toUtf8().split('\n'))
            live.addBytes(line);
        QCOMPARE(live.optimization().converged, converged);
        QCOMPARE(live.optimization().steps.size(), steps);
        QCOMPARE(result.steps, live.optimization().steps);
        QCOMPARE(result.properties["thresholds"].toObject(), live.optimization().thresholds);
        QCOMPARE(live.optimization().thresholds.size(), thresholds);
        QCOMPARE(result.properties["optimization"].toString(),
                 converged ? QString("converged") : QString("not_converged"));
        QCOMPARE(result.warnings.contains("最適化の収束メッセージを検出しました。"), converged);
        QVERIFY(
            resultBriefSummary(result).contains(converged ? "最適化: 収束" : "最適化: 収束メッセージなし"));
        if (steps)
            QCOMPARE(result.steps.last().energy, energy);
    }
    void logSummaries() {
        // Excerpts in the layout NWChem 7.3.1 prints (TDDFT, DRIVER, vib, multipole and MO analysis).
        const QString log =
            "NWChem\n" + geometry() +
            " maximum gradient threshold         (gmax) =   0.000450\n"
            " rms gradient threshold             (grms) =   0.000300\n"
            "@ Step       Energy      Delta E   Gmax     Grms     Xrms     Xmax   Walltime\n"
            "@ ---- ---------------- -------- -------- -------- -------- -------- --------\n"
            "@    0     -76.00723755  0.0D+00  0.06730  0.02421  0.00000  0.00000      0.3\n"
            "@    1     -76.01098000 -3.7D-03  0.00835  0.00348  0.02135  0.04987      0.7\n"
            "      Optimization converged\n"
            "@    1     -76.01098000 -3.7D-03  0.00835  0.00348  0.02135  0.04987      0.7\n"
            "                       DFT Final Molecular Orbital Analysis\n"
            " Vector    5  Occ=2.000000D+00  E=-3.907179D-01\n"
            " Vector    6  Occ=0.000000D+00  E= 6.054023D-01\n"
            "       Multipole analysis of the density wrt the origin\n"
            "     L   x y z        total         open         nuclear\n"
            "     0   0 0 0     -0.000000      0.000000     10.000000\n\n"
            "     1   1 0 0      0.000000      0.000000      0.000000\n"
            "     1   0 1 0     -0.000000      0.000000      0.000000\n"
            "     1   0 0 1     -0.677559      0.000000     -0.113384\n"
            "  Root   1 singlet a              0.313760064 a.u.                8.5378 eV \n"
            "     Dipole Oscillator Strength                    0.0198731899\n"
            "  Root   1 triplet a              0.286149906 a.u.                7.7865 eV \n"
            "  Root   2         a              0.076143009 a.u.                2.0720 eV \n"
            "  <S2> =   0.7528\n"
            "     Dipole Oscillator Strength                    0.0010067966\n"
            " Temperature                      =   298.15K\n"
            " Zero-Point correction to Energy  =   15.291 kcal/mol  (  0.024367 au)\n"
            " Thermal correction to Energy     =   17.068 kcal/mol  (  0.027199 au)\n"
            " Thermal correction to Enthalpy   =   17.660 kcal/mol  (  0.028143 au)\n"
            " Total Entropy                    =   46.638 cal/mol-K\n"
            "   - Rotational                   =   12.045 cal/mol-K (symmetry #  =        1)\n"
            " Cv (constant volume heat capacity) =    5.965 cal/mol-K\n";
        auto r = readResult(save("summaries.out", log));
        QCOMPARE(r.steps.size(), 2);
        QCOMPARE(r.steps[1].gmax, 0.00835);
        QCOMPARE(r.properties["optimization"].toString(), QString("converged"));
        QCOMPARE(r.properties["thresholds"].toObject()["gmax"].toDouble(), 0.00045);
        QCOMPARE(r.orbitals.size(), 2);
        QCOMPARE(r.orbitals[0].occupation, 2.0);
        // A log lists only some orbitals: keep NWChem's numbers (as in the MOLDEN file).
        QCOMPARE(r.orbitals[0].number, 5);
        QCOMPARE(r.orbitals[1].number, 6);
        QVERIFY(std::abs(r.properties["dipole"].toObject()["debye"].toDouble() - 0.677559 * 2.541746473) <
                1e-9);
        QCOMPARE(r.excitations.size(), 3);
        QCOMPARE(r.excitations[0].spin, QString("singlet"));
        QCOMPARE(r.excitations[0].oscillator, 0.0198731899);
        QCOMPARE(r.excitations[1].oscillator, 0.0);
        QVERIFY(r.excitations[2].spin.isEmpty());
        QCOMPARE(r.excitations[2].s2, 0.7528);
        const auto thermo = r.properties["thermochemistry"].toArray()[0].toObject();
        QCOMPARE(thermo["temperature"].toDouble(), 298.15);
        QCOMPARE(thermo["entropy_cal"].toDouble(), 46.638);
        QCOMPARE(thermo["symmetry_number"].toDouble(), 1.0);
        // The worker transfer keeps the new sections.
        const auto path = files.filePath("summaries.bin");
        saveResultData(path, r);
        const auto loaded = loadResultData(path);
        QCOMPARE(loaded.excitations.size(), 3);
        QCOMPARE(loaded.excitations[2].s2, 0.7528);
        QCOMPARE(loaded.steps.size(), 2);
        QCOMPARE(loaded.properties, r.properties);
    }
    void excitationContributions() {
        // The amplitudes NWChem 7.3.1 prints under each excited state, in its three layouts.
        const QDir dir(FIXTURE_DIR "/nwchem");
        const auto pairText = [](const ResultData &r, int state) {
            QStringList rows;
            for (const auto &p : resultTransitions(r.excitations[state]))
                rows << QString("%1 %2 %3").arg(resultTransitionText(r, p)).arg(p.x).arg(p.weight, 0, 'f', 2);
            return rows.join(" | ");
        };
        // Restricted TDDFT: "Occ.    3  a   ---  Virt.    7  a    0.10795 X".
        auto r = readResult(dir.filePath("water_tddft.out"));
        QCOMPARE(r.properties["tddft"].toObject()["alpha_electrons"].toInt(), 5);
        QCOMPARE(r.excitations.size(), 4);
        const auto &second = r.excitations[1].contributions;
        QCOMPARE(second.size(), 2);
        QCOMPARE(second[0].from, 3);
        QCOMPARE(second[0].to, 7);
        QCOMPARE(second[0].coefficient, 0.10795);
        QVERIFY(second[0].spin.isEmpty() && !second[0].deexcitation);
        QCOMPARE(second[1].coefficient, -0.99396);
        // The heaviest pair first, against the frontier orbitals (five occupied ones).
        QCOMPARE(pairText(r, 1), QString("HOMO−1 → LUMO -0.99396 98.83 | HOMO−2 → LUMO+1 0.10795 1.17"));
        QCOMPARE(resultHomoNumber(r, {}), 5);
        // X and Y of the same pair: the weight is X² − Y².
        r = readResult(dir.filePath("peroxide_ecd.out"));
        QCOMPARE(r.excitations[0].contributions.size(), 3);
        QVERIFY(r.excitations[0].contributions[1].deexcitation);
        auto pairs = resultTransitions(r.excitations[0]);
        QCOMPARE(pairs.size(), 2);
        QCOMPARE(pairs[0].x, 0.99487);
        QCOMPARE(pairs[0].y, -0.05366);
        const double x2 = 0.99487 * 0.99487 - 0.05366 * 0.05366, other = 0.10767 * 0.10767;
        QVERIFY(std::abs(pairs[0].weight - 100 * x2 / (x2 + other)) < 1e-9);
        QVERIFY(std::isnan(pairs[1].y));
        QVERIFY(std::abs(pairs[0].weight + pairs[1].weight - 100) < 1e-9);
        // Unrestricted: "Occ.    4 beta  a   ---  Virt.    6 beta  a   -0.83332 X", each spin against
        // its own frontier orbitals (five α and four β electrons).
        r = readResult(dir.filePath("water_cation_tddft.out"));
        QCOMPARE(r.excitations.size(), 3);
        QCOMPARE(r.properties["tddft"].toObject()["beta_electrons"].toInt(), 4);
        const auto &third = r.excitations[2].contributions;
        QCOMPARE(third.size(), 4);
        QCOMPARE(third[1].spin, QString("alpha"));
        QCOMPARE(third[1].from, 5);
        QCOMPARE(third[1].coefficient, 0.54022);
        QCOMPARE(third[3].spin, QString("beta"));
        QCOMPARE(pairText(r, 0), QString("β HOMO → LUMO -0.99984 100.00"));
        QCOMPARE(pairText(r, 2), QString("β HOMO → LUMO+1 -0.83332 69.83 | α HOMO → LUMO 0.54022 29.34 | "
                                         "α HOMO−2 → LUMO+1 0.0754 0.57 | β HOMO−1 → LUMO+2 -0.05064 0.26"));
        // Tamm–Dancoff: "Occ.    7  a   ---  Virt.    9  a       -0.76260", X only.
        r = readResult(dir.filePath("formaldehyde_tda.out"));
        QCOMPARE(r.excitations.size(), 3);
        QCOMPARE(r.excitations[2].contributions.size(), 7);
        QCOMPARE(r.excitations[2].contributions[4].coefficient, -0.7626);
        QVERIFY(!r.excitations[2].contributions[4].deexcitation);
        pairs = resultTransitions(r.excitations[2]);
        QCOMPARE(resultTransitionText(r, pairs[0]), QString("HOMO−1 → LUMO"));
        QCOMPARE(resultTransitionText(r, pairs[1]), QString("HOMO → LUMO+2"));
        QVERIFY(std::abs(pairs[0].weight - 58.745) < 0.001); // 0.7626² of the sum of the seven squares.
        // The worker transfer keeps them.
        const auto path = files.filePath("contributions.bin");
        saveResultData(path, r);
        const auto loaded = loadResultData(path);
        QCOMPARE(loaded.excitations[2].contributions.size(), 7);
        QCOMPARE(loaded.excitations[2].contributions[6].to, 15);
        QCOMPARE(loaded.excitations[2].contributions[6].coefficient, 0.05814);
        // A log that ends within the amplitudes of a state (between lines, within a line, or before
        // anything follows them) keeps the state without them; with the next text, both.
        const auto text = QString::fromUtf8(readFile(dir.filePath("formaldehyde_tda.out")));
        const auto last = text.indexOf("     Occ.    8  a   ---  Virt.   11  a       -0.60613");
        const auto next = text.indexOf("              Target root =", last);
        QVERIFY(last > 0 && next > last);
        for (const auto end : {last, last + 30, last + 51, next}) {
            r = readResult(save("tda-cut.out", text.left(end)));
            QCOMPARE(r.excitations.size(), 3);
            QCOMPARE(r.excitations[1].contributions.size(), 2);
            QVERIFY2(r.excitations[2].contributions.isEmpty(), qPrintable(text.mid(last, end - last)));
        }
        r = readResult(save("tda-cut-after.out", text.left(next + 20)));
        QCOMPARE(r.excitations[2].contributions.size(), 7);
        // The fifty largest of a state, in their order; an irreducible representation of four
        // characters touches a negative amplitude. Without the electron counts, the frontier orbitals
        // are those of the orbital analysis, and without that the numbers stay.
        QString many =
            "NWChem\n  Root   1 singlet a1'            0.100000000 a.u.                2.7211 eV \n"
            "     Dipole Oscillator Strength                    0.0100000000\n\n"
            "     Occ.    5  a1''---  Virt.    6  a2''-0.90000 X\n";
        for (int k = 1; k <= 59; ++k)
            many += QString("     Occ.    4  e'  ---  Virt.%1  e'' %2 X\n")
                        .arg(k + 6, 5)
                        .arg(.05 + k * .001, 0, 'f', 5);
        many += "  ----------------------------------------------------------------------------\n";
        r = readResult(save("many.out", many));
        QCOMPARE(r.excitations[0].contributions.size(), ResultExcitation::maxContributions);
        QCOMPARE(r.excitations[0].contributions[0].coefficient, -0.9);
        QCOMPARE(r.excitations[0].contributions[1].to, 17); // 0.061: the ten smallest are left out.
        QCOMPARE(r.excitations[0].contributions.last().to, 65);
        QCOMPARE(resultTransitionText(r, resultTransitions(r.excitations[0])[0]), QString("5 → 6"));
        r = readResult(
            save("many-analysis.out", many + "                       DFT Final Molecular Orbital Analysis\n"
                                             " Vector    5  Occ=2.000000D+00  E=-3.907179D-01\n"
                                             " Vector    6  Occ=0.000000D+00  E= 6.054023D-01\n"));
        QCOMPARE(resultTransitionText(r, resultTransitions(r.excitations[0])[0]), QString("HOMO → LUMO"));
    }
    void bondIndicesAndValencies() {
        // The Mulliken analysis of NWChem 7.3.1: the whole matrix, eight columns at a time.
        const QDir dir(FIXTURE_DIR "/nwchem");
        const auto index = [](const ResultData &r, int a, int b) {
            for (const auto &value : r.properties["bond_indices"].toArray())
                if (value.toObject()["a"].toInt() == a && value.toObject()["b"].toInt() == b)
                    return value.toObject()["value"].toDouble();
            return std::numeric_limits<double>::quiet_NaN();
        };
        auto r = readResult(dir.filePath("ethanol_mulliken.out"));
        QCOMPARE(r.properties["bond_indices"].toArray().size(), 36); // Nine atoms.
        QVERIFY(!r.properties.contains("bond_indices_omitted"));
        QCOMPARE(r.properties["bond_indices"].toArray()[0].toObject(),
                 (QJsonObject{{"a", 1}, {"b", 2}, {"value", 1.00216}}));
        QCOMPARE(index(r, 2, 3), 1.00439);
        QCOMPARE(index(r, 3, 9), 0.93779); // From the second set of columns.
        QCOMPARE(index(r, 8, 9), 0.00031);
        QCOMPARE(r.properties["valency"].toArray().size(), 9);
        QCOMPARE(r.properties["valency"].toArray()[0].toDouble(), 3.95907);
        QCOMPARE(r.properties["free_electrons"].toArray()[2].toDouble(), 6.26658);
        QCOMPARE(r.properties["mulliken"].toArray().size(), 9);
        // Open shell: the valency is more than the sum of the bond indices.
        r = readResult(dir.filePath("oh_mulliken.out"));
        QCOMPARE(r.properties["bond_indices"].toArray(),
                 (QJsonArray{QJsonObject{{"a", 1}, {"b", 2}, {"value", 0.79181}}}));
        QCOMPARE(r.properties["valency"].toArray(), (QJsonArray{1.79514, 0.79287}));
        QCOMPARE(r.properties["free_electrons"].toArray(), (QJsonArray{7.61019, -0.19381}));
        QCOMPARE(index(readResult(dir.filePath("water_properties.out")), 2, 3), -0.00026);
        // A matrix or a table that the log ends in is not read; an earlier complete one stays, and a
        // later complete one replaces it.
        const auto text = QString::fromUtf8(readFile(dir.filePath("ethanol_mulliken.out")));
        const auto cut = [&](const QString &before) {
            const auto at = text.indexOf(before);
            Q_ASSERT(at > 0);
            return text.left(at);
        };
        r = readResult(save("bonds-cut.out", cut("  1- 9   0.00972")));
        QVERIFY(!r.properties.contains("bond_indices") && !r.properties.contains("valency"));
        QCOMPARE(r.properties["mulliken"].toArray().size(), 9);
        r = readResult(save("bonds-cut-row.out", cut("  9- 2   0.01327")));
        QVERIFY(!r.properties.contains("bond_indices"));
        // Eight rows of eight columns are no matrix of eight atoms without the list after them.
        r = readResult(save("bonds-cut-eight.out", cut("  9- 1   0.00972")));
        QVERIFY(!r.properties.contains("bond_indices"));
        r = readResult(save("valency-cut.out", cut("     9  H       0.96387")));
        QCOMPARE(r.properties["bond_indices"].toArray().size(), 36);
        QVERIFY(!r.properties.contains("valency") && !r.properties.contains("free_electrons"));
        const QString water = "          ----- Bond indices -----\n"
                              "  1- 1   0.00000  1- 2   %1  1- 3   0.82927\n"
                              "  2- 1   %1  2- 2   0.00000  2- 3  -0.00026\n"
                              "  3- 1   0.82927  3- 2  -0.00026  3- 3   0.00000\n\n"
                              " Large bond indices\n";
        r = readResult(save("bonds-twice.out", "NWChem\n" + water.arg("0.80000") + water.arg("0.90000") +
                                                   "          ----- Bond indices -----\n"
                                                   "  1- 1   0.00000  1- 2   0.70000  1- 3   0.82927\n"));
        QCOMPARE(r.properties["bond_indices"].toArray().size(), 3);
        QCOMPARE(index(r, 1, 2), 0.9);
        // From the 100th atom on NWChem prints "**" for the numbers: 101 atoms, where each pair has
        // the index (a + b) / 1000, are read by their places.
        const auto matrix = [](int atoms, const std::function<double(int, int)> &value) {
            QString text = "NWChem\n          ----- Bond indices -----\n";
            const auto number = [](int n) { return n < 100 ? QString("%1").arg(n, 2) : QString("**"); };
            for (int first = 1; first <= atoms; first += 8)
                for (int row = 1; row <= atoms; ++row) {
                    for (int column = first; column <= std::min(first + 7, atoms); ++column)
                        text +=
                            QString(" %1-%2%3")
                                .arg(number(row), number(column))
                                .arg(row == column ? 0 : value(std::min(row, column), std::max(row, column)),
                                     10, 'f', 5);
                    text += '\n';
                }
            return text + "\n Large bond indices\n";
        };
        r = readResult(save("bonds-101.out", matrix(101, [](int a, int b) { return (a + b) / 1000.; })));
        QCOMPARE(r.properties["bond_indices"].toArray().size(), 101 * 100 / 2);
        QCOMPARE(index(r, 1, 2), 0.003);
        QCOMPARE(index(r, 99, 100), 0.199);
        QCOMPARE(index(r, 100, 101), 0.201);
        QCOMPARE(r.properties["bond_indices"].toArray().last().toObject()["a"].toInt(), 100);
        // More pairs than are kept (201 atoms: 20100): only the bonds, here of neighbours in a chain,
        // and how many pairs were left out.
        r = readResult(
            save("bonds-201.out", matrix(201, [](int a, int b) { return b == a + 1 ? 1.5 : 0.001; })));
        QCOMPARE(r.properties["bond_indices"].toArray().size(), 200);
        QCOMPARE(r.properties["bond_indices_omitted"].toInt(), 20100 - 200);
        QCOMPARE(index(r, 200, 201), 1.5);
        // A matrix whose rows do not go on as counted is not NWChem's.
        r = readResult(save("bonds-odd.out", "NWChem\n          ----- Bond indices -----\n"
                                             "  1- 1   0.00000  1- 2   0.80000\n"
                                             "  3- 1   0.80000  3- 2   0.00000\n\n"));
        QVERIFY(!r.properties.contains("bond_indices"));
    }
    void calculationEnergyByTask() {
        // Optimization, then numerical frequencies: the displaced structures' energies come last.
        const QString log = "NWChem\n" + geometry() +
                            "                                NWChem Input Module\n"
                            "         Total DFT energy =    -76.42\n"
                            "         Total DFT energy =    -76.43\n"
                            "                                NWChem Input Module\n"
                            "                   NWChem Nuclear Hessian and Frequency Analysis\n"
                            "         Total DFT energy =    -76.43\n"
                            "         Total DFT energy =    -76.4299\n"
                            "                                NWChem Input Module\n";
        auto r = readResult(save("frequencies.out", log));
        QCOMPARE(r.energies.last().hartree, -76.4299);
        auto energy = r.properties["final_energy"].toObject();
        QCOMPARE(energy["hartree"].toDouble(), -76.43);
        QCOMPARE(energy["method"].toString(), "DFT");
        QCOMPARE(energy["source"].toString(), "frequency_reference");
        // An energy task after it (e.g. a second level) is the calculation's energy again.
        r = readResult(save("followup.out", log + "         Total CCSD(T) energy:    -76.30\n"));
        energy = r.properties["final_energy"].toObject();
        QCOMPARE(energy["hartree"].toDouble(), -76.30);
        QCOMPARE(energy["source"].toString(), "final");
    }
    void typedProperties() {
        // What several views read of the properties, as typed values: from real logs,
        const auto root = QString(FIXTURE_DIR) + "/nwchem/";
        const auto optimized = readResult(root + "water_optfreq.out");
        auto energy = resultFinalEnergy(optimized);
        QVERIFY(energy.has_value());
        QCOMPARE(energy->hartree, -74.965901191753);
        QCOMPARE(energy->method, QString("SCF"));
        QVERIFY(energy->source == ResultEnergySource::FrequencyReference);
        QCOMPARE(energy->stored, optimized.properties["final_energy"].toObject());
        QVERIFY(resultOptimizationConverged(optimized) == std::optional<bool>(true));
        const auto dipole = resultDipole(optimized);
        QVERIFY(dipole.has_value());
        QVERIFY(dipole->au == Vec(0, 0, -0.672449));
        QCOMPARE(dipole->debye, optimized.properties["dipole"].toObject()["debye"].toDouble());
        QVERIFY(std::abs(dipole->debye - 1.709195) < 1e-6);
        QCOMPARE(Document::fromXyz(dipole->geometry).atoms.size(), 3);
        const auto dimer = readResult(root + "water_dimer_bsse.out");
        QVERIFY(resultFinalEnergy(dimer)->source == ResultEnergySource::BsseCorrected);
        QCOMPARE(resultFinalEnergy(dimer)->hartree,
                 dimer.properties["bsse"].toObject()["corrected"].toDouble());
        QVERIFY(!resultOptimizationConverged(dimer).has_value()); // A single point has no optimization.
        // and by their rules: nothing without the fact, and any other state or source is the plain one.
        ResultData r;
        QVERIFY(!resultFinalEnergy(r) && !resultOptimizationConverged(r) && !resultDipole(r));
        r.properties["optimization"] = "not_converged";
        QVERIFY(resultOptimizationConverged(r) == std::optional<bool>(false));
        r.properties["optimization"] = "converged";
        QVERIFY(*resultOptimizationConverged(r));
        r.properties["final_energy"] = QJsonObject{{"hartree", -1.5}, {"method", "MP2"}, {"source", "final"}};
        energy = resultFinalEnergy(r);
        QVERIFY(energy->source == ResultEnergySource::Final);
        QCOMPARE(energy->hartree, -1.5);
        QCOMPARE(energy->method, QString("MP2"));
        QCOMPARE(resultBriefSummary(r), QString("最適化: 収束 · E = −1.5 Eh（MP2）"));
        r.properties["final_energy"] = QJsonObject{{"hartree", -2.}, {"method", "DFT"}};
        QVERIFY(resultFinalEnergy(r)->source == ResultEnergySource::Final);
        r.properties["final_energy"] = QJsonObject();
        QVERIFY(!resultFinalEnergy(r));
        r.properties["dipole"] = QJsonObject{{"x", 1.}, {"y", -2.}, {"z", .5}, {"debye", 5.8}};
        QVERIFY(resultDipole(r)->au == Vec(1, -2, .5));
        QCOMPARE(resultDipole(r)->debye, 5.8);
        QVERIFY(resultDipole(r)->geometry.isEmpty());
    }
    void transferAndBounds() {
        auto r = readResult(save("transfer.cube", cubeText()), .25);
        auto path = files.filePath("result.bin");
        saveResultData(path, r);
        auto loaded = loadResultData(path);
        QCOMPARE(loaded.surface.size(), r.surface.size());
        QCOMPARE(loaded.frames[0].molecule.toJson(), r.frames[0].molecule.toJson());
        QVERIFY((loaded.surface[0].a - r.surface[0].a).norm() < 1e-15);
        writeFile(path, readFile(path).chopped(4));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, loadResultData(path));
        QFile huge(files.filePath("huge.xyz"));
        QVERIFY(huge.open(QIODevice::WriteOnly));
        QVERIFY(huge.resize(128 * 1024 * 1024 + 1));
        huge.close();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(huge.fileName()));
    }
    void outcomeSurvivesDisplayAndTransfer() {
        const auto source = save("outcome.out", "NWChem\nTotal times  cpu: 1\n");
        auto result = readResult(source);
        QCOMPARE(result.outcome, ResultOutcome::Normal);
        result.status = "Changed display text";
        QCOMPARE(resultOutcome(result), QString("normal"));
        const auto binary = files.filePath("outcome.bin");
        saveResultData(binary, result);
        const auto copy = loadResultData(binary);
        QCOMPARE(copy.outcome, ResultOutcome::Normal);
        QCOMPARE(copy.status, result.status);
    }
    void normalRaman() {
        const auto root = QString(FIXTURE_DIR) + "/nwchem/";
        const auto r = readResult(root + "water_raman.out");
        const auto file = readResult(root + "water_raman.normal");
        auto afterProperties = r;
        auto finalEnergy = r.properties["final_energy"].toObject();
        finalEnergy["source"] = "final";
        afterProperties.properties["final_energy"] = finalEnergy;
        const auto record = energyRecord(afterProperties, r.frames.first().molecule, nwchemDefaults());
        QCOMPARE(record["geometry"].toString(), r.vibrationGeometry.xyz());
        QCOMPARE(r.properties["raman"].toArray().size(), 3);
        QCOMPARE(file.properties["raman"].toArray().size(), 3);
        for (int i = 0; i < 3; ++i) {
            const auto peak = r.properties["raman"][i].toObject();
            QVERIFY(peak.contains("mode"));
            const int mode = peak["mode"].toInt();
            QVERIFY(std::abs(r.modes[mode].frequency - peak["frequency"].toDouble()) < .02);
            QVERIFY(!r.modes[mode].displacement.isEmpty());
            QVERIFY(std::abs(file.properties["raman"][i].toObject()["activity"].toDouble() -
                             peak["activity"].toDouble()) < .001);
        }
        const auto s = calculateSpectrum(r, {{"kind", "raman"}, {"width", 20}});
        QCOMPARE(s.sticks.size(), 3);
        QCOMPARE(s.yUnit, "Å⁴/amu");
        QVERIFY(nwchemResultFiles(root).contains("water_raman.normal"));
        QVERIFY_THROWS_EXCEPTION(
            std::runtime_error,
            readResult(save("bad.normal", "normal Raman Scattering Plot\nS [Ang**4/amu]\n1 100 ***\n")));
    }
    void thermochemistryOfLogsRecomputed() {
        // The wavenumbers of an energy record are those NWChem's thermochemistry summed (vib_wrtfreq.F:
        // its scale factor times the projected frequencies above 0.1 cm⁻¹). Recomputed with NWChem's
        // own constants (h = 6.626e-27 erg s, c = 2.998e10 cm/s, k = 1.3807e-16 erg/K, R = 1.9863
        // cal/(mol K)), the zero-point energy, the thermal energy and the vibrational entropy are the
        // printed ones: these are rounded to 0.001 and the frequencies to 0.01 cm⁻¹.
        const double kelvin = 6.626e-27 * 2.998e10 / 1.3807e-16, r = 1.9863e-3;
        int temperatures = 0;
        for (const auto *name : {"water_optfreq", "peroxide_thermo", "ammonia_planar_scaled",
                                 "conformers/nitrous_acid_cis", "conformers/nitrous_acid_trans"}) {
            const auto record = logRecord(name);
            const auto wavenumbers = recordVibrations(record);
            QVERIFY2(!wavenumbers.isEmpty(), name);
            for (const auto &value : record["thermochemistry"].toArray()) {
                const auto t = value.toObject();
                const double temperature = t["temperature"].toDouble();
                double zpe = 0, thermal = 0, entropy = 0;
                for (const double wavenumber : wavenumbers) {
                    const double x = kelvin * wavenumber / temperature;
                    zpe += r * kelvin * wavenumber / 2;
                    thermal += r * kelvin * wavenumber / (std::exp(x) - 1);
                    entropy += 1000 * r * (x / (std::exp(x) - 1) - std::log(1 - std::exp(-x)));
                }
                QVERIFY2(std::abs(zpe - t["zpe_kcal"].toDouble()) < 6e-4, name);
                // "Thermal correction to Energy" adds 3RT of translation and rotation (not linear).
                QVERIFY2(std::abs(zpe + thermal + 3 * r * temperature - t["energy_kcal"].toDouble()) < 6e-4,
                         name);
                QVERIFY2(std::abs(entropy - t["entropy_vibrational_cal"].toDouble()) < 6e-4, name);
                QVERIFY2(std::abs(t["entropy_translational_cal"].toDouble() +
                                  t["entropy_rotational_cal"].toDouble() +
                                  t["entropy_vibrational_cal"].toDouble() - t["entropy_cal"].toDouble()) <
                             2.1e-3,
                         name);
                // CODATA 2018 constants give 0.048 % more: NWChem's gas constant is 1.9863 cal/(mol K).
                const auto codata = vibrationalTerms(wavenumbers, temperature);
                QVERIFY2(std::abs(codata.zpe / zpe - 1.000478) < 2e-6, name);
                QVERIFY2(std::abs(codata.zpe + codata.thermal - (zpe + thermal)) < 6e-4 * (zpe + thermal),
                         name);
                QVERIFY2(std::abs(codata.entropy - entropy) < 6e-4 * entropy + 1e-6, name);
                ++temperatures;
            }
        }
        QCOMPARE(temperatures, 6);
        // Two temperatures and a torsion of low frequency.
        const auto peroxide = logRecord("peroxide_thermo");
        QCOMPARE(peroxide["thermochemistry"].toArray().size(), 2);
        QCOMPARE(peroxide["thermochemistry"][1].toObject()["temperature"].toDouble(), 350.);
        QCOMPARE(peroxide["thermochemistry"][1].toObject()["entropy_translational_cal"].toDouble(), 37.283);
        QCOMPARE(peroxide["thermochemistry"][1].toObject()["entropy_rotational_cal"].toDouble(), 18.4);
        QCOMPARE(peroxide["thermochemistry"][1].toObject()["entropy_vibrational_cal"].toDouble(), 2.645);
        QCOMPARE(recordVibrations(peroxide),
                 (QVector<double>{184.64, 1486.95, 1589.64, 1781.05, 4140.89, 4148.28}));
        // Planar NH3 with "set vib:scalefreq 0.9": the imaginary mode (-1081.38 cm⁻¹) is left out as
        // NWChem does, and the wavenumbers are the scaled ones its thermochemistry used.
        const auto ammonia = logRecord("ammonia_planar_scaled");
        QCOMPARE(ammonia["thermochemistry"][0].toObject()["nwchem_frequency_scale"].toDouble(), .9);
        QCOMPARE(ammonia["imaginary"].toInt(), 1);
        const auto scaled = recordVibrations(ammonia);
        QCOMPARE(scaled.size(), 5);
        QVERIFY(std::abs(scaled[0] - .9 * 1866.45) < 1e-9);
        QVERIFY(std::abs(scaled[4] - .9 * 4363.45) < 1e-9);
        // A result without thermochemistry keeps no list.
        QVERIFY(!logRecord("water_tddft").contains("vibrations"));
    }
    void thermalCorrectionModels() {
        // One oscillator with x = hcν/kT = 1 at 300 K: ν = 300 K/(hc/k) = 300/1.4387768775 =
        // 208.510440146 cm⁻¹. Then ZPE = RT/2, the thermal energy is RT/(e − 1) and
        // S = R[1/(e − 1) − ln(1 − 1/e)] = 1.0406517749 R, with R = k·N_A = 1.98720425864 cal/(mol K).
        const double one = 208.51044014583823, r = 1.9872042586408316e-3;
        const auto same = [](const VibrationalTerms &t, double zpe, double thermal, double entropy) {
            return std::abs(t.zpe - zpe) < 1e-12 && std::abs(t.thermal - thermal) < 1e-12 &&
                   std::abs(t.entropy - entropy) < 1e-10;
        };
        QVERIFY(std::abs(r * 300 / 2 - .29808063879612473) < 1e-15);
        QVERIFY(
            same(vibrationalTerms({one}, 300.), .29808063879612473, .3469519770961477, 2.067987792566404));
        // A scale factor of 0.5 on twice the wavenumber is the same oscillator; two modes add up.
        QVERIFY(same(vibrationalTerms({2 * one}, 300., {.scale = .5}), .29808063879612473, .3469519770961477,
                     2.067987792566404));
        QVERIFY(same(vibrationalTerms({one, 2 * one}, 300., {.scale = .5}), .29808063879612473 * 1.5,
                     .3469519770961477 + .459489540823015, 2.067987792566404 + 3.3852008068756057));
        // Truhlar: a mode below the cutoff counts as one at the cutoff, in the entropy alone. At
        // x = 1/2 the thermal energy is RT·(1/2)/(√e − 1) and the harmonic entropy 1.7034991 R.
        QVERIFY(same(vibrationalTerms({one / 2}, 300.), .29808063879612473 / 2, .459489540823015,
                     3.3852008068756057));
        QVERIFY(same(vibrationalTerms({one / 2}, 300., {.model = "truhlar", .cutoff = one}),
                     .29808063879612473 / 2, .459489540823015, 2.067987792566404));
        QVERIFY(same(vibrationalTerms({one}, 300., {.model = "truhlar", .cutoff = 100}), .29808063879612473,
                     .3469519770961477, 2.067987792566404));
        // Grimme at 298.15 K with the cutoff of 100 cm⁻¹. A mode of 100 cm⁻¹ has w = 1/2: its free
        // rotor has μ = h/(8π²cν) = 2.79928e-48 kg m², μ′ = μB/(μ + B) = 2.79850e-48 with B = 1e-44
        // and S = R[1/2 + ln √(8π³μ′kT/h²)] = 1.43655 R = 2.8547015505 cal/(mol K); the oscillator
        // (x = 0.482568) has 3.4543174674: the mean is 3.1545095090. A mode of 50 cm⁻¹ has
        // w = 1/17: (4.8173856768 + 16 × 3.5431360452)/17 = 3.6180919059.
        const ThermalOptions grimme{.model = "grimme"};
        QVERIFY(std::abs(vibrationalTerms({100.}, 298.15).entropy - 3.4543174673879418) < 1e-10);
        QVERIFY(std::abs(vibrationalTerms({100.}, 298.15, grimme).entropy - 3.1545095089507953) < 1e-10);
        QVERIFY(std::abs(vibrationalTerms({50.}, 298.15, grimme).entropy - 3.6180919058919234) < 1e-10);
        // Energies are those of the oscillator, and a stretch far above the cutoff keeps its entropy.
        QCOMPARE(vibrationalTerms({50.}, 298.15, grimme).thermal, vibrationalTerms({50.}, 298.15).thermal);
        QVERIFY(std::abs(vibrationalTerms({3000.}, 298.15, grimme).entropy -
                         vibrationalTerms({3000.}, 298.15).entropy) < 1e-6);
        // 1 atm → 1 mol/L: an ideal gas at 1 atm and 298.15 K takes R′T = 0.0820573661 × 298.15 =
        // 24.4654 L/mol, and RT·ln 24.4654 = 0.592485 × 3.197262 = 1.8943 kcal/mol.
        QVERIFY(std::abs(standardStateShift(298.15) - 1.8943284454941451) < 1e-12);
        QVERIFY(std::abs(standardStateShift(298.15) - 1.894) < 5e-4);
        // Eyring: k_B·T/h = 6.212438e12 s⁻¹ at 298.15 K and exp(−20/0.592485) = 2.187249e-15 for a
        // barrier of 20 kcal/mol: k = 1.358815e-2 s⁻¹.
        QVERIFY(std::abs(boltzmannFactor(20, 298.15) / 2.1872492033246132e-15 - 1) < 1e-12);
        QVERIFY(std::abs(eyringRate(20, 298.15) / 1.358815004787466e-2 - 1) < 1e-12);
        // Options out of range are refused; missing ones are the defaults.
        QVERIFY(!thermalOptions({}).vibrational() && !thermalOptions({}).molar);
        QVERIFY(thermalOptions({{"standard_state_correction", "1M"}}).molar);
        for (const auto &bad : {QJsonObject{{"frequency_scale", 1.6}}, QJsonObject{{"frequency_scale", "1"}},
                                QJsonObject{{"entropy_model", "free"}}, QJsonObject{{"entropy_cutoff", 5}},
                                QJsonObject{{"standard_state_correction", "1 bar"}}})
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, thermalOptions(bad));
    }
    void energyProfileCorrections() {
        // H2O2 at HF/STO-3G (tests/fixtures/nwchem/peroxide_thermo.out) at 298.15 K: NWChem printed
        // ZPE 19.049, H 21.791 kcal/mol and S 56.713 cal/(mol K).
        const auto peroxide = logRecord("peroxide_thermo");
        const double e = peroxide["energy_hartree"].toDouble(), zpe = 19.049, h = 21.791, s = 56.713;
        auto p = profileOf({peroxide}, "G", 298.15);
        const auto stage = [](const QJsonObject &profile) {
            return evaluateEnergyProfile(profile)["stages"][0].toObject();
        };
        const auto hartree = [&](const QJsonObject &profile) { return stage(profile)["hartree"].toDouble(); };
        // Without the corrections, left out or at their defaults, the values are the printed ones to
        // the last bit, also for a record saved without its wavenumbers.
        auto defaults = p;
        defaults["frequency_scale"] = 1.;
        defaults["entropy_model"] = "rrho";
        defaults["entropy_cutoff"] = 100.;
        defaults["standard_state_correction"] = "none";
        auto older = peroxide;
        older.remove("vibrations");
        const auto old = profileOf({older}, "G", 298.15);
        for (const auto &pair : {qMakePair(QString("ZPE"), e + zpe / HartreeToKcal),
                                 qMakePair(QString("H"), e + h / HartreeToKcal),
                                 qMakePair(QString("G"), e + (h - 298.15 * s / 1000) / HartreeToKcal),
                                 qMakePair(QString("E"), e)})
            for (auto profile : {p, defaults, old}) {
                profile["metric"] = pair.first;
                QVERIFY2(hartree(profile) == pair.second, qPrintable(pair.first));
            }
        QCOMPARE(evaluateEnergyProfile(p)["entropy_model"].toString(), "rrho");
        QCOMPARE(evaluateEnergyProfile(p)["frequency_scale"].toDouble(), 1.);
        // Corrections that need the wavenumbers say so; the standard state does not need them.
        auto refused = old;
        refused["frequency_scale"] = .97;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, evaluateEnergyProfile(refused));
        refused = old;
        refused["entropy_model"] = "grimme";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, evaluateEnergyProfile(refused));
        refused["metric"] = "H"; // The entropy model changes G only.
        QVERIFY(hartree(refused) == e + h / HartreeToKcal);
        refused = old;
        refused["standard_state_correction"] = "1M";
        QVERIFY(std::abs((hartree(refused) - hartree(old)) * HartreeToKcal - 1.8943284454941451) < 1e-9);
        // A scale factor of 0.9 with CODATA 2018 constants on the six wavenumbers (sum 13331.45 cm⁻¹):
        // ΔZPE = −0.1 × ½ × 2.8591435382e-3 kcal cm/mol × 13331.45 = −1.9058264561, ΔH = −1.8808983933
        // (ZPE and the thermal energy Σ hcν/(e^x − 1)) and ΔS = +0.2199610546 cal/(mol K). They are
        // added to the printed values, so 0.9 × 19.049 = 17.144 is met within NWChem's constants.
        auto scaled = p;
        scaled["frequency_scale"] = .9;
        scaled["metric"] = "ZPE";
        QVERIFY(std::abs((hartree(scaled) - e) * HartreeToKcal - (zpe - 1.9058264561233351)) < 1e-9);
        QVERIFY(std::abs((hartree(scaled) - e) * HartreeToKcal - .9 * zpe) < 2e-3);
        scaled["metric"] = "H";
        QVERIFY(std::abs((hartree(scaled) - e) * HartreeToKcal - (h - 1.8808983932684837)) < 1e-9);
        scaled["metric"] = "G";
        auto source = stage(scaled)["sources"][0].toObject();
        QVERIFY(std::abs(source["enthalpy_kcal"].toDouble() - (h - 1.8808983932684837)) < 1e-9);
        QVERIFY(std::abs(source["entropy_cal"].toDouble() - (s + .2199610545580574)) < 1e-9);
        QVERIFY(std::abs(source["correction_kcal"].toDouble() -
                         (h - 1.8808983932684837 - 298.15 * (s + .2199610545580574) / 1000)) < 1e-9);
        // Quasi-RRHO changes the entropy alone, mostly of the torsion at 184.64 cm⁻¹: Grimme with the
        // cutoff of 100 cm⁻¹ gives ΔS = −0.0028052094 over the six modes, Truhlar with 300 cm⁻¹
        // S_HO(300) − S_HO(184.64) = −0.8640741536 cal/(mol K).
        auto model = p;
        model["entropy_model"] = "grimme";
        source = stage(model)["sources"][0].toObject();
        QCOMPARE(source["enthalpy_kcal"].toDouble(), h);
        QVERIFY(std::abs(source["entropy_cal"].toDouble() - (s - .002805209394948247)) < 1e-9);
        model["entropy_model"] = "truhlar";
        QCOMPARE(stage(model)["sources"][0].toObject()["entropy_cal"].toDouble(), s); // All above 100.
        model["entropy_cutoff"] = 300.;
        QVERIFY(std::abs(stage(model)["sources"][0].toObject()["entropy_cal"].toDouble() -
                         (s - .8640741536148497)) < 1e-9);
        QVERIFY(std::abs((hartree(model) - hartree(p)) * HartreeToKcal - 298.15 * .8640741536148497 / 1000) <
                1e-9);
        // 1 mol/L adds RT·ln(R′T) = 1.8943 kcal/mol to G of each mole, and nothing to H or ZPE; at
        // 350 K it is 0.695522 × ln 28.7201 = 2.3353.
        auto molar = p;
        molar["standard_state_correction"] = "1M";
        auto stages = molar["stages"].toArray();
        stages[0] = QJsonObject{
            {"name", "2 H2O2"},
            {"terms", QJsonArray{QJsonObject{{"record", "peroxide_thermo"}, {"coefficient", 2}}}}};
        molar["stages"] = stages;
        auto gas = molar;
        gas.remove("standard_state_correction");
        QVERIFY(std::abs((hartree(molar) - hartree(gas)) * HartreeToKcal - 2 * 1.8943284454941451) < 1e-9);
        QVERIFY(std::abs(evaluateEnergyProfile(molar)["standard_state_shift_kcal"].toDouble() - 1.894) <
                5e-4);
        QCOMPARE(evaluateEnergyProfile(molar)["standard_state_correction"].toString(), "1M");
        molar["temperature"] = 350.;
        gas["temperature"] = 350.;
        QVERIFY(std::abs((hartree(molar) - hartree(gas)) * HartreeToKcal - 2 * 2.3353) < 2e-4);
        for (const auto *metric : {"H", "ZPE", "E"}) {
            molar["metric"] = metric;
            gas["metric"] = metric;
            QVERIFY(hartree(molar) == hartree(gas));
            QCOMPARE(evaluateEnergyProfile(molar)["standard_state_shift_kcal"].toDouble(), 0.);
        }
    }
    void conformerThermalCorrections() {
        // cis- and trans-HONO at B3LYP/STO-3G (tests/fixtures/nwchem/conformers/): trans is
        // 0.001305350416 Eh = 0.81912 kcal/mol above cis, and with the printed corrections
        // (H 15.287/15.345 kcal/mol, S 59.456/59.654 cal/(mol K)) 0.81809 kcal/mol in G at 298.15 K.
        const QJsonArray records{logRecord("conformers/nitrous_acid_cis"),
                                 logRecord("conformers/nitrous_acid_trans")};
        const double rt = 1.9872042586408316e-3 * 298.15;
        const auto cis = [](const QJsonArray &rows) { return rows[0].toObject()["population"].toDouble(); };
        auto rows = conformerPopulations(records, "E", 298.15, .1);
        QVERIFY(std::abs(rows[1].toObject()["relative_kcal"].toDouble() - .81912) < 1e-5);
        QVERIFY(std::abs(cis(rows) - 1 / (1 + std::exp(-.001305350416 * HartreeToKcal / rt))) < 1e-9);
        rows = conformerPopulations(records, "G", 298.15, .1);
        const double printed =
            .001305350416 * HartreeToKcal + (15.345 - 298.15 * .059654) - (15.287 - 298.15 * .059456);
        QVERIFY(std::abs(printed - .81809) < 1e-5);
        QVERIFY(std::abs(rows[1].toObject()["relative_kcal"].toDouble() - printed) < 1e-9);
        QVERIFY(std::abs(cis(rows) - 1 / (1 + std::exp(-printed / rt))) < 1e-9);
        // Default options are the same; every HONO vibration is above 500 cm⁻¹, where Truhlar's
        // raising ends.
        const QJsonObject defaults{
            {"frequency_scale", 1.}, {"entropy_model", "rrho"}, {"entropy_cutoff", 100.}};
        QCOMPARE(conformerPopulations(records, "G", 298.15, .1, defaults), rows);
        QCOMPARE(conformerPopulations(
                     records, "G", 298.15, .1,
                     {{"frequency_scale", 1.}, {"entropy_model", "truhlar"}, {"entropy_cutoff", 500.}}),
                 rows);
        // A scale factor and Grimme's entropy move G by the differences of the vibrational terms of
        // the two conformers.
        const QJsonObject options{
            {"frequency_scale", .95}, {"entropy_model", "grimme"}, {"entropy_cutoff", 500.}};
        const auto corrected = conformerPopulations(records, "G", 298.15, .1, options);
        const ThermalOptions o{.scale = .95, .model = "grimme", .cutoff = 500};
        double shift = 0;
        for (int i = 0; i < 2; ++i) {
            const auto wavenumbers = recordVibrations(records[i].toObject());
            const auto harmonic = vibrationalTerms(wavenumbers, 298.15),
                       model = vibrationalTerms(wavenumbers, 298.15, o);
            shift += (i ? 1 : -1) * (model.zpe + model.thermal - harmonic.zpe - harmonic.thermal -
                                     298.15 * (model.entropy - harmonic.entropy) / 1000);
        }
        QVERIFY(std::abs(shift) > 1e-3);
        QVERIFY(std::abs(corrected[1].toObject()["relative_kcal"].toDouble() - (printed + shift)) < 1e-9);
        QVERIFY(std::abs(cis(corrected) - 1 / (1 + std::exp(-(printed + shift) / rt))) < 1e-9);
        // With E the options have nothing to change.
        QCOMPARE(conformerPopulations(records, "E", 298.15, .1, options),
                 conformerPopulations(records, "E", 298.15, .1));
    }
    void ramanPreservesReferenceEnergy() {
        NwchemEnergySummary summary;
        for (const auto &line : QStringList{
                 "NWChem Input Module", "Total DFT energy = -76.0",
                 "NWChem Nuclear Hessian and Frequency Analysis", "Total DFT energy = -75.9",
                 "NWChem Input Module", "Raman Analysis", "Total DFT energy = -75.8", "Total times cpu: 2s"})
            summary.add(line);
        QCOMPARE(summary.energy()["hartree"].toDouble(), -76.);
    }
    void rigidModes() {
        ResultData r;
        r.vibrationGeometry = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        for (double f : {-0.02, 1600.0, 0.01, -1.5, 0.0, 3700.0, 2.0, 0.5, 3800.0})
            r.modes.append({f, 1, {}});
        const auto rigid = resultRigidModes(r);
        QCOMPARE(rigid, (QVector<bool>{true, false, true, true, true, false, true, true, false}));
        // Linear molecules have five rigid motions; incomplete mode sets are left alone.
        r.vibrationGeometry = Document::fromXyz("3\nCO2\nO -1.16 0 0\nC 0 0 0\nO 1.16 0 0\n");
        r.modes[6].frequency = 667;
        QCOMPARE(resultRigidModes(r).count(true), 5);
        r.modes.removeLast();
        QCOMPARE(resultRigidModes(r).count(true), 0);
    }
    void summaryRules() {
        // Every negative vibration is imaginary; below −10 cm⁻¹ it is significant.
        ResultData r;
        r.source = "water.out";
        r.status = "status";
        r.vibrationGeometry = Document::fromXyz("3\nwater\nO 0 0 0\nH 0.758 0 0.504\nH -0.758 0 0.504\n");
        for (double f : {-0.02, -250.0, 0.01, -1.5, 0.0, -5.0, 2.0, 0.5, 3800.0})
            r.modes.append({f, 1, {}});
        using K = ResultModeKind;
        const auto kinds = resultModeKinds(r);
        QCOMPARE(kinds, (QVector<K>{K::Rigid, K::SignificantImaginary, K::Rigid, K::Rigid, K::Rigid,
                                    K::Imaginary, K::Rigid, K::Rigid, K::Vibration}));
        const auto counts = resultModeCounts(kinds);
        QCOMPARE(counts.vibrations, 3);
        QCOMPARE(counts.rigid, 6);
        QCOMPARE(counts.imaginary, 2);
        QCOMPARE(counts.significant, 1);
        QCOMPARE(resultImaginaryText(counts), QString("虚振動 2個（−10 cm⁻¹未満 1個）"));
        QCOMPARE(resultImaginaryText({}), QString("虚振動なし"));
        r.properties["optimization"] = "converged";
        QCOMPARE(resultBriefSummary(r), QString("最適化: 収束 · 虚振動 2個（−10 cm⁻¹未満 1個）"));
        QCOMPARE(resultMessage(r), "status\n" + resultBriefSummary(r));
        QVERIFY(resultMessage(ResultData()).isEmpty());
        // The criteria in NWChem's order; the displacements are not judged at step 0.
        const QJsonObject thresholds{{"gmax", 4.5e-4}, {"grms", 3e-4}, {"xmax", 1.8e-3}, {"xrms", 1.2e-3}};
        ResultStep step{0, -76, 0, 4e-4, 2e-4, 0, 0, 0};
        auto criteria = resultCriteria(step, thresholds);
        QCOMPARE(QString(criteria[2].name), QString("Xrms"));
        QCOMPARE(QString(criteria[3].name), QString("Xmax"));
        QVERIFY(criteria[0].met() && criteria[1].met());
        QVERIFY(!criteria[2].judged() && !criteria[3].judged() && !criteria[3].met());
        step.step = 1;
        QVERIFY(resultCriteria(step, thresholds)[3].met());
        QVERIFY(!resultCriteria(step, {{"gmax", 4.5e-4}})[1].judged()); // No threshold printed for it.
    }
    void animationLoadingRejectsChangedSources() {
        QTemporaryDir job;
        const auto first = job.filePath("mode.m-001.s-001.xyz");
        const auto second = job.filePath("mode.m-001.s-002.xyz");
        const auto third = job.filePath("mode.m-001.s-003.xyz");
        writeFile(first, "1\nfirst\nH 0 0 1\n");
        writeFile(second, "1\nsecond\nH 0 0 2\n");
        ResultLoader loader;
        QSignalSpy loaded(&loader, &ResultLoader::loaded), failed(&loader, &ResultLoader::failed);
        ResultLoadRequest request{first};
        request.animation = true;
        loader.start(request);
        // No event processing between start and these changes: even an already finished worker's
        // result must be rejected. Check a non-primary source, additions and removals separately.
        writeFile(second, "1\nchanged second step\nH 0 0 20\n");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        loader.start(request);
        writeFile(third, "1\nthird\nH 0 0 3\n");
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 10000);
        loader.start(request);
        QVERIFY(QFile::remove(second));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 3, 10000);
        QCOMPARE(loaded.count(), 0);
        loader.start(request);
        QVERIFY(loader.cancel());
        QVERIFY(!loader.active());
        loader.start(request);
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        const auto data = qvariant_cast<ResultData>(loaded[0][0]);
        QCOMPARE(data.source, first);
        QCOMPARE(data.frames.size(), 2);
        QCOMPARE(data.frames[1].molecule.atoms[0].position.z(), 3.0);
    }
    void analyticOrbitalsAndNormalization() {
        const Vec point = Vec(.3, .5, .7);
        const double exponential = std::exp(-point.squaredNorm());
        for (int l = 0; l <= 4; ++l) {
            QMap<QString, QStringList> blocks{{"GTO", {"1 0", QString("%1 1 0").arg("spdfg"[l]), "1 1"}}};
            double df = 1;
            for (int n = 2 * l - 1; n > 1; n -= 2)
                df *= n;
            const double radial = std::pow(2 / std::numbers::pi, .75) * std::sqrt(std::pow(4., l) / df);
            for (bool spherical : {false, true}) {
                if (spherical) {
                    blocks["5D"] = {};
                    blocks["9G"] = {};
                }
                const int n = spherical && l >= 2 ? 2 * l + 1 : (l + 1) * (l + 2) / 2;
                QVector<QVector<double>> coefficients;
                for (int i = 0; i < n; ++i) {
                    QVector<double> c(n);
                    c[i] = 1;
                    coefficients << c;
                }
                const auto w = moldenWavefunction(blocks, hydrogen(), coefficients);
                QCOMPARE(w.normalization, "standard");
                double sum = 0;
                for (int i = 0; i < n; ++i) {
                    const double v = w.value(i, point * BohrToAngstrom);
                    sum += v * v;
                }
                if (spherical && l >= 2)
                    QVERIFY(std::abs(sum - radial * radial * exponential * exponential *
                                               std::pow(point.squaredNorm(), l)) < 1e-12);
                double polynomial = std::pow(point.x(), l);
                if (spherical && l == 2)
                    polynomial = (3 * point.z() * point.z() - point.squaredNorm()) / 2;
                if (spherical && l == 3)
                    polynomial = (5 * std::pow(point.z(), 3) - 3 * point.z() * point.squaredNorm()) / 2;
                if (spherical && l == 4)
                    polynomial =
                        (35 * std::pow(point.z(), 4) - 30 * point.z() * point.z() * point.squaredNorm() +
                         3 * std::pow(point.squaredNorm(), 2)) /
                        8;
                QVERIFY(std::abs(w.value(0, point * BohrToAngstrom) - radial * exponential * polynomial) <
                        1e-12);
                auto embedded = blocks;
                embedded["GTO"][2] = "1 " + QString::number(radial, 'g', 17);
                const auto nw = moldenWavefunction(embedded, hydrogen(), coefficients);
                QCOMPARE(nw.normalization, "nwchem");
                for (int i = 0; i < n; ++i)
                    QVERIFY(std::abs(nw.value(i, point * BohrToAngstrom) -
                                     w.value(i, point * BohrToAngstrom)) < 1e-12);
            }
        }
        // Cartesian xy has a different primitive normalization from xx.
        QMap<QString, QStringList> blocks{{"GTO", {"1", "d 1", "1 1"}}};
        auto w = moldenWavefunction(blocks, hydrogen(), {{0, 0, 0, 1, 0, 0}});
        const double norm = std::pow(2 / std::numbers::pi, .75) * 4;
        QVERIFY(std::abs(w.value(0, point * BohrToAngstrom) - norm * point.x() * point.y() * exponential) <
                1e-12);
    }
    void contractedBasisAndBounds() {
        QMap<QString, QStringList> b{{"GTO", {"1 0", "s 2 1", "2 .5", ".5 .6"}}};
        // Valid standard coefficients need not describe a normalized contraction. Do not
        // silently guess when the file cannot distinguish standard from embedded factors.
        const auto standard = moldenWavefunction(b, hydrogen(), {{1}}, "standard");
        const QMap<QString, QStringList> unnormalized{{"GTO", {"1 0", "s 2 1", "2 1", ".5 1.2"}}};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenWavefunction(unnormalized, hydrogen(), {{1}}));
        const auto explicitStandard = moldenWavefunction(unnormalized, hydrogen(), {{1}}, "standard");
        QVERIFY(std::abs(explicitStandard.value(0, Vec(.2, .3, .4)) - standard.value(0, Vec(.2, .3, .4))) <
                1e-12);
        auto embedded = b;
        for (int i = 0; i < 2; ++i) {
            const auto p = standard.shells[0].primitives[i];
            embedded["GTO"][i + 2] =
                QString::number(p.exponent, 'g', 17) + ' ' + QString::number(p.coefficient, 'g', 17);
        }
        const auto nw = moldenWavefunction(embedded, hydrogen(), {{1}});
        QCOMPARE(nw.normalization, "nwchem");
        for (double x : {0., .2, 1., 3.})
            QVERIFY(std::abs(nw.value(0, Vec(x, 0, 0)) - standard.value(0, Vec(x, 0, 0))) < 1e-12);
        for (const auto &bad : {QStringList{"1 0", "h 1 0", "1 1"}, QStringList{"1 0", "s 2 0", "1 1"},
                                QStringList{"1 0", "s 1 0", "-1 1"}, QStringList{"1 0", "s 1 0", "nan 1"},
                                QStringList{"2 0", "s 1 0", "1 1"}, QStringList{"1 0", "s 1 0", "1 0"}}) {
            b["GTO"] = bad;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenWavefunction(b, hydrogen(), {{1}}));
        }
        b["GTO"] = {"1 0", "p 1 0", "1 1"};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenWavefunction(b, hydrogen(), {{0, 0, 0, 1}}));
        b["GTO"] = {"1 0", "d 1 0", "1 1"};
        b["5D7F9G"] = {};
        QCOMPARE(moldenWavefunction(b, hydrogen(), {{1, 0, 0, 0, 0}}).shells[0].functions.size(), 5);
        b["6D"] = {};
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenWavefunction(b, hydrogen(), {{1}}));
        b.remove("5D7F9G");
        QVector<QVector<double>> sparse(20000, QVector<double>{1});
        b["GTO"] = {"1 0"};
        for (int i = 0; i < 100; ++i)
            b["GTO"] << "g 1 0" << "1 1";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenWavefunction(b, hydrogen(), sparse));
    }
    void separableGridMatchesPointEvaluation() {
        auto atoms = Document::fromXyz("2\noff-center\nH .23 -.41 .37\nH -.71 .53 -.29\n");
        for (bool spherical : {false, true}) {
            QMap<QString, QStringList> blocks{{"GTO",
                                               {"1 0", "sp 2 0", ".7 .3 -.4", "2.1 .8 .9", "2 0", "d 2 0",
                                                ".9 .6", "3.2 -.2", "f 1 0", "1.3 1", "g 1 0", "1.7 1"}}};
            if (spherical)
                blocks["5D7F9G"] = {};
            const int basis = spherical ? 25 : 35;
            QVector<double> coefficients(basis);
            for (int i = 0; i < basis; ++i)
                coefficients[i] = std::sin(i + .4);
            const auto w =
                moldenWavefunction(blocks, atoms, {coefficients, QVector<double>(basis)}, "standard");
            const Vec origin(-1.21, -.83, -.47), step(.31, .27, .19);
            const std::array<int, 3> grid{7, 6, 5};
            const auto values = moldenOrbitalGrid(w, 0, origin, step, grid);
            QCOMPARE(values.size(), 7 * 6 * 5);
            int index = 0;
            for (int x = 0; x < grid[0]; ++x)
                for (int y = 0; y < grid[1]; ++y)
                    for (int z = 0; z < grid[2]; ++z) {
                        const auto expected = w.value(0, origin + step.cwiseProduct(Vec(x, y, z)));
                        QVERIFY(std::abs(values[index++] - expected) < 1e-12);
                    }
            for (double v : moldenOrbitalGrid(w, 1, origin, step, grid))
                QCOMPARE(v, 0.);
            const auto edge = moldenOrbitalGrid(w, 0, origin, step, {101, 2, 3});
            QVERIFY(std::abs(edge.last() - w.value(0, origin + step.cwiseProduct(Vec(100, 1, 2)))) < 1e-12);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenOrbitalGrid(w, 2, origin, step, grid));
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, moldenOrbitalGrid(w, 0, origin, step, {102, 2, 3}));
        }
    }
    void isosurfaceNormals() {
        // exp(-r²) around the origin (Bohr): a sphere whose normals point straight outward.
        QString text = "gaussian\nlobe\n    1 -2.0 -2.0 -2.0\n";
        for (int axis = 0; axis < 3; ++axis)
            text += QString("   21 %1 %2 %3\n")
                        .arg(axis == 0 ? .2 : 0.)
                        .arg(axis == 1 ? .2 : 0.)
                        .arg(axis == 2 ? .2 : 0.);
        text += "    1 0.0 0.0 0.0 0.0\n";
        for (int x = 0; x < 21; ++x)
            for (int y = 0; y < 21; ++y)
                for (int z = 0; z < 21; ++z) {
                    const Vec p = Vec(x, y, z) * .2 - Vec::Constant(2);
                    text += QString::number(std::exp(-p.squaredNorm()), 'e', 8) + (z % 6 == 5 ? "\n" : " ");
                }
        const auto r = readResult(save("lobe.cube", text + "\n"));
        QVERIFY(!r.surface.isEmpty());
        // A closed surface: triangles share exact vertices, every edge belongs to two of them, and
        // the winding agrees with the outward normals.
        QMap<std::pair<std::array<double, 3>, std::array<double, 3>>, int> edges;
        const auto key = [](const Vec &v) { return std::array<double, 3>{v.x(), v.y(), v.z()}; };
        for (const auto &t : r.surface) {
            QVERIFY(t.positive);
            for (const auto &[point, normal] : {std::pair{t.a, t.na}, {t.b, t.nb}, {t.c, t.nc}}) {
                QVERIFY(std::abs(normal.norm() - 1) < 1e-9);
                QVERIFY(normal.dot(point.normalized()) > .98);
            }
            QVERIFY((t.b - t.a).cross(t.c - t.a).dot(t.na + t.nb + t.nc) > 0);
            for (const auto &[p, q] : {std::pair{t.a, t.b}, {t.b, t.c}, {t.c, t.a}})
                ++edges[std::minmax(key(p), key(q))];
        }
        QVERIFY(std::all_of(edges.cbegin(), edges.cend(), [](int n) { return n == 2; }));
        // The worker hands the normals over with the triangles.
        const auto bin = files.filePath("lobe.bin");
        saveResultData(bin, r);
        const auto loaded = loadResultData(bin);
        QCOMPARE(loaded.surface.size(), r.surface.size());
        QVERIFY((loaded.surface.first().nb - r.surface.first().nb).norm() < 1e-6);
    }
    void metadataSelectionAndTransfer() {
        const auto path = save("orbitals.molden", sample());
        const auto r = readResult(path);
        QCOMPARE(r.selectedOrbital, 0);
        QCOMPARE(r.orbitals.size(), 3);
        QVERIFY(r.orbitalError.isEmpty());
        QVERIFY(!r.surface.isEmpty());
        QCOMPARE(r.orbitalNormalization, "standard");
        QCOMPARE(frontierOrbital(r.orbitals, "Alpha", true), 1);
        QCOMPARE(frontierOrbital(r.orbitals, "Beta"), 2);
        QCOMPARE(frontierOrbital(r.orbitals, "Beta", true), -1);
        const auto p = readResult(path, .03, 0, 1);
        bool positive = false, negative = false;
        for (const auto &t : p.surface) {
            positive |= t.positive;
            negative |= !t.positive;
            QVERIFY(t.positive ? t.a.x() > 0 : t.a.x() < 0);
        }
        QVERIFY(positive && negative);
        const auto bin = files.filePath("result.bin");
        saveResultData(bin, p);
        const auto loaded = loadResultData(bin);
        QCOMPARE(loaded.selectedOrbital, 1);
        QCOMPARE(loaded.orbitalFrame, 0);
        QCOMPARE(loaded.orbitalNormalization, p.orbitalNormalization);
        QCOMPARE(loaded.surface.size(), p.surface.size());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(path, .03, 0, 100));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(path, .03, 0, 0, "auto", 1000));
        auto incomplete = sample();
        incomplete.replace("sp 1 0", "s 1 0").replace("1 1 1", "1 1");
        const auto bad = save("incomplete.molden", incomplete);
        QVERIFY(!readResult(bad).orbitalError.isEmpty());
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, readResult(bad, .03, 0, 0));
        QVERIFY_THROWS_EXCEPTION(std::runtime_error,
                                 readResult(save("duplicate.molden", sample() + "3 1\n")));
    }
    void realAngularReferences() {
        const auto root = qEnvironmentVariable("MOLFORMA_MOLDEN_REFERENCES");
        if (root.isEmpty())
            return;
        for (const auto &name : {"spherical", "cartesian", "g_spherical"}) {
            for (int orbital : {5, 20, 40}) {
                const auto prefix = root + '/' + name;
                const auto r = readResult(prefix + ".molden", .03, 0, orbital - 1);
                const auto cube = readResult(prefix + '_' + QString::number(orbital) + ".cube");
                QCOMPARE(r.orbitalNormalization, "nwchem");
                QCOMPARE(r.selectedOrbital, orbital - 1);
                QVERIFY2(std::abs(r.minimum - cube.minimum) < 2e-5, qPrintable(prefix));
                QVERIFY2(std::abs(r.maximum - cube.maximum) < 2e-5, qPrintable(prefix));
                QCOMPARE(r.surface.size(), cube.surface.size());
                for (int i = 0; i < r.surface.size(); ++i) {
                    QCOMPARE(r.surface[i].positive, cube.surface[i].positive);
                    QVERIFY((r.surface[i].a - cube.surface[i].a).norm() < .001);
                    QVERIFY((r.surface[i].b - cube.surface[i].b).norm() < .001);
                    QVERIFY((r.surface[i].c - cube.surface[i].c).norm() < .001);
                }
            }
        }
    }
};
int main(int argc, char **argv) {
    if (const int code = resultWorkerMain(argc, argv); code >= 0)
        return code;
    QCoreApplication app(argc, argv);
    ResultReadersTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_result_readers.moc"
