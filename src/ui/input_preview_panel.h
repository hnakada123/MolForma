// SPDX-License-Identifier: GPL-2.0-or-later
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
class QToolButton;
namespace mf {
class MainWindow;
class CalculationSummary;
class ChoiceButtons;
class InputEditor;
class SettingsCard;
class StatusCard;
// "4 入力プレビュー": the blocks of the input with the tabs that set them at the left, the input
// with marks on the lines from the details and the additional input, and at the right the input in
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
    QFrame *banner, *sourceChip;
    CalculationSummary *summaryCard;
    StatusCard *checkCard;
    QLabel *hint, *position, *stats, *searchStatus, *paths, *sourceTitle;
    SettingsCard *pathCard; // "書き出し後のパス" or "実行時のパス", folded with a summary.
    QWidget *sourceLinks;
    QPushButton *copy, *save;
    QString contents;
    NwchemInputOutline outline;
    QString prepareExport();
    void findText(bool backwards);
    void navigate(int tab);
    // The tab that sets part of the input: by its number (1 分子 … 3 実行設定), at a setting or at a
    // group of the details of "2 計算条件".
    void openSource(int tab, const QString &setting, const QString &group);
    // Where a block is set, one entry per link: tab, setting and group (the details: tab 2).
    struct Source {
        int tab;
        QString setting, group;
    };
    static QList<Source> sources(const NwchemInputBlock &block);
    void setBadge(bool failed, const QString &tip);
    void rebuildOutline();
    // The block at the cursor: marked in the outline and the text, with its tabs over the text.
    void followCursor();
};
} // namespace mf
