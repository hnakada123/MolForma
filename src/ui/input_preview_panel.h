#pragma once
#include <QWidget>
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTextDocument;
namespace mf {
class MainWindow;
// Colors the comments, keywords and numbers of generated NWChem input, or a fragment of it.
void highlightInput(QTextDocument *document);
class InputPreviewPanel : public QWidget {
    Q_OBJECT
  public:
    explicit InputPreviewPanel(MainWindow *owner);
    void refresh();
    // Only the tab's error color and tool tip, while the tab is hidden and not refreshed.
    void refreshBadge();
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
    void setBadge(bool failed, const QString &tip);
};
} // namespace mf
