// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QFormLayout>
#include <QFrame>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPalette>
#include <QToolButton>
#include <QVector>
#include <functional>
#include <utility>
class QAbstractButton;
class QButtonGroup;
class QComboBox;
class QGridLayout;
class QHBoxLayout;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
namespace mf {
struct Document;
class MoleculeView;
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
// The rules shared by every tab, as tokens for chromeRules(): the toolbar and banner rows, cards
// (with their states), segmented controls and chips (ChoiceButtons), buttons (with the properties
// primary, danger, link and flat), search fields, status texts, tabs with an underline (a QTabWidget
// with the property tabs="underline"), plain tables (plain=true) and logs (log=true).
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

// A QFormLayout whose rows of wrapping text or buttons get the height they need at its width. QFormLayout
// keeps one set of row heights for the two widths it remembers (its size hint's and the last one asked),
// so after a height asked for its size hint's width it laid rows out at the heights of that wider width,
// too short for its own, and they covered each other.
class FormLayout : public QFormLayout {
  public:
    using QFormLayout::QFormLayout;
    void setGeometry(const QRect &rect) override;
};

// Keeps a toolbar laid out by a FlowLayout on one row while it can: before the row would wrap,
// apply(true) makes the bar compact (names of buttons left to their tooltips, narrower fields).
// Widened or shown again, apply(false) tries the full bar, compact again if it still wraps.
class ToolbarFit : public QObject {
  public:
    ToolbarFit(QWidget *bar, std::function<void(bool compact)> apply);
    // What the bar holds changed (a shorter name, a button hidden): the full bar again if it fits.
    void refit();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QWidget *bar;
    std::function<void(bool)> apply;
    bool compact = false;
    int narrowest = 0; // The width of the bar when it became compact.
    void fit();
};
// A button of a compact toolbar: its icon alone, with its name kept in the tooltip and as its
// accessible name; or back with its name.
void setIconOnly(QAbstractButton *button, bool iconOnly);

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
// right ("変更 4", "使わない"). Muted entries take no part in the current calculation.
// Entries hold their key in Qt::UserRole.
class NavList : public QListWidget {
  public:
    enum Role { HeadingRole = Qt::UserRole + 10, BadgeRole, NoteRole, MutedRole };
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
    // An arrow before the title that folds the whole card (for what is seldom needed); while it is
    // folded, `summary` after the title says what is inside. Returns the arrow, for callers that
    // remember the state (its clicked()) or rename it.
    QToolButton *setCollapsible(bool open, const QString &summary = {});
    // Opens or folds a collapsible card as its arrow would.
    void setOpen(bool open);
    void setSummary(const QString &summary);

  private:
    QHBoxLayout *head;
    QVBoxLayout *content;
    QWidget *contents;
    QLabel *heading, *folded = nullptr;
    QToolButton *fold = nullptr;
    void showOpen(bool open);
};

// An arrow and a name in the accent that open what is seldom needed inside a card ("詳細設定",
// "パスの扱いについて"), with what it holds after them in muted text ("21 項目 · 変更 1"). button()
// carries the state and the object name; the caller shows what it opens on its toggled().
// SettingsCard::setCollapsible() folds a whole card instead.
class Disclosure : public QWidget {
  public:
    explicit Disclosure(const QString &text, bool open = false, QWidget *parent = nullptr);
    QToolButton *button() const {
        return toggle;
    }
    bool isOpen() const;
    // Opens or closes it as a click would (toggled() follows).
    void setOpen(bool open);
    void setDetail(const QString &detail);

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    QToolButton *toggle;
    QLabel *detail;
};

// The outcome of a tab's checks, the same card in "2 計算条件" … "4 入力プレビュー": an icon and a
// headline in the color of the outcome over the details, and optionally "再検査".
class StatusCard : public SettingsCard {
  public:
    enum State { Ok, Warning, Error, Waiting };
    explicit StatusCard(QWidget *parent = nullptr);
    void setState(State state, const QString &headline);
    QLabel *headline() const {
        return text;
    }
    // "再検査": for checks of what can change outside MolForma (files, programs).
    QPushButton *addRecheck(const QString &objectName, const QString &toolTip);

  private:
    QLabel *icon, *text;
    State current = Waiting;
};

// A link to another place ("3 実行設定で変更", a tab named as in the text), with an arrow after it.
QPushButton *linkButton(const QString &text, const QString &objectName = {});
// Read-only text of a log or file: fixed-width, not wrapped, in the frame of the logs.
QPlainTextEdit *logView(const QString &objectName);
// A flat button with the copy icon.
QToolButton *copyButton(const QString &objectName, const QString &toolTip);
// Applies style rules that depend on a dynamic property ("state", "pill") again.
void repolish(QWidget *widget);

// The calculation in brief, the same card at the top of the right column of "2 計算条件" and
// "4 入力プレビュー": method / basis, the task, then composition, electrons, charge and memory.
// The memory is set in "3 実行設定": its row links there (openLink receives "memory").
class CalculationSummary : public SettingsCard {
  public:
    // prefix: of the object names ("calculation" → calculationSummary, calculationSummaryTask, …).
    CalculationSummary(const QString &prefix, const QString &caption,
                       const std::function<void(const QString &)> &openLink, QWidget *parent = nullptr);
    void update(const Document &document);

  private:
    QLabel *title, *task, *formula, *electrons, *charge, *memory;
};
// "HF / STO-3G · 一点エネルギー" (and the solvent): the one line of the calculation in the summaries
// and "3 実行設定".
QString calculationBrief(const Document &document);

// The controls of a 3D view, the same over every view: +X/+Y/+Z (that axis toward the viewer, the
// whole molecule shown), "全体を表示" (the whole molecule without turning it), "回転" (sliders for
// the angles, following the mouse) and a "表示" menu. Object names of the parts come from names.
class ViewControls : public QFrame {
  public:
    struct Names {
        QString axes[3], fit, rotate, menu, popup, sliders[3];
    };
    // rotate / menu false: without those buttons (the coordinate table's small view).
    ViewControls(MoleculeView *view, const Names &names, bool rotate = true, bool menu = true,
                 QWidget *parent = nullptr);
    QToolButton *menuButton() const {
        return display;
    }
    // Buttons and sliders, for callers that enable them together.
    QList<QWidget *> controls() const {
        return parts;
    }
    void applyStyle(const QPalette &palette);

  private:
    QToolButton *rotation = nullptr, *display = nullptr;
    QFrame *popup = nullptr;
    QList<QWidget *> parts;
};
// The mouse controls of a 3D view, the hint under every view.
QString viewControlsHint();

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
