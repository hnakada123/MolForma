// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "editing/chemistry.h"
#include "results/energy_profile.h"
#include "results/result_loader.h"
#include <functional>

namespace mf {
struct ConformerOptions {
    QString metric = "E";
    double temperature = 298.15, rmsd = .1;
    bool frequencies = false;
    ThermalOptions thermal;
};
struct ConformerAnalysis {
    QJsonArray rows;    // Population rows with an "entry" index into the set.
    QJsonArray spectra; // Weighted members for averagedSpectrum.
};
// The version-1 conformer set, including its job associations and cancellable result reads.
// No widgets: callers supply enqueueing and path access policy at the application boundary.
class ConformerSet : public QObject {
    Q_OBJECT
  public:
    using Enqueue = std::function<QStringList(const QVector<Document> &, bool)>;
    using CheckPath = std::function<void(const QString &)>;
    explicit ConformerSet(QObject *parent = nullptr);
    void setConformers(const Document &base, const QVector<Conformer> &conformers);
    QJsonObject specification() const;
    void setSpecification(const QJsonObject &specification);
    const Document &original() const {
        return base;
    }
    const QJsonArray &entries() const {
        return items;
    }
    const ConformerOptions &options() const {
        return settings;
    }
    void setOptions(const ConformerOptions &options);
    void setSelected(int index, bool selected);
    void setName(int index, const QString &name);
    Document geometry(int index) const;
    QByteArray selectedXyz() const;
    void queueSelected(const Enqueue &enqueue);
    void refreshResults(CheckPath check = {});
    void cancel();
    bool loading() const {
        return reading >= 0;
    }
    ConformerAnalysis analyze() const;
  signals:
    void changed();

  private:
    Document base, pendingOriginal;
    QJsonObject extra;
    QJsonArray items;
    ConformerOptions settings;
    ResultLoader loader;
    QVector<int> pending;
    int reading = -1;
    quint64 generation = 0;
    CheckPath checkPath;
    void readNext();
    void scheduleNext();
    void checkIndex(int index) const;
};
} // namespace mf
