#pragma once
#include <QJsonArray>
#include <QMap>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QFormLayout;
class QTableWidget;
class QVBoxLayout;
class QLineEdit;
class QScrollArea;
class QToolButton;
namespace mf {
class MainWindow;
class DiagnosticsList;
class CalculationPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CalculationPanel(MainWindow *window);
    void refresh();
    void exportInput(bool toFile);
    // Shows and focuses the field of a setting key (also for "4 詳細設定" diagnostics).
    void showField(const QString &key);
    QString fieldFor(const QString &message) const;
    // Stop focus-out callbacks before the owner's document and undo stack are destroyed.
    void stopEditing() {
        busy = true;
    }

  private:
    MainWindow *window;
    bool busy = false;
    QWidget *formWidget;
    QMap<QString, QWidget *> fields;
    QComboBox *presets;
    QLabel *summary, *presetSummary, *message;
    QPlainTextEdit *preview;
    DiagnosticsList *validation;
    QLabel *changesFromDefaults;
    QToolButton *showInput;
    QLineEdit *search;
    QScrollArea *scroll;
    QMap<QString, QString> labels; // Setting key → label shown in the form (for search and changes).
    QStringList fieldOrder;        // Keys in the order of the form.
    QTableWidget *elementBases;    // Per-element library basis for the elements of the molecule.
    QTableWidget *constraints;     // Internal-coordinate constraints (ZCOORD) of optimizations.
    QLabel *scanCoordinate, *monomers;
    QCheckBox *moldenOutput; // The MOLDEN file of "4 詳細設定", for the orbital shapes in the viewer.
    QPlainTextEdit *extraInput, *extraTasks;
    QString appliedExtra[2]; // Stored additional input last shown, so typing is not overwritten.
    QPushButton *copy, *save, *undo, *redo, *removePreset;
    QJsonArray userPresets;
    QFormLayout *group(const QString &title, QVBoxLayout *layout);
    void choice(QFormLayout *form, const QString &label, const QString &key);
    void integer(QFormLayout *form, const QString &label, const QString &key, int minimum, int maximum);
    void real(QFormLayout *form, const QString &label, const QString &key, double minimum, double maximum,
              int decimals);
    void flag(QFormLayout *form, const QString &label, const QString &key);
    void change(const QString &key, const QJsonValue &value);
    void reloadPresets();
    void describePreset();
    void storePreset();
    void refreshElementBases(const QJsonObject &settings);
    void refreshConstraints(const QJsonObject &settings);
    void addConstraint();
    void setScanCoordinate();
    QMap<QString, QString> fixes(const QStringList &errors) const;
    void applyFix(const QString &message);
    QString valueText(const QString &key, const QJsonValue &value) const;
    void searchSettings(const QString &text);
};
} // namespace mf
