#pragma once
#include "editing/chemistry.h"
#include "editing/editor_session.h"
#include <QProcess>
#include <QTemporaryDir>

namespace mf {
struct ChemistryRequest {
    QString mode, smiles;
    Ids moving;
    int count = 10;
    bool remote = false;
};
struct ChemistryResult {
    ChemistryRequest request;
    Document base, document;
    QVector<Conformer> conformers;
    quint64 generation = 0;
};
// Owns worker lifetime, snapshots and candidate application independently of the window.
// The receiver of ready() chooses a conformer and calls propose(), or leaves the result unused.
class ChemistryJob : public QObject {
    Q_OBJECT
  public:
    explicit ChemistryJob(EditorSession &session, QObject *parent = nullptr, QString program = {});
    ~ChemistryJob() override;
    void start(const ChemistryRequest &request);
    bool cancel();
    bool active() const {
        return process != nullptr;
    }
    bool propose(const ChemistryResult &result, int conformer = 0);
  signals:
    void changed();
    void ready(const mf::ChemistryResult &result);
    void failed(const mf::ChemistryRequest &request, QString message, bool starting);

  private:
    EditorSession &session;
    QString program;
    QProcess *process = nullptr;
    std::unique_ptr<QTemporaryDir> scratch;
    quint64 generation = 0;
    void release();
};
int chemistryWorker(const QString &input, const QString &output);
} // namespace mf
