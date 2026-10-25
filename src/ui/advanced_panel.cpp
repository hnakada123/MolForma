// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/advanced_panel.h"
#include "nwchem/nwchem_output.h"
#include "ui/calculation_panel.h"
#include "ui/diagnostics_list.h"
#include "ui/editor_chrome.h"
#include "ui/execution_panel.h"
#include "ui/input_preview_panel.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include <QtWidgets>
#include <algorithm>

namespace mf {
namespace {
// Roles of the name column: the key (and group) of a row, the keyword under the name (for a group,
// its count), and whether the option is changed and used by the current calculation.
constexpr int KeyRole = Qt::UserRole, GroupRole = Qt::UserRole + 1, DetailRole = Qt::UserRole + 2,
              ChangedRole = Qt::UserRole + 3, ActiveRole = Qt::UserRole + 4, KindRole = Qt::UserRole;
// The state of an option as a small colored label: applied, pending, error or blank.
void paintPill(QPainter *p, const QRect &cell, const QString &text, const QString &kind, const QFont &font) {
    const ChromeColors c(QApplication::palette());
    const QColor background = kind == "applied" ? c.tint : kind == "pending" ? c.candidate : c.badTint;
    const QColor color = kind == "applied" ? c.accentText : kind == "pending" ? c.candidateText : c.bad;
    auto small = font;
    small.setPointSizeF(font.pointSizeF() * .88);
    small.setBold(true);
    const QFontMetrics metrics(small);
    const int w = metrics.horizontalAdvance(text) + 14, h = metrics.height() + 4;
    const QRect pill(cell.left() + 6, cell.center().y() - h / 2, w, h);
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(Qt::NoPen);
    p->setBrush(background);
    p->drawRoundedRect(pill, h / 2., h / 2.);
    p->setFont(small);
    p->setPen(color);
    p->drawText(pill, Qt::AlignCenter, text);
}
class NameDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const int line = option.fontMetrics.height();
        return {200, index.parent().isValid() ? 2 * line + 12 : line + 18};
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        // The selection and hover backgrounds of the style, then the texts.
        QStyleOptionViewItem base = option;
        initStyleOption(&base, index);
        base.text.clear();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, p, option.widget);
        const ChromeColors c(option.palette);
        const QRect r = option.rect.adjusted(4, 0, -6, 0);
        auto font = option.font;
        p->save();
        if (!index.parent().isValid()) { // A category: its name and count.
            font.setBold(true);
            font.setPointSizeF(font.pointSizeF() * 1.05);
            p->setFont(font);
            p->setPen(c.text);
            const auto name = index.data().toString();
            p->drawText(r, Qt::AlignVCenter | Qt::AlignLeft, name);
            auto small = option.font;
            small.setPointSizeF(small.pointSizeF() * .9);
            p->setFont(small);
            p->setPen(c.muted);
            p->drawText(r.adjusted(QFontMetrics(font).horizontalAdvance(name) + 10, 0, 0, 0),
                        Qt::AlignVCenter | Qt::AlignLeft, index.data(DetailRole).toString());
            p->restore();
            return;
        }
        const bool changed = index.data(ChangedRole).toBool(), active = index.data(ActiveRole).toBool();
        const int line = option.fontMetrics.height();
        const QRect top(r.left() + 14, r.center().y() - line, r.width() - 14, line),
            bottom(r.left() + 14, r.center().y() + 1, r.width() - 14, line);
        if (changed) {
            p->setRenderHint(QPainter::Antialiasing);
            p->setPen(Qt::NoPen);
            p->setBrush(c.accent);
            p->drawEllipse(QPointF(r.left() + 5, top.center().y() + 1), 3, 3);
        }
        font.setBold(changed);
        p->setFont(font);
        p->setPen(active ? c.text : c.muted);
        p->drawText(top, Qt::AlignVCenter | Qt::AlignLeft,
                    QFontMetrics(font).elidedText(index.data().toString(), Qt::ElideRight, top.width()));
        auto mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        mono.setPointSizeF(option.font.pointSizeF() * .86);
        p->setFont(mono);
        p->setPen(c.muted);
        p->drawText(
            bottom, Qt::AlignVCenter | Qt::AlignLeft,
            QFontMetrics(mono).elidedText(index.data(DetailRole).toString(), Qt::ElideRight, bottom.width()));
        p->restore();
    }
};
class StateDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem base = option;
        initStyleOption(&base, index);
        base.text.clear();
        option.widget->style()->drawControl(QStyle::CE_ItemViewItem, &base, p, option.widget);
        const auto kind = index.data(KindRole).toString(), text = index.data().toString();
        p->save();
        if (kind.isEmpty() && index.parent().isValid()) { // Blank: the default.
            p->setPen(ChromeColors(option.palette).muted);
            p->drawText(option.rect.adjusted(10, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, "—");
        } else if (!kind.isEmpty())
            paintPill(p, option.rect, text, kind, option.font);
        p->restore();
    }
};
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
// A row of the legend: a state as in the table, and what it means.
QWidget *legendRow(const QString &state, const QString &kind, const QString &text) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    auto *pill = new QLabel(state);
    pill->setProperty("pill", kind);
    pill->setAlignment(Qt::AlignCenter);
    layout->addWidget(pill, 0, Qt::AlignTop);
    layout->addWidget(caption(text), 1);
    return row;
}
} // namespace

