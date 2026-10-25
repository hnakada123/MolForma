#pragma once
#include "nwchem/nwchem.h"
#include <QWidget>
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTextBrowser;
class QTextDocument;
class QToolButton;
namespace mf {
class MainWindow;
class ChoiceButtons;
class InputEditor;
// Colors the comments, keywords and numbers of generated NWChem input, or a fragment of it.
void highlightInput(QTextDocument *document);
// "6 入力プレビュー": the blocks of the input with the tabs that set them at the left, the input
// with marks on the lines from "4 詳細設定" and the additional input, and at the right the input in
// brief, its checks and how its paths are written.
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
    // Shows this tab with the cursor on a line (from 0) of the input.
    void showLine(int line);
    void applyChromeStyle();

  private:
    MainWindow *window;
    QComboBox *mode;
    ChoiceButtons *modes;
    QLineEdit *search;
    InputEditor *input;
    QListWidget *outlineList;
    QTextBrowser *diagnostics;
    QFrame *banner, *sourceChip, *checkCard;
    QLabel *summary, *summaryTask, *summaryFormula, *summaryElectrons, *summaryCharge, *summaryMemory,
        *status, *hint, *position, *stats, *searchStatus, *paths, *sourceTitle;
    QToolButton *pathsToggle;
    QWidget *sourceLinks;
    QPushButton *copy, *save;
    QString contents;
    NwchemInputOutline outline;
    QString prepareExport();
    void findText(bool backwards);
    void navigate(int tab);
    // The tab that sets part of the input: by its number (1 分子 … 5 実行設定), at a setting of
    // "3 計算条件" or a group of "4 詳細設定".
    void openSource(int tab, const QString &setting, const QString &group);
    void setBadge(bool failed, const QString &tip);
    void rebuildOutline();
    // The block at the cursor: marked in the outline and the text, with its tabs over the text.
    void followCursor();
};
} // namespace mf
