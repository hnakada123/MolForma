// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "results/results.h"
#include <memory>
#include <optional>

namespace mf {
// What NWChem prints, read line by line. The wording of its output is known in nwchem_log.cpp and
// nowhere else: the result viewer, the run panel, the history and the diagnostics of a run ask here.

struct NwchemTotalEnergy; // A "Total … energy" line, matched once for everything that reads it.

// The last DRIVER optimization as one unit. Later energy/property/frequency tasks keep it;
// starting another optimization replaces its steps, thresholds and convergence together.
struct NwchemOptimization {
    int task = -1;
    bool present = false, converged = false;
    QVector<ResultStep> steps;
    QJsonObject thresholds;
};
// The energy of a calculation, fed an NWChem log line by line: that of the last task with one; for
// a frequency task the reference structure's (numerical Hessians print the energies of displaced
// structures after it), for BSSE the corrected energy. NWChem reads the input again ("NWChem
// Input Module") before each task.
class NwchemEnergySummary {
  public:
    // Lines add() may use; others can be skipped without decoding them.
    static bool relevant(QByteArrayView line);
    void add(const QString &line);
    // {"hartree", "method", "source": final|frequency_reference|bsse_corrected}; empty without one.
    QJsonObject energy() const;
    // The last counterpoise correction: {"error", "supermolecular", "corrected"} summed over all the
    // monomers, with "nwchem_error" when NWChem printed another error and, when the monomer energies
    // were found, "interaction" and "interaction_uncorrected" (Hartree); empty without one.
    QJsonObject bsse() const;
    // Whether NWChem printed its closing line ("Total times  cpu: 1.2s  wall: 3.4s"), which only a
    // run that ended normally has, and the wall time of that line / s when it gives one.
    bool normalTermination() const {
        return ended;
    }
    std::optional<double> wallSeconds() const {
        return wall;
    }

  private:
    friend class NwchemLog;
    void add(const QString &line, const NwchemTotalEnergy *total);
    struct Task {
        double first = 0, last = 0, corrected = 0, supermolecular = 0, error = 0, nwchemError = 0,
               monomerSum = qQNaN(); // The monomers in their own bases.
        QString firstMethod, lastMethod;
        bool energy = false, frequency = false, bsse = false, raman = false;
        // The energies since the monomers were listed (or since the last correction), for the sum.
        int monomers = 0;
        QVector<QPair<QString, double>> since;
    };
    QVector<Task> tasks{Task{}};
    bool ended = false;
    std::optional<double> wall;
};
// One reading of an NWChem log, fed its lines in order: the whole log for the result viewer, a growing
// log for the run panel, the end of an old one for the history. Sections that span several lines
// (coordinate and normal-mode tables, excited states, charges, thermochemistry, ...) are read as their
// lines arrive.
class NwchemLog {
  public:
    // With a result, everything goes into it and finish() completes it; without, only the progress
    // below and the energy summary are read.
    explicit NwchemLog(ResultData *result = nullptr);
    ~NwchemLog();
    NwchemLog(NwchemLog &&) noexcept;
    NwchemLog &operator=(NwchemLog &&) noexcept;
    void add(const QString &line);
    // A line as read from the file (UTF-8). Without a result, the lines that change neither the
    // progress nor the energy summary are passed over without decoding them.
    void addBytes(QByteArrayView line);
    // The log ends: sections still open are incomplete. Completes the result.
    void finish();
    const NwchemEnergySummary &summary() const;
    // Tasks begun; the current one is started() - 1.
    int started() const;
    const NwchemOptimization &optimization() const;
    // The last total energy printed / Hartree (NaN before one), and its method ("SCF", "MP2"). The
    // energy of the calculation is summary().energy().
    double energy() const;
    QString energyMethod() const;
    // NWChem's closing line (summary().normalTermination()).
    bool ended() const;

  private:
    class Reader;
    std::unique_ptr<Reader> reader;
};
// Whether a file that begins with these bytes is an NWChem output: its banner, or the heading it prints
// as it reads the input.
bool nwchemOutputStart(QByteArrayView head);
// The failures of a run that MolForma tells apart by NWChem's messages, in its output or among its
// errors: the SCF did not converge, an optimization reached its step limit, memory ran out, a basis
// set could not be read.
enum class NwchemFailure { ScfConvergence, OptimizationLimit, Memory, Basis };
inline constexpr NwchemFailure nwchemFailures[] = {NwchemFailure::ScfConvergence,
                                                   NwchemFailure::OptimizationLimit, NwchemFailure::Memory,
                                                   NwchemFailure::Basis};
// Whether a line reports that failure (a line may report more than one).
bool nwchemLineReports(const QString &line, NwchemFailure failure);
} // namespace mf