AdvancedPanel::AdvancedPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("advancedTab");
    setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // The calculation the options are for, search, the filter and the reset of everything.
    auto *toolbar = new QFrame;
    toolbar->setObjectName("advancedToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 10, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    outer->addWidget(toolbar);
    auto *target = new QWidget;
    auto *targetLayout = new QHBoxLayout(target);
    targetLayout->setContentsMargins(0, 0, 0, 0);
    targetLayout->addWidget(caption("対象の計算"));
    context = new QLabel;
    context->setObjectName("advancedContext");
    context->setTextFormat(Qt::PlainText);
    targetLayout->addWidget(context);
    tools->addWidget(target);
    search = new QLineEdit;
    search->setObjectName("advancedSearch");
    search->setProperty("search", true);
    search->setPlaceholderText("項目名・NWChemキーワードで検索");
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(260);
    tools->addWidget(search);
    filter = new QComboBox;
    filter->setObjectName("advancedFilter");
    filter->addItem("現在の計算で使用する項目", "applicable");
    filter->addItem("全項目（保留も表示）", "all");
    filter->addItem("変更した項目のみ", "changed");
    filterButtons =
        new ChoiceButtons(filter, ChoiceButtons::Segments, {"この計算で使う", "すべて", "変更済み"});
    filterButtons->setObjectName("advancedFilters");
    tools->addWidget(filterButtons);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tools->addWidget(spacer);
    resetAll = new QPushButton("すべて既定へ");
    resetAll->setObjectName("advancedResetAll");
    resetAll->setIcon(lineIcon("reset"));
    resetAll->setToolTip("詳細設定の明示指定をすべて解除します。「元に戻す」で戻せます。");
    tools->addWidget(resetAll);
    connect(resetAll, &QPushButton::clicked, this, [this] { reset({}); });

    banner = new QFrame;
    banner->setObjectName("advancedBanner");
    banner->setProperty("banner", true);
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(12, 6, 12, 6);
    message = new QLabel;
    message->setObjectName("advancedMessage");
    message->setWordWrap(true);
    bannerRow->addWidget(message, 1);
    banner->hide();
    outer->addWidget(banner);

    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    outer->addLayout(body, 1);
    // The categories, and what the states of the table mean.
    auto *navColumn = new QFrame;
    navColumn->setObjectName("advancedNav");
    navColumn->setFixedWidth(230);
    auto *navLayout = new QVBoxLayout(navColumn);
    navLayout->setContentsMargins(6, 10, 6, 12);
    navLayout->setSpacing(6);
    categories = new NavList;
    categories->setObjectName("advancedCategories");
    navLayout->addWidget(categories, 1);
    category = new QComboBox(navColumn); // The value behind the list, for callers that set it.
    category->setObjectName("advancedCategory");
    category->hide();
    category->addItem("全カテゴリ", "");
    categories->addEntry("すべてのカテゴリ", QString());
    categories->addHeading("カテゴリ");
    for (const auto &c : nwchemAdvancedGroups()) {
        if (c.value == "visualization")
            categories->addHeading("出力");
        category->addItem(c.label, c.value);
        categories->addEntry(c.value == "visualization" ? QString("CUBE / MOLDEN 出力") : c.label, c.value);
    }
    categories->setCurrentKey(QString());
    connect(categories, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item)
            category->setCurrentIndex(category->findData(item->data(Qt::UserRole).toString()));
    });
    connect(category, &QComboBox::currentIndexChanged, this,
            [this] { categories->setCurrentKey(category->currentData().toString()); });
    for (const auto &[state, kind, text] :
         {std::tuple{"適用", "applied", "今の計算の入力に入ります。"},
          {"保留", "pending",
           "今の計算では使いません。値は保存され、対応する計算に切り替えると有効になります。"},
          {"空欄", "blank",
           "「3 計算条件」かNWChemの既定値を使います。1e-8のような指数表記も入力できます。"}})
        navLayout->addWidget(legendRow(state, kind, text));
    body->addWidget(navColumn);

    // The options.
    auto *center = new QWidget;
    center->setObjectName("advancedCenter");
    center->setProperty("canvas", true);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(16, 16, 16, 16);
    table = new QTreeWidget;
    table->setObjectName("advancedTable");
    table->setColumnCount(4);
    table->setHeaderLabels({"項目 / NWChemキーワード", "値（空欄は既定）", "状態", ""});
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setUniformRowHeights(false);
    table->setIndentation(0);
    table->setRootIsDecorated(false);
    table->setItemsExpandable(false);
    table->setItemDelegateForColumn(Name, new NameDelegate(table));
    table->setItemDelegateForColumn(State, new StateDelegate(table));
    auto *header = table->header();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(Name, QHeaderView::Stretch);
    header->setSectionResizeMode(Value, QHeaderView::Interactive);
    header->setSectionResizeMode(State, QHeaderView::Fixed);
    header->setSectionResizeMode(Reset, QHeaderView::Fixed);
    for (const auto &[column, width] : {std::pair{Value, 280}, {State, 72}, {Reset, 40}})
        table->setColumnWidth(column, width);
    centerLayout->addWidget(table, 1);
    empty = new QLabel("一致する詳細設定はありません。");
    empty->setObjectName("advancedEmpty");
    empty->setProperty("muted", true);
    empty->setAlignment(Qt::AlignCenter);
    centerLayout->addWidget(empty);
    body->addWidget(center, 1);
    for (const auto &g : nwchemAdvancedGroups()) {
        auto *group = new QTreeWidgetItem(table, {g.label});
        group->setData(Name, GroupRole, g.value);
        group->setFlags(Qt::ItemIsEnabled);
        groups[g.value] = group;
        auto *resetGroup = new QPushButton("グループを既定へ");
        resetGroup->setObjectName("advancedReset_" + g.value);
        resetGroup->setProperty("link", true);
        resetGroup->setCursor(Qt::PointingHandCursor);
        resetGroup->setToolTip("「" + g.label + "」の明示指定をすべて解除します。「元に戻す」で戻せます。");
        groupResets[g.value] = resetGroup;
        auto *cell = new QWidget;
        auto *cellLayout = new QHBoxLayout(cell);
        cellLayout->setContentsMargins(0, 0, 8, 0);
        cellLayout->addStretch();
        cellLayout->addWidget(resetGroup);
        table->setItemWidget(group, Value, cell);
        connect(resetGroup, &QPushButton::clicked, this, [this, group = g.value] { reset(group); });
        for (const auto &o : nwchemAdvancedOptions()) {
            if (o.group != g.value)
                continue;
            Row row;
            row.item = new QTreeWidgetItem(group, {o.label});
            row.item->setData(Name, KeyRole, o.key);
            row.item->setData(Name, GroupRole, o.group);
            row.item->setData(Name, DetailRole, o.keyword);
            const auto tip = o.help + "\nNWChem: " + o.keyword + "\n未指定: " + o.defaultText;
            row.item->setToolTip(Name, tip);
            if (o.kind == AdvancedKind::Flag) {
                auto *check = new QCheckBox("オン");
                row.editor = check;
                connect(check, &QCheckBox::toggled, this, [this, key = o.key](bool value) {
                    change(key, value ? QJsonValue(true) : QJsonValue::Undefined);
                });
            } else if (o.kind == AdvancedKind::Choice) {
                auto *choices = new QComboBox;
                choices->addItem(QString(), "");
                for (const auto &c : o.choices)
                    choices->addItem(c.label, c.value);
                row.editor = choices;
                connect(choices, &QComboBox::currentIndexChanged, this, [this, choices, key = o.key] {
                    const auto value = choices->currentData().toString();
                    change(key, value.isEmpty() ? QJsonValue::Undefined : QJsonValue(value));
                });
            } else {
                auto *edit = new QLineEdit;
                edit->setClearButtonEnabled(true);
                row.editor = edit;
                connect(edit, &QLineEdit::editingFinished, this, [this, edit, o] {
                    if (!busy && edit->isModified())
                        change(o.key, parseNwchemAdvanced(o, edit->text()));
                    edit->setModified(false);
                });
            }
            row.editor->setObjectName("advanced_" + o.key);
            row.editor->setToolTip(tip);
            // The editor keeps its own height in the taller row.
            auto *cell = new QWidget;
            auto *cellLayout = new QHBoxLayout(cell);
            cellLayout->setContentsMargins(0, 4, 8, 4);
            cellLayout->addWidget(row.editor);
            table->setItemWidget(row.item, Value, cell);
            row.reset = new QPushButton;
            row.reset->setObjectName("advancedResetField_" + o.key);
            row.reset->setIcon(lineIcon("reset"));
            row.reset->setProperty("flat", true);
            row.reset->setToolTip("この項目を既定に戻します（明示指定を解除）。「元に戻す」で戻せます。");
            auto *resetCell = new QWidget;
            auto *resetLayout = new QHBoxLayout(resetCell);
            resetLayout->setContentsMargins(0, 0, 0, 0);
            resetLayout->addWidget(row.reset, 0, Qt::AlignCenter);
            table->setItemWidget(row.item, Reset, resetCell);
            connect(row.reset, &QPushButton::clicked, this, [this, key = o.key] {
                // Later: the click may not hide its own button while it is still being handled.
                QTimer::singleShot(0, this, [this, key] { change(key, QJsonValue::Undefined); });
            });
            rows[o.key] = row;
        }
    }
    table->expandAll();
    connect(table, &QTreeWidget::currentItemChanged, this, &AdvancedPanel::describe);
    // Working in an editor selects its row, so the description follows the keyboard too.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (auto it = rows.cbegin(); now && it != rows.cend(); ++it)
            if (it->editor == now || it->editor->isAncestorOf(now)) {
                if (table->currentItem() != it->item)
                    table->setCurrentItem(it->item);
                return;
            }
    });

    // The selected option, and the checks of "4 詳細設定".
    auto *inspector = new QScrollArea;
    inspector->setObjectName("advancedInspector");
    inspector->setWidgetResizable(true);
    inspector->setFrameShape(QFrame::NoFrame);
    inspector->setFixedWidth(350);
    auto *side = new QWidget;
    side->setObjectName("advancedSide");
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(18, 16, 18, 16);
    sideLayout->setSpacing(10);
    inspector->setWidget(side);
    body->addWidget(inspector);
    sideLayout->addWidget(caption("選んだ項目"));
    selectedTitle = new QLabel;
    selectedTitle->setObjectName("advancedSelectedTitle");
    selectedTitle->setProperty("cardTitle", true);
    selectedTitle->setWordWrap(true);
    sideLayout->addWidget(selectedTitle);
    auto *keywordRow = new QHBoxLayout;
    keyword = new QLabel;
    keyword->setObjectName("advancedKeyword");
    keyword->setProperty("mono", true);
    keyword->setProperty("muted", true);
    keyword->setTextInteractionFlags(Qt::TextSelectableByMouse);
    keywordRow->addWidget(keyword, 1);
    selectedState = new QLabel;
    selectedState->setObjectName("advancedSelectedState");
    keywordRow->addWidget(selectedState);
    sideLayout->addLayout(keywordRow);
    help = new QLabel;
    help->setObjectName("advancedHelp");
    help->setTextFormat(Qt::PlainText);
    help->setWordWrap(true);
    help->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sideLayout->addWidget(help);
    details = new QWidget;
    auto *detailLayout = new QVBoxLayout(details);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(10);
    auto *facts = new QFormLayout;
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(4);
    for (const auto &[name, label, objectName] : {std::tuple{"既定", &defaultValue, "advancedDefault"},
                                                  {"現在", &currentValue, "advancedCurrent"},
                                                  {"使う計算", &scope, "advancedScope"}}) {
        *label = new QLabel;
        (*label)->setObjectName(objectName);
        (*label)->setWordWrap(true);
        auto *term = new QLabel(name);
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    detailLayout->addLayout(facts);
    detailLayout->addWidget(caption("入力への反映"));
    snippet = new QPlainTextEdit;
    snippet->setObjectName("advancedSnippet");
    snippet->setReadOnly(true);
    snippet->setLineWrapMode(QPlainTextEdit::NoWrap);
    snippet->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    snippet->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    highlightInput(snippet->document());
    detailLayout->addWidget(snippet);
    snippetNote = new QLabel;
    snippetNote->setObjectName("advancedSnippetNote");
    snippetNote->setProperty("muted", true);
    snippetNote->setWordWrap(true);
    detailLayout->addWidget(snippetNote);
    auto *actions = new QHBoxLayout;
    resetSelected = new QPushButton("既定に戻す");
    resetSelected->setObjectName("advancedResetSelected");
    resetSelected->setIcon(lineIcon("reset"));
    showInInput = new QPushButton("入力で見る");
    showInInput->setObjectName("advancedShowInInput");
    showInInput->setIcon(lineIcon("document"));
    showInInput->setToolTip("「6 入力プレビュー」でこの項目の行を表示します。");
    actions->addWidget(resetSelected);
    actions->addWidget(showInInput);
    actions->addStretch();
    detailLayout->addLayout(actions);
    connect(resetSelected, &QPushButton::clicked, this, [this] {
        if (const auto *item = table->currentItem())
            change(item->data(Name, KeyRole).toString(), QJsonValue::Undefined);
    });
    connect(showInInput, &QPushButton::clicked, this, [this] {
        if (const auto line = showInInput->property("line").toInt(); line >= 0)
            window->inputPreview->showLine(line);
    });
    outputSummary = new QLabel;
    outputSummary->setObjectName("advancedOutputSummary");
    outputSummary->setTextFormat(Qt::PlainText);
    outputSummary->setWordWrap(true);
    outputSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(outputSummary);
    sideLayout->addWidget(details);
    sideLayout->addStretch();
    status = new QLabel;
    status->setObjectName("advancedStatus");
    status->setWordWrap(true);
    sideLayout->addWidget(status);
    diagnostics = new DiagnosticsList;
    diagnostics->setObjectName("advancedValidation");
    diagnostics->setFrameShape(QFrame::NoFrame);
    diagnostics->setFitContents(220);
    sideLayout->addWidget(diagnostics);
    // A message naming an option (「…」) shows its row; others belong to "3 計算条件".
    connect(diagnostics, &DiagnosticsList::messageClicked, this, [this](const QString &text) {
        for (const auto &o : nwchemAdvancedOptions())
            if (text.contains("「" + o.label + "」") && rows.contains(o.key)) {
                showOption(o.key);
                return;
            }
        const auto key = window->calculation->fieldFor(text);
        if (!key.isEmpty() && key != "advanced") {
            window->workspaceTabs->setCurrentWidget(window->calculation);
            window->calculation->showField(key);
        }
    });
    connect(search, &QLineEdit::textChanged, this, &AdvancedPanel::filterRows);
    connect(filter, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
    connect(category, &QComboBox::currentIndexChanged, this, &AdvancedPanel::filterRows);
}
void AdvancedPanel::showChanged() {
    window->workspaceTabs->setCurrentWidget(this);
    filter->setCurrentIndex(filter->findData("changed"));
}
void AdvancedPanel::showGroup(const QString &group) {
    window->workspaceTabs->setCurrentWidget(this);
    search->clear();
    category->setCurrentIndex(std::max(0, category->findData(group)));
}
void AdvancedPanel::change(const QString &key, const QJsonValue &value) {
    if (busy || window->hasCandidate() || window->worker ||
        nwchemAdvanced(window->document()).value(key) == value)
        return;
    window->safe([&] {
        window->edit("NWChem詳細設定を変更", [&](Document &d) { setNwchemAdvanced(d, key, value); });
    });
}
void AdvancedPanel::reset(const QString &group) {
    if (busy || window->hasCandidate() || window->worker)
        return;
    window->safe(
        [&] { window->edit("NWChem詳細設定を既定へ", [&](Document &d) { resetNwchemAdvanced(d, group); }); });
}
void AdvancedPanel::showOption(const QString &key) {
    if (!rows.contains(key))
        return;
    const auto &row = rows[key];
    search->clear();
    category->setCurrentIndex(0);
    filter->setCurrentIndex(0);
    if (row.item->isHidden()) // An option kept for another kind of calculation.
        filter->setCurrentIndex(filter->findData("all"));
    table->setCurrentItem(row.item);
    table->scrollToItem(row.item, QAbstractItemView::PositionAtCenter);
    row.editor->setFocus(Qt::OtherFocusReason);
}
void AdvancedPanel::showSnippet(const AdvancedOption &o) {
    // The block of the generated input that carries the option, its own lines marked.
    const auto &d = window->displayDocument();
    snippet->clear();
    snippet->setExtraSelections({});
    showInInput->setProperty("line", -1);
    QString input;
    try {
        input = nwchemInput(d);
    } catch (const std::exception &) {
    }
    const bool active = nwchemAdvancedApplies(o, d), changed = nwchemAdvanced(d).contains(o.key);
    if (input.isEmpty()) {
        snippet->hide();
        snippetNote->setText("入力を生成できません。「6 入力プレビュー」の検証結果を確認してください。");
        return;
    }
    if (!active) {
        snippet->hide();
        snippetNote->setText(changed ? "今の計算では使わないため、入力に入りません（保留）。"
                                     : "今の計算では使いません。");
        return;
    }
    const auto lines = input.split('\n');
    const auto outline = nwchemInputOutline(d, input);
    const auto name = nwchemAdvancedBlock(o, d);
    const auto last = o.keyword.section(' ', -1).toLower();
    const NwchemInputBlock *block = nullptr;
    for (const auto &b : outline.blocks) {
        bool contains = b.keyword == name;
        for (int i = b.line + 1; !contains && i < b.line + b.lines; ++i)
            contains = lines[i].trimmed().startsWith(name + ' ') || lines[i].trimmed() == name;
        // Of several blocks with the keyword, the one that "4 詳細設定" writes into.
        const auto marked = [&](const NwchemInputBlock &x) {
            for (int i = x.line; i < x.line + x.lines; ++i)
                if (outline.advanced.contains(i))
                    return true;
            return false;
        };
        if (contains && (!block || (!marked(*block) && marked(b))))
            block = &b;
    }
    if (!block) {
        snippet->hide();
        snippetNote->setText(changed ? "この値を書き込む行はありません。"
                                     : "空欄のため指定しません（既定値を使います）。");
        return;
    }
    QList<int> own;
    for (int i = block->line; i < block->line + block->lines; ++i)
        if (outline.advanced.contains(i) && lines[i].toLower().contains(last))
            own << i;
    // The header, the option's lines with a neighbor each, and the end; "…" for what is left out.
    QList<int> shown{block->line};
    const int end = block->line + block->lines - 1;
    const auto add = [&](int i) {
        if (i > block->line && i <= end && !shown.contains(i))
            shown << i;
    };
    if (own.isEmpty())
        for (int i = block->line + 1; i <= std::min(end, block->line + 5); ++i)
            add(i);
    for (const int i : std::as_const(own))
        for (const int j : {i - 1, i, i + 1})
            add(j);
    add(end);
    std::sort(shown.begin(), shown.end());
    QStringList text;
    QList<int> marks;
    for (int k = 0; k < shown.size(); ++k) {
        if (k && shown[k] > shown[k - 1] + 1)
            text << "  …";
        if (own.contains(shown[k]))
            marks << text.size();
        text << lines[shown[k]];
    }
    snippet->setPlainText(text.join('\n'));
    QList<QTextEdit::ExtraSelection> selections;
    const ChromeColors colors(palette());
    for (const int mark : std::as_const(marks)) {
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(snippet->document()->findBlockByNumber(mark));
        s.format.setBackground(colors.tint);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        selections << s;
    }
    snippet->setExtraSelections(selections);
    // As tall as its lines, with room for the scroll bar of long lines.
    snippet->setFixedHeight(snippet->fontMetrics().lineSpacing() * int(text.size()) +
                            int(2 * snippet->document()->documentMargin()) + 2 * snippet->frameWidth() +
                            style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 2);
    snippet->show();
    showInInput->setProperty("line", own.isEmpty() ? block->line : own.first());
    snippetNote->setText(changed ? QString() : "空欄のため、ここには指定しません（既定値を使います）。");
}
void AdvancedPanel::describe() {
    const auto *item = table->currentItem();
    const auto key = item ? item->data(Name, KeyRole).toString() : QString();
    const AdvancedOption *option = nullptr;
    for (const auto &o : nwchemAdvancedOptions())
        if (o.key == key)
            option = &o;
    details->setVisible(option);
    keyword->setVisible(option);
    selectedState->setVisible(option);
    if (!option) {
        selectedTitle->setText("項目を選んでください");
        help->setText("表の行を選ぶと、その項目の説明・既定値・使う計算と、入力に書き込む行を表示します。");
        return;
    }
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d);
    const auto &o = *option;
    selectedTitle->setText(o.label);
    keyword->setText(nwchemAdvancedBlock(o, d) == o.keyword || o.group == "files"
                         ? o.keyword
                         : nwchemAdvancedBlock(o, d) + " › " + o.keyword);
    const auto state = item->data(State, KindRole).toString();
    selectedState->setText(item->text(State));
    selectedState->setProperty("pill", state);
    selectedState->setVisible(!state.isEmpty());
    selectedState->style()->unpolish(selectedState);
    selectedState->style()->polish(selectedState);
    const auto error = nwchemAdvancedError(o, a.value(o.key));
    help->setText(o.help + (error.isEmpty() ? QString() : "\nエラー: " + error));
    defaultValue->setText(nwchemAdvancedDefault(o, d));
    QString current = "空欄（既定）";
    if (a.contains(o.key)) {
        current = nwchemAdvancedText(a[o.key]);
        if (o.kind == AdvancedKind::Flag)
            current = a[o.key].toBool() ? "オン" : "オフ";
        for (const auto &c : o.choices)
            if (c.value == a[o.key].toString())
                current = c.label;
    }
    currentValue->setText(current);
    scope->setText(nwchemAdvancedScope(o) +
                   (nwchemAdvancedApplies(o, d) ? QString() : "（今の計算では使いません）"));
    showSnippet(o);
    resetSelected->setEnabled(a.contains(o.key) && !window->hasCandidate() && !window->worker);
    showInInput->setEnabled(showInInput->property("line").toInt() >= 0);
    outputSummary->setVisible(o.group == "visualization");
}
void AdvancedPanel::filterRows() {
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d);
    const auto query = search->text().trimmed();
    const auto mode = filter->currentData().toString(), selectedGroup = category->currentData().toString();
    QSet<QString> visibleGroups;
    QMap<QString, int> counts;
    int visible = 0, total = 0;
    for (const auto &o : nwchemAdvancedOptions()) {
        const bool kept =
            (mode != "applicable" || nwchemAdvancedApplies(o, d)) && (mode != "changed" || a.contains(o.key));
        const bool show =
            kept && (selectedGroup.isEmpty() || selectedGroup == o.group) &&
            (o.label + ' ' + o.keyword + ' ' + o.key + ' ' + o.help).contains(query, Qt::CaseInsensitive);
        rows[o.key].item->setHidden(!show);
        counts[o.group] += kept;
        total += kept;
        if (show) {
            visibleGroups.insert(o.group);
            ++visible;
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it)
        it.value()->setHidden(!visibleGroups.contains(it.key()));
    // The categories count what the filter keeps; a category without any is not used.
    for (int i = 0; i < categories->count(); ++i) {
        auto *entry = categories->item(i);
        if (entry->data(NavList::HeadingRole).toBool())
            continue;
        const auto group = entry->data(Qt::UserRole).toString();
        const int count = group.isEmpty() ? total : counts.value(group);
        const bool unused = count == 0 && mode == "applicable" && !group.isEmpty();
        entry->setData(NavList::BadgeRole,
                       unused || (!count && !group.isEmpty()) ? QString() : QString::number(count));
        entry->setData(NavList::NoteRole, unused ? QString("使わない") : QString());
        entry->setData(NavList::MutedRole, unused);
        bool changedHere = false;
        for (const auto &o : nwchemAdvancedOptions())
            changedHere |= (group.isEmpty() || o.group == group) && a.contains(o.key);
        entry->setData(NavList::MarkRole, changedHere && !group.isEmpty());
    }
    categories->viewport()->update();
    empty->setVisible(!visible);
    table->setVisible(visible);
}
void AdvancedPanel::refresh() {
    if (busy)
        return;
    QScopedValueRollback guard(busy, true);
    const auto &d = window->displayDocument();
    const auto a = nwchemAdvanced(d), s = nwchemSettings(d);
    const bool editable = !window->hasCandidate() && !window->worker;
    int activeCount = 0, pendingCount = 0, changedCount = 0, applicable = 0;
    QMap<QString, int> changedGroups, groupSizes;
    for (const auto &o : nwchemAdvancedOptions()) {
        auto &row = rows[o.key];
        const auto v = a.value(o.key);
        const bool active = nwchemAdvancedApplies(o, d), changed = !v.isUndefined();
        const auto blank = nwchemAdvancedDefault(o, d);
        const QSignalBlocker blocker(row.editor);
        if (auto *line = qobject_cast<QLineEdit *>(row.editor)) {
            const auto value = changed ? nwchemAdvancedText(v) : QString();
            if (line->text() != value)
                line->setText(value);
            line->setPlaceholderText(blank);
        } else if (auto *check = qobject_cast<QCheckBox *>(row.editor)) {
            check->setChecked(v.toBool());
            check->setText(v.toBool() ? "オン" : "オフ");
        } else if (auto *combo = qobject_cast<QComboBox *>(row.editor)) {
            while (combo->count() > o.choices.size() + 1)
                combo->removeItem(combo->count() - 1);
            combo->setItemText(0, "既定（" + blank + "）");
            int index = changed ? combo->findData(v.toString()) : 0;
            if (index < 0) {
                combo->addItem("未対応: " + nwchemAdvancedText(v), v.toString());
                index = combo->count() - 1;
            }
            combo->setCurrentIndex(index);
        }
        row.editor->setEnabled(editable && active);
        row.reset->setEnabled(editable && changed);
        row.reset->setVisible(changed);
        row.item->setData(Name, ChangedRole, changed);
        row.item->setData(Name, ActiveRole, active);
        const auto error = nwchemAdvancedError(o, v);
        const QString kind = !changed           ? QString()
                             : !error.isEmpty() ? "error"
                             : active           ? "applied"
                                                : "pending";
        row.item->setText(State, kind == "error"     ? "エラー"
                                 : kind == "applied" ? "適用"
                                 : kind == "pending" ? "保留"
                                                     : QString());
        row.item->setData(State, KindRole, kind);
        row.item->setToolTip(State, !error.isEmpty()    ? "エラー: " + error
                                    : kind == "applied" ? QString("入力へ反映します。")
                                    : kind == "pending" ? QString("保留: 現在の計算では使用しません。")
                                                        : QString("空欄: 既定値を使います。"));
        ++groupSizes[o.group];
        applicable += active;
        if (changed) {
            ++changedCount;
            active ? ++activeCount : ++pendingCount;
            ++changedGroups[o.group];
        }
    }
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        const int changed = changedGroups.value(it.key());
        it.value()->setData(Name, DetailRole,
                            QString("%1 項目%2")
                                .arg(groupSizes.value(it.key()))
                                .arg(changed ? QString(" · 変更 %1").arg(changed) : QString()));
        groupResets[it.key()]->setVisible(changed);
        groupResets[it.key()]->setEnabled(editable && changed);
    }
    QString task = s["task"].toString();
    for (const auto &c : nwchemChoices("task"))
        if (c.value == task)
            task = c.label;
    context->setText(
        QString("%1 / %2 · %3")
            .arg(s["method"] == "dft" ? nwchemFunctionalLabel(s) : s["method"].toString().toUpper(),
                 nwchemBasisLabel(s), task));
    context->setToolTip(
        QString("詳細指定 %1項目（適用 %2 / 保留 %3）").arg(changedCount).arg(activeCount).arg(pendingCount));
    filterButtons->button(0)->setText(QString("この計算で使う %1").arg(applicable));
    filterButtons->button(1)->setText(QString("すべて %1").arg(rows.size()));
    filterButtons->button(2)->setText(QString("変更済み %1").arg(changedCount));
    resetAll->setEnabled(editable && changedCount);
    message->setText(window->hasCandidate() ? "未確定の分子候補を表示しています。確定・取消後に編集できます。"
                     : window->worker       ? "分子の整形中です。終了後に編集できます。"
                                            : QString());
    banner->setVisible(!message->text().isEmpty());
    // The checks of the options themselves; the rest of the input is checked in "3 計算条件".
    const auto report = validateNwchem(d);
    NwchemValidation own;
    own.reference = report.reference;
    validateNwchemAdvanced(d, own);
    status->setText(own.errors.isEmpty()
                        ? QString("✓ 詳細設定に問題はありません")
                        : QString("詳細設定にエラーがあります（%1 件）").arg(own.errors.size()));
    status->setProperty("good", own.errors.isEmpty());
    status->setProperty("bad", !own.errors.isEmpty());
    status->style()->unpolish(status);
    status->style()->polish(status);
    diagnostics->setMessages(own.errors, own.warnings, {});
    diagnostics->setVisible(diagnostics->count());
    const auto outputs = nwchemOutputFiles(d, report);
    outputSummary->setText(
        "HF/KSの軌道・密度を、計算後に出力します。保存先は「軌道・ファイル」の永続ディレクトリに従います。"
        "この出力設定は計算プリセットの切替後も保持します。\n" +
        (outputs.isEmpty() ? QString("CUBE・MOLDENの出力はオフです。") : "出力予定:\n" + outputs.join('\n')));
    filterRows();
    describe();
}
void AdvancedPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #advancedTab, #advancedCenter { background: @window; }
        #advancedNav { background: @base; border: none; border-right: 1px solid @line; }
        #advancedInspector, #advancedSide { background: @base; }
        #advancedInspector { border: none; border-left: 1px solid @line; }
        QTreeWidget#advancedTable { background: @base; border: 1px solid @line; border-radius: 10px; }
        QTreeWidget#advancedTable::item { border: none; }
        QTreeWidget#advancedTable::item:hover { background: @hover; }
        QTreeWidget#advancedTable::item:selected { background: @tint; color: @text; }
        QPlainTextEdit#advancedSnippet { background: @window; border: 1px solid @line; border-radius: 6px; }
        QLabel[pill] { border-radius: 8px; padding: 1px 7px; font-weight: bold; font-size: 11px; }
        QLabel[pill="applied"] { background: @tint; color: @accentText; }
        QLabel[pill="pending"] { background: @candidate; color: @candidateText; }
        QLabel[pill="error"] { background: @badTint; color: @error; }
        QLabel[pill="blank"] { background: @segment; color: @muted; }
        QListWidget#advancedValidation { background: transparent; }
    )",
                              pal));
}
} // namespace mf
