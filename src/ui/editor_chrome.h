// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPalette>
#include <QToolButton>
#include <QVector>
#include <utility>
class QButtonGroup;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QVBoxLayout;
namespace mf {
// Colors of the editor chrome (header, tool rail, option bar, overlays): MolForma's own accent, and
// the rest derived from the widget palette so that light and dark themes both stay readable.
// accentText keeps 4.5:1 on tint.
struct ChromeColors {
    QColor base, window, text, muted, line, hover, segment, accent, accentText, tint, onAccent;
    QColor candidate, candidateLine, candidateText; // The banner of a pending change.
    QColor good, goodTint, bad, badTint;            // Outcomes: text and a tinted background.
    explicit ChromeColors(const QPalette &palette);
};
// A style sheet with the chrome colors put in for the tokens "@accent", "@accentText", "@base",
// "@window", "@text", "@muted", "@line", "@hover", "@segment", "@tint", "@onAccent", "@accentHover",
// "@candidate…", "@good", "@goodTint", "@error" and "@badTint".
QString chromeRules(QString rules, const QPalette &palette);
// The rules shared by the settings tabs (3 計算条件 … 6 入力プレビュー), as tokens for chromeRules():
// the toolbar and banner rows, cards, segmented controls and chips (ChoiceButtons), buttons (with
// the properties primary, danger, link and flat), search fields and status texts.
QString settingsRules();
// Stroked icons on a 24×24 grid ("select", "move", "undo", "lock", "copy", …), in the palette's text color,
// the accent when checked and the disabled color when off, or always in `color` if given. "play", "pause",
// "first" and "last" are filled.
QIcon lineIcon(const QString &name, const QColor &color = {});
// The number of a workflow step in a circle, filled with the accent while its tab is open.
QIcon stepIcon(int number);
// Markers of candidate changes as on the 3D view: "added", "removed" or "moved".
QIcon changeIcon(const QString &kind);

// Lays items out left to right and wraps them into further rows when the width runs out. Spacer
// items and horizontally expanding widgets share the width left over in their row.
class FlowLayout : public QLayout {
  public:
    explicit FlowLayout(QWidget *parent = nullptr, int horizontalSpacing = 8, int verticalSpacing = 6);
    ~FlowLayout() override;
    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;
    // Rows as tall as the tallest item, shown or hidden, so that showing other items in a row
    // does not change the height of the layout.
    void setUniformRows(bool on) {
        uniform = on;
        invalidate();
    }

  private:
    int rowHeight(const QList<QLayoutItem *> &row) const;
    int arrange(const QRect &rect, bool testOnly) const;
    QList<QLayoutItem *> items;
    int horizontal, vertical;
    bool uniform = false;
};

// Buttons for the items of a combo box: a segmented control, or chips that wrap. The combo box
// stays the control that holds the value, hidden inside (so callers and MCP clients that set it
// keep working): a click selects its item, and the buttons follow its current item. A value
// without a button ("未対応: …") is named beside them. The buttons take the combo box's name with
// the item's value ("nwchem_task_energy") and follow its enabled state.
class ChoiceButtons : public QFrame {
  public:
    enum Look { Segments, Chips };
    // labels: shorter texts for the buttons, in the order of the items (else the item texts).
    ChoiceButtons(QComboBox *combo, Look look, const QStringList &labels = {}, QWidget *parent = nullptr);
    QComboBox *comboBox() const {
        return combo;
    }
    QToolButton *button(int index) const {
        return buttons.value(index);
    }
    void sync();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QComboBox *combo;
    QList<QToolButton *> buttons;
    QLabel *other;
};

// The list at the left of a settings tab: headings, and entries with a count or a note at the
// right ("変更 4", "使わない"). Muted entries take no part in the current calculation; a mark is a
// dot before the count. Entries hold their key in Qt::UserRole.
class NavList : public QListWidget {
  public:
    enum Role { HeadingRole = Qt::UserRole + 10, BadgeRole, NoteRole, MutedRole, MarkRole };
    explicit NavList(QWidget *parent = nullptr);
    QListWidgetItem *addHeading(const QString &text);
    QListWidgetItem *addEntry(const QString &text, const QVariant &key);
    QListWidgetItem *entry(const QVariant &key) const;
    // Selects the entry of a key without emitting a click (the list follows the scroll position).
    void setCurrentKey(const QVariant &key);
};

// A card of a settings tab: a bordered box with a title row (the title, then what is added to
// header()) above its contents (body()).
class SettingsCard : public QFrame {
  public:
    explicit SettingsCard(const QString &title, QWidget *parent = nullptr);
    QHBoxLayout *header() const {
        return head;
    }
    QVBoxLayout *body() const {
        return content;
    }
    QLabel *titleLabel() const {
        return heading;
    }

  private:
    QHBoxLayout *head;
    QVBoxLayout *content;
    QLabel *heading;
};

// One line of text, shortened with "…" when narrower than the text (the full text in toolTip).
class ElidedLabel : public QLabel {
  public:
    explicit ElidedLabel(const QString &text = {}, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;
};

// An editing tool in the rail: the icon over the name, and the tool's key in the corner.
class ToolRailButton : public QToolButton {
  public:
    explicit ToolRailButton(const QString &key, QWidget *parent = nullptr);

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    QString key;
};

// Shows a base widget (the 3D view) with small panels kept in its corners above it.
class OverlayHost : public QWidget {
  public:
    explicit OverlayHost(QWidget *base, QWidget *parent = nullptr);
    void addOverlay(QWidget *overlay, Qt::Alignment corner);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    void place();
    QWidget *base;
    QVector<std::pair<QWidget *, Qt::Alignment>> overlays;
};

// The window header: the document on the left, the workflow steps in the middle, and history and
// run on the right. The steps move to a second row when one row would be too narrow.
class HeaderBar : public QWidget {
  public:
    HeaderBar(QWidget *left, QWidget *middle, QWidget *right, QWidget *parent = nullptr);
    bool singleRow() const {
        return single;
    }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent *event) override;

  private:
    int singleRowWidth() const;
    void arrange(bool oneRow);
    QWidget *left, *middle, *right;
    QGridLayout *grid;
    bool single = false;
};
} // namespace mf
