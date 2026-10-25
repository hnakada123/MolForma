#pragma once
#include "nwchem/nwchem_advanced.h"
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
class CalculationSummary;
class ChoiceButtons;
class DiagnosticsList;
class NavList;
class SettingsCard;
class StatusCard;
// "計算条件": the sections of the form in a list at the left, the form as cards in the middle, and
// at the right the calculation in brief, the checks of the input, the detail chosen last and the
// settings changed from the defaults (each with its own reset). The detailed NWChem options are part
// of the sections they belong to ("詳細設定" under the fields of SCF, optimization, environment and
// files), with what NWChem uses while blank, whether the calculation uses them and their lines in
// the input.
class CalculationPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CalculationPanel(MainWindow *window);
    void refresh();
    // Shows and focuses the field of a setting key ("advanced:<key>" for a detailed option).
    void showField(const QString &key);
    // Shows a detailed option (opening its section's details), or the details of a group ("dft").
    void showDetail(const QString &key);
    void showDetailGroup(const QString &group);
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
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    // A detailed option: its row in the details of a section (name and keyword, editor, state, reset).
    struct Detail {
        const AdvancedOption *option = nullptr;
        QString section;
        QWidget *row = nullptr, *editor = nullptr;
        QLabel *name = nullptr, *state = nullptr;
        QPushButton *reset = nullptr;
    };
    // The details of a section, opened under its fields; options the calculation does not use are
    // shown on request.
    struct DetailBox {
        QToolButton *toggle = nullptr;
        QWidget *body = nullptr;
        QPushButton *unused = nullptr;
        QLabel *empty = nullptr;
        QLabel *outputs = nullptr; // The CUBE and MOLDEN files to come (visualization).
        QStringList groups;
        bool showUnused = false;
    };
    MainWindow *window;
    bool busy = false, scrolling = false;
    QMap<QString, Detail> details;         // Option key → its row.
    QMap<QString, DetailBox> detailBoxes;  // Section key → its details.
    QMap<QString, QLabel *> groupHeadings; // Group → its heading in the details.
    QString selectedDetail;
    // The detail chosen last, at the right: what it does, its values and its lines in the input.
    SettingsCard *detailCard;
    QLabel *detailTitle, *detailKeyword, *detailState, *detailHelp, *detailDefault, *detailCurrent,
        *detailScope, *snippetNote;
    QPlainTextEdit *snippet;
    QPushButton *detailReset, *showInInput;
    QWidget *formWidget, *navColumn;
    QMap<QString, QWidget *> fields;
    QMap<QString, QWidget *> rows;       // Setting key → the widget of its row in the form.
    QMap<QString, QLabel *> fieldLabels; // Row labels, with a dot while the setting is changed.
    QMap<QString, QString> sectionOf;    // Setting key → section key.
    QHash<QWidget *, QString> rowKeys;   // Row widget → the setting it belongs to (help rows: none).
    QVector<Section> sections;
    QList<ChoiceButtons *> choices;
    NavList *nav;
    QPushButton *presetButton, *storeButton, *resetAll;
    QComboBox *changeFilter; // All rows or the changed ones, behind its segments.
    ChoiceButtons *changeFilters;
    QMenu *presetMenu;
    QFrame *banner;
    CalculationSummary *summaryCard;
    StatusCard *validationCard;
    QLabel *message, *changeCount, *noChanges, *reference;
    DiagnosticsList *validation;
    QTreeWidget *changes;
    QPushButton *formalCharge;
    QLineEdit *search;
    QScrollArea *scroll;
    QMap<QString, QString> labels; // Setting key → label shown in the form (for search and changes).
    QStringList fieldOrder;        // Keys in the order of the form.
    QTableWidget *elementBases;    // Per-element library basis for the elements of the molecule.
    QTableWidget *constraints;     // Internal-coordinate constraints (ZCOORD) of optimizations.
    QLabel *scanCoordinate, *monomers;
    QLabel *memoryState; // The memory is set in "実行設定": shown with a link there.
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
    // name: of the edit in the undo history (else from the keys); groups: of detailed options, all
    // of them for "*".
    void reset(const QStringList &keys, const QString &name = {}, const QStringList &groups = {});
    void addDetails(const QString &section, const QStringList &groups, bool open);
    // Shows a section's details with what they hold, then lets its layouts settle.
    void openDetails(const QString &section, bool unused);
    void changeDetail(const QString &key, const QJsonValue &value);
    void refreshDetails(const Document &document);
    void selectDetail(const QString &key);
    void describeDetail();
    void showSnippet(const AdvancedOption &option);
    QString detailValueText(const AdvancedOption &option, const QJsonValue &value) const;
    bool changedOnly() const;
    QWidget *readOnlyRow(QLabel *value, QPushButton *link);
    void showSection(const QString &key);
    void followScroll();
    QMap<QString, QString> fixes(const QStringList &errors) const;
    void applyFix(const QString &message);
    QString valueText(const QString &key, const QJsonValue &value) const;
    void searchSettings(const QString &text);
};
} // namespace mf
