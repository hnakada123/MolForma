// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
#include "ui/editor_context.h"
#include <QWidget>
#include <functional>
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
    // The title of the section of "2 計算条件" that holds a setting ("基本設定"; empty for none), as
    // the panel of that tab names it: the links to a block's settings say where they lead.
    using SectionTitle = std::function<QString(const QString &setting)>;
    InputPreviewPanel(EditorContext &editorContext, UiServices &uiContext, WorkspaceActions &workspace,
                      SectionTitle sectionTitle, QWidget *parent = nullptr);
    void refresh();
    // Only the tab's error color and tool tip, while the tab is hidden and not refreshed.
    void refreshBadge();
    void exportInput(bool toFile);
    // Save the currently selected input type, after committing any active settings editor.
    void saveInput(const QString &path);
    // Shows this tab with the cursor on a line (from 0) of the input.
    void showLine(int line);
    void applyChromeStyle();
  signals:
    // Whether the input cannot be generated, and the state in a line: for the mark and the tool tip of
    // this panel's tab and of its step in the header.
    void badgeChanged(bool failed, QString tip);
    // Before the input is copied or saved: what is being typed in the settings is to be taken in first.
    void settingsCommitRequested();

  private:
    EditorContext &editorContext;
    UiServices &uiContext;
    WorkspaceActions &workspace;
    SectionTitle sectionTitle;
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
    // Where a block is set, one entry per link: the owner whose tab sets it, at a setting or at a
    // group of the details (the calculation's).
    struct Source {
        SettingTarget target;
        QString setting, group;
    };
    static QList<Source> sources(const NwchemInputBlock &block);
    void setBadge(bool failed, const QString &tip);
    void rebuildOutline();
    // The block at the cursor: marked in the outline and the text, with its tabs over the text.
    void followCursor();
};
} // namespace mf
