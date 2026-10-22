#pragma once
#include <QWidget>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
namespace mf {
class MainWindow;
class InputPreviewPanel : public QWidget {
    Q_OBJECT
  public:
    explicit InputPreviewPanel(MainWindow *owner);
    void refresh();
    void exportInput(bool toFile);
    // Save the currently selected input type, after committing any active settings editor.
    void saveInput(const QString &path);

  private:
    MainWindow *window;
    QComboBox *mode, *sections;
    QLineEdit *search;
    QPlainTextEdit *input;
    QTextBrowser *diagnostics;
    QLabel *summary, *status, *hint, *position, *searchStatus;
    QPushButton *copy, *save;
    QString contents;
    QString prepareExport();
    void findText(bool backwards);
    void navigate(int tab);
};
} // namespace mf
