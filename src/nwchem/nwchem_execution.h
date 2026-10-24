#pragma once
#include "model/document.h"
#include <QElapsedTimer>
#include <QProcess>
#include <QTimer>

namespace mf {
// Expands "~" and "~/" to the home directory and resolves relative paths against base.
// Every execution path goes through this, and the MCP access policy checks the same result.
QString absolutePath(QString path, const QString &base);
QJsonObject executionDefaults();
// Defaults the user saved in Preferences (QSettings "execution/defaults") for new documents.
// Only machine-wide keys are kept; per-project ones such as reference_directory are not.
QStringList executionPreferenceKeys();
QJsonObject executionPreferences();
void saveExecutionPreferences(const QJsonObject &settings);
// Writes the saved preferences into a new document (only values that differ from the defaults).
void applyExecutionPreferences(Document &document);
QJsonObject executionSettings(const Document &document);
void setExecutionSetting(Document &document, const QString &key, const QJsonValue &value);
struct ExecutionValidation {
    QStringList errors, warnings;
    QString executable, launcher, referenceDirectory;
    QMap<QString, QString> environment; // Only explicit overrides, never the inherited environment.
};
ExecutionValidation validateExecution(const Document &document, const QString &projectDirectory,
                                      bool checkLocal = true);
struct ExecutionPlan {
    QString directory, program, input;
    QStringList arguments;
    QMap<QString, QString> environment;
    QJsonObject manifest;
    int timeoutSeconds = 0;
};
ExecutionPlan executionPlan(const Document &document, const QString &projectDirectory,
                            const QString &jobDirectory, bool checkLocal = true);
// Side-effect-free plan with a placeholder job directory; never saves or launches anything.
ExecutionPlan executionPreviewPlan(const Document &document, const QString &projectDirectory,
                                   bool checkLocal = false);
QString executionCommand(const ExecutionPlan &plan);
ExecutionPlan saveExecutionBundle(const Document &document, const QString &projectDirectory,
                                  bool forRun = false);
// One immutable calculation snapshot per run. Results never modify the editor document.
class NwchemRun : public QObject {
    Q_OBJECT
  public:
    explicit NwchemRun(QObject *parent = nullptr);
    ~NwchemRun() override;
    void start(const Document &document, const QString &projectDirectory);
    void stop();
    void shutdown();
    bool active() const {
        return running;
    }
    const ExecutionPlan &plan() const {
        return current;
    }
    QString state() const {
        return status;
    }
    QString message() const {
        return detail;
    }
    qint64 elapsedSeconds() const;
  signals:
    void changed();

  private:
    QProcess process;
    QTimer timer, killTimer;
    QElapsedTimer elapsed;
    ExecutionPlan current;
    QString status = "idle", detail;
    bool running = false, cancelling = false, timedOut = false;
    qint64 groupId = 0, duration = 0;
    void signalProcess(bool force);
    void finish(int exitCode, QProcess::ExitStatus exitStatus, const QString &error = {});
    void record();
};
} // namespace mf
