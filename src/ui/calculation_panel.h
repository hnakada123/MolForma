#pragma once
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QWidget>
class QCheckBox;
class QComboBox;
class QFormLayout;
class QFrame;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTableWidget;
class QToolButton;
class QTreeWidget;
namespace mf {
class MainWindow;
class ChoiceButtons;
class DiagnosticsList;
class NavList;
class SettingsCard;
// "3 計算条件": the sections of the form in a list at the left, the form as cards in the middle, and
// at the right the calculation in brief, the checks of the input and the settings changed from the
// defaults (each with its own reset).
class CalculationPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CalculationPanel(MainWindow *window);
    void refresh();
    // Shows and focuses the field of a setting key (also for "4 詳細設定" diagnostics).
    void showField(const QString &key);
    QString fieldFor(const QString &message) const;
    // The title of the section that holds a setting ("基本設定"), or empty.
    QString sectionTitle(const QString &key) const;
    // Stop focus-out callbacks before the owner's document and undo stack are destroyed.
    void stopEditing() {
        busy = true;
    }
    void applyChromeStyle();

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    // A card of the form, and the setting keys of its fields.
    struct Section {
        QString key, title;
        SettingsCard *card;
        QFormLayout *form;
        QStringList keys;
        QLabel *note;
        QPushButton *reset;
    };
    MainWindow *window;
    bool busy = false, scrolling = false;
    QWidget *formWidget, *navColumn;
    QMap<QString, QWidget *> fields;
    QMap<QString, QWidget *> rows;       // Setting key → the widget of its row in the form.
    QMap<QString, QLabel *> fieldLabels; // Row labels, with a dot while the setting is changed.
    QMap<QString, QString> sectionOf;    // Setting key → section key.
    QHash<QWidget *, QString> rowKeys;   // Row widget → the setting it belongs to (help rows: none).
    QVector<Section> sections;
    QList<ChoiceButtons *> choices;
    NavList *nav;
    QPushButton *presetButton, *storeButton, *changedOnly;
    QMenu *presetMenu;
    QFrame *banner, *validationCard;
    QLabel *message, *status, *summaryTitle, *summaryTask, *summaryFormula, *summaryElectrons, *summaryCharge,
        *summaryMemory, *changeCount, *noChanges, *reference;
    DiagnosticsList *validation;
    QTreeWidget *changes;
    QPushButton *advancedChanges, *formalCharge;
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
    QJsonArray userPresets;
    QFormLayout *group(const QString &key, const QString &title);
    void addRow(QFormLayout *form, const QString &key, const QString &label, QWidget *field);
    void choice(QFormLayout *form, const QString &label, const QString &key);
    // Segments (or chips) for a choice with few items; the combo box holds the value.
    void buttons(QFormLayout *form, const QString &label, const QString &key, bool chips,
                 const QStringList &texts = {});
    void integer(QFormLayout *form, const QString &label, const QString &key, int minimum, int maximum);
    void real(QFormLayout *form, const QString &label, const QString &key, double minimum, double maximum,
              int decimals);
    void flag(QFormLayout *form, const QString &label, const QString &key);
    void help(QFormLayout *form, const QString &text);
    void change(const QString &key, const QJsonValue &value);
    bool editable() const;
    void reloadPresets();
    QString describePreset(const QJsonObject &preset) const;
    void applyPreset(const QString &name, const QJsonObject &preset);
    void storePreset();
    void refreshElementBases(const QJsonObject &settings);
    void refreshConstraints(const QJsonObject &settings);
    // The section list, the marks of changed fields and the list of changes.
    void refreshChanges(const QJsonObject &settings);
    void filterRows();
    void addConstraint();
    void setScanCoordinate();
    // Settings that differ from the defaults, in the order of the form.
    QStringList changedKeys(const QJsonObject &settings) const;
    void reset(const QStringList &keys);
    void showSection(const QString &key);
    void followScroll();
    QMap<QString, QString> fixes(const QStringList &errors) const;
    void applyFix(const QString &message);
    QString valueText(const QString &key, const QJsonValue &value) const;
    void searchSettings(const QString &text);
};
} // namespace mf
