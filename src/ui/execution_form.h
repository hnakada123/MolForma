// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QWidget>
class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QTableWidget;
namespace mf {
class ChoiceButtons;
class SettingsCard;
// The settings of a run as the cards of "3 実行設定": programs and folders, resources and the extra
// environment. "3 実行設定" edits the project's settings with it and Preferences the defaults of new
// documents, so both have the same controls, ranges (executionRanges) and environment rows.
class ExecutionForm : public QWidget {
    Q_OBJECT
  public:
    // Project adds the reference folder and the help on paths.
    enum class Scope { Project, Preferences };
    // The controls are named <prefix>_<key> ("execution_mpi_ranks"), the browse buttons
    // <prefix>Browse_<key>, and the environment table and its buttons <prefix>Environment,
    // <prefix>AddEnvironment and <prefix>RemoveEnvironment.
    ExecutionForm(Scope scope, const QString &prefix, QWidget *parent = nullptr);
    // Shows the settings without emitting changed().
    void setSettings(const QJsonObject &settings);
    // Emits the rows of the environment table if they were edited since they were shown.
    void commitEnvironment();
    // The control of a setting, the environment table for "environment".
    QWidget *field(const QString &key) const;
    // Under the executable or the launcher: where its name was found, or why it was not.
    void setFound(const QString &key, const QString &text, bool ok);
    // "3 実行設定" puts the memory per process and the use of this PC into the resources card.
    SettingsCard *resourcesCard() const {
        return resources;
    }
    QGridLayout *resourcesGrid() const {
        return resourceGrid;
    }
  signals:
    void changed(const QString &key, const QJsonValue &value);

  private:
    QString prefix;
    QMap<QString, QWidget *> fields;
    QMap<QString, QLabel *> found; // Where the executable and the launcher were found.
    QComboBox *mode;
    ChoiceButtons *modes;
    SettingsCard *resources;
    QGridLayout *resourceGrid;
    QTableWidget *environment;
    QPushButton *removeEnvironment;
    QLabel *environmentEmpty;
    QJsonArray rows; // The environment as last shown, with the fields of its rows the table leaves out.
    bool environmentDirty = false, mpi = true;
    void pathField(QGridLayout *grid, const QString &label, const QString &key, bool directory);
    QWidget *integer(const QString &label, const QString &key, const QString &unit = {});
};
} // namespace mf
