#pragma once
#include <QJsonArray>
#include <QMap>
#include <QWidget>
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QFormLayout;
class QVBoxLayout;
namespace mf {
class MainWindow;
class CalculationPanel : public QWidget {
    Q_OBJECT
  public:
    explicit CalculationPanel(MainWindow *window);
    void refresh();
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
    QPlainTextEdit *validation, *preview;
    QPushButton *copy, *save, *undo, *redo, *removePreset;
    QJsonArray userPresets;
    QFormLayout *group(const QString &title, QVBoxLayout *layout);
    void choice(QFormLayout *form, const QString &label, const QString &key);
    void integer(QFormLayout *form, const QString &label, const QString &key, int minimum, int maximum);
    void flag(QFormLayout *form, const QString &label, const QString &key);
    void change(const QString &key, const QJsonValue &value);
    void reloadPresets();
    void describePreset();
    void storePreset();
    void exportInput(bool toFile);
};
} // namespace mf
