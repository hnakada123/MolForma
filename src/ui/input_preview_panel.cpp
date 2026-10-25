// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/input_preview_panel.h"
#include "editing/coordinates.h"
#include "nwchem/nwchem_advanced.h"
#include "ui/advanced_panel.h"
#include "ui/calculation_panel.h"
#include "ui/editor_chrome.h"
#include "ui/execution_panel.h"
#include "ui/main_window.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
namespace {
class InputHighlighter : public QSyntaxHighlighter {
  public:
    explicit InputHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}

  protected:
    void highlightBlock(const QString &text) override {
        // Only color the generated input; this is not an NWChem parser.
        static const QRegularExpression tokens(
            R"re(#[^\n]*|"[^"]*"|\b(?:start|title|memory|echo|scratch_dir|permanent_dir|charge|geometry|constraints|basis|ecp|bq|scf|dft|mp2|ccsd|driver|freq|cosmo|property|dplot|moldenfile|molden_norm|gaussian|limitxyz|orbitals|vectors|task|end)\b|(?<!\w)[+-]?(?:\d+\.?\d*|\.\d+)(?:[eEdD][+-]?\d+)?\b)re",
            QRegularExpression::CaseInsensitiveOption);
        auto matches = tokens.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const auto token = match.captured();
            QTextCharFormat format;
            const auto palette = qApp->palette();
            // Darker than the accents of the chrome, so that each reads at 4.5:1 on white.
            if (token.startsWith('#'))
                format.setForeground(accentColor(palette, "#64748b", "#a0aec0"));
            else if (token.startsWith('"'))
                format.setForeground(accentColor(palette, "#16836a", "#5fd3b4"));
            else if (token.front().isLetter()) {
                format.setForeground(accentColor(palette, "#2f66b3", "#7fb0ff"));
                format.setFontWeight(QFont::Bold);
            } else
                format.setForeground(accentColor(palette, "#9a4fa8", "#d9a3e6"));
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};
QColor advancedMark(const QPalette &palette) {
    return accentColor(palette, "#8250df", "#b392f0");
}
QColor extraMark(const QPalette &palette) {
    return accentColor(palette, "#d97706", "#f0b35a");
}
QLabel *caption(const QString &text) {
    auto *label = new QLabel(text);
    label->setProperty("caption", true);
    label->setWordWrap(true);
    return label;
}
QString tabLink(int index, const QString &text) {
    return QString("<a href=\"tab:%1\">%2</a>").arg(index).arg(text.toHtmlEscaped());
}
const QStringList tabNames{"", "1 分子", "2 座標", "3 計算条件", "4 詳細設定", "5 実行設定"};
} // namespace

// The input, read only, with line numbers in a gutter (so copied or saved input never contains
// them) and the lines from "4 詳細設定" and the additional input marked at its edge.
class InputEditor : public QPlainTextEdit {
    class Gutter : public QWidget {
        InputEditor *editor;

      public:
        explicit Gutter(InputEditor *owner) : QWidget(owner), editor(owner) {}
        void paintEvent(QPaintEvent *event) override {
            editor->paintNumbers(event);
        }
    };
    Gutter *gutter;
    int gutterWidth() const {
        return 22 + fontMetrics().horizontalAdvance('9') * QString::number(blockCount()).size();
    }
    void paintNumbers(QPaintEvent *event) {
        QPainter painter(gutter);
        const ChromeColors c(palette());
        painter.fillRect(event->rect(), c.window);
        const int current = textCursor().blockNumber();
        auto block = firstVisibleBlock();
        while (block.isValid()) {
            const auto geometry = blockBoundingGeometry(block).translated(contentOffset());
            if (geometry.top() > event->rect().bottom())
                break;
            const int line = block.blockNumber();
            if (block.isVisible()) {
                const QColor mark = advanced.contains(line) ? advancedMark(palette())
                                    : extra.contains(line)  ? extraMark(palette())
                                                            : QColor();
                if (mark.isValid())
                    painter.fillRect(QRectF(0, geometry.top(), 3, geometry.height()), mark);
                auto font = this->font();
                font.setBold(line == current);
                painter.setFont(font);
                painter.setPen(line == current ? c.accentText : c.muted);
                painter.drawText(0, qRound(geometry.top()), gutter->width() - 8, fontMetrics().height(),
                                 Qt::AlignRight, QString::number(line + 1));
            }
            block = block.next();
        }
    }
    void updateGutter() {
        setViewportMargins(gutterWidth(), 0, 0, 0);
        const auto area = contentsRect();
        gutter->setGeometry(area.left(), area.top(), gutterWidth(), area.height());
        gutter->update();
    }
    void resizeEvent(QResizeEvent *event) override {
        QPlainTextEdit::resizeEvent(event);
        updateGutter();
    }

  public:
    QSet<int> advanced, extra;
    InputEditor() : gutter(new Gutter(this)) {
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        setFrameShape(QFrame::NoFrame);
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutter(); });
        connect(this, &QPlainTextEdit::updateRequest, gutter, [this] { gutter->update(); });
        connect(this, &QPlainTextEdit::cursorPositionChanged, gutter, [this] { gutter->update(); });
        new InputHighlighter(document());
        updateGutter();
    }
    // A tint behind the lines of a block (from 0), stronger on the cursor's line.
    void markBlock(int first, int count) {
        QList<QTextEdit::ExtraSelection> selections;
        auto color = ChromeColors(palette()).accent;
        for (int line = first; line < first + count; ++line) {
            QTextEdit::ExtraSelection s;
            s.cursor = QTextCursor(document()->findBlockByNumber(line));
            color.setAlpha(line == textCursor().blockNumber() ? 46 : 20);
            s.format.setBackground(color);
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            selections << s;
        }
        setExtraSelections(selections);
    }
};

void highlightInput(QTextDocument *document) {
    new InputHighlighter(document);
}
InputPreviewPanel::InputPreviewPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("inputPreviewTab");
    setAttribute(Qt::WA_StyledBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Which input, search, and copy or save it.
    auto *toolbar = new QFrame;
    toolbar->setObjectName("inputPreviewToolbar");
    toolbar->setProperty("toolbar", true);
    auto *tools = new FlowLayout(toolbar, 8, 6);
    tools->setContentsMargins(12, 8, 12, 8);
    outer->addWidget(toolbar);
    auto *modeBox = new QWidget;
    auto *modeRow = new QHBoxLayout(modeBox);
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->addWidget(caption("表示する入力"));
    mode = new QComboBox;
    mode->setObjectName("inputPreviewMode");
    mode->addItem("書出し用入力（設定どおりのパス）", "input");
    mode->addItem("実行時の入力（入力パスを解決）", "execution");
    modes = new ChoiceButtons(mode, ChoiceButtons::Segments, {"書出し用", "実行時（パスを解決）"});
    modes->setObjectName("inputPreviewModes");
    modeRow->addWidget(modes);
    tools->addWidget(modeBox);
    auto *reload = new QPushButton("再検証");
    reload->setObjectName("inputPreviewRefresh");
    reload->setIcon(lineIcon("refresh"));
    reload->setToolTip("表示は編集に追従します。参照ファイル・保存先など、MolFormaの外で変わったものを"
                       "確認し直すときに使います。");
    tools->addWidget(reload);
    auto *gap = new QWidget;
    gap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tools->addWidget(gap);
    auto *find = new QWidget;
    auto *findRow = new QHBoxLayout(find);
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->setSpacing(4);
    search = new QLineEdit;
    search->setObjectName("inputPreviewSearch");
    search->setProperty("search", true);
    search->setPlaceholderText("入力内を検索（Ctrl+F）");
    search->setClearButtonEnabled(true);
    search->addAction(lineIcon("search"), QLineEdit::LeadingPosition);
    search->setFixedWidth(240);
    findRow->addWidget(search);
    for (bool backwards : {true, false}) {
        auto *button = new QPushButton;
        button->setObjectName(backwards ? "inputPreviewPrevious" : "inputPreviewNext");
        button->setIcon(lineIcon(backwards ? "previous" : "next"));
        button->setToolTip(backwards ? "前を検索" : "次を検索");
        findRow->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, backwards] { findText(backwards); });
    }
    find->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); // As wide as its parts.
    search->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    tools->addWidget(find);
    copy = new QPushButton("全文をコピー");
    save = new QPushButton("入力を保存…");
    copy->setObjectName("inputPreviewCopy");
    save->setObjectName("inputPreviewSave");
    copy->setIcon(lineIcon("copy"));
    save->setIcon(lineIcon("apply"));
    tools->addWidget(copy);
    tools->addWidget(save);

    banner = new QFrame;
    banner->setObjectName("inputPreviewBanner");
    banner->setProperty("banner", true);
    auto *bannerRow = new QHBoxLayout(banner);
    bannerRow->setContentsMargins(12, 6, 12, 6);
    hint = new QLabel;
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    hint->setObjectName("inputPreviewHint");
    bannerRow->addWidget(hint, 1);
    banner->hide();
    outer->addWidget(banner);

    auto *body = new QHBoxLayout;
    body->setSpacing(0);
    outer->addLayout(body, 1);
    // The blocks of the input, each with the tabs that set it.
    auto *outlineColumn = new QFrame;
    outlineColumn->setObjectName("inputPreviewOutlineColumn");
    outlineColumn->setFixedWidth(270);
    auto *outlineLayout = new QVBoxLayout(outlineColumn);
    outlineLayout->setContentsMargins(8, 12, 8, 12);
    outlineLayout->setSpacing(6);
    auto *outlineHead = new QHBoxLayout;
    outlineHead->setContentsMargins(8, 0, 8, 0);
    outlineHead->addWidget(caption("入力の構成"));
    outlineHead->addStretch();
    outlineHead->addWidget(caption("編集元"));
    outlineLayout->addLayout(outlineHead);
    outlineList = new QListWidget;
    outlineList->setObjectName("inputPreviewOutline");
    outlineList->setProperty("nav", true);
    outlineList->setFrameShape(QFrame::NoFrame);
    outlineList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outlineLayout->addWidget(outlineList, 1);
    outlineLayout->addWidget(caption("番号はその行を設定するタブです。押すとそのタブへ移ります。"));
    connect(outlineList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row < 0 || row >= outline.blocks.size())
            return;
        QTextCursor cursor(input->document()->findBlockByNumber(outline.blocks[row].line));
        input->setTextCursor(cursor);
        input->centerCursor();
    });
    body->addWidget(outlineColumn);

    // The input with the tabs of the block at the cursor over it, and a status line.
    auto *center = new QWidget;
    center->setObjectName("inputPreviewCenter");
    center->setProperty("canvas", true);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(16, 16, 16, 16);
    auto *code = new QFrame;
    code->setObjectName("inputPreviewCode");
    auto *codeLayout = new QVBoxLayout(code);
    codeLayout->setContentsMargins(1, 1, 1, 1);
    codeLayout->setSpacing(0);
    input = new InputEditor;
    input->setObjectName("inputPreviewText");
    input->setPlaceholderText("分子と計算条件を設定すると、NWChem入力を自動表示します。");
    auto *host = new OverlayHost(input);
    codeLayout->addWidget(host, 1);
    sourceChip = new QFrame;
    sourceChip->setObjectName("inputPreviewSource");
    auto *chipLayout = new QHBoxLayout(sourceChip);
    chipLayout->setContentsMargins(10, 5, 6, 5);
    chipLayout->setSpacing(8);
    sourceTitle = new QLabel;
    sourceTitle->setObjectName("inputPreviewSourceTitle");
    chipLayout->addWidget(sourceTitle);
    sourceLinks = new QWidget;
    auto *linksLayout = new QHBoxLayout(sourceLinks);
    linksLayout->setContentsMargins(0, 0, 0, 0);
    linksLayout->setSpacing(4);
    chipLayout->addWidget(sourceLinks);
    sourceChip->hide();
    host->addOverlay(sourceChip, Qt::AlignTop | Qt::AlignRight);
    auto *footer = new QFrame;
    footer->setObjectName("inputPreviewFooter");
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 5, 12, 5);
    footerRow->setSpacing(14);
    position = new QLabel;
    position->setObjectName("inputPreviewPosition");
    footerRow->addWidget(position);
    stats = new QLabel;
    stats->setObjectName("inputPreviewStats");
    footerRow->addWidget(stats);
    searchStatus = new QLabel;
    searchStatus->setObjectName("inputPreviewSearchStatus");
    footerRow->addWidget(searchStatus, 1);
    // The legend of the marks: a short bar in the color of each.
    for (const auto &[name, text] : {std::pair{"inputPreviewLegendAdvanced", "4 詳細設定から"},
                                     {"inputPreviewLegendExtra", "追加入力から"}}) {
        auto *swatch = new QLabel;
        swatch->setObjectName(name);
        swatch->setFixedSize(4, 13);
        footerRow->addWidget(swatch);
        footerRow->addSpacing(-8);
        footerRow->addWidget(new QLabel(text));
    }
    codeLayout->addWidget(footer);
    centerLayout->addWidget(code, 1);
    body->addWidget(center, 1);

    // The input in brief, its checks, and how its paths are written.
    auto *rail = new QWidget;
    rail->setObjectName("inputPreviewRail");
    rail->setProperty("canvas", true);
    rail->setFixedWidth(330);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 16, 16, 16);
    railLayout->setSpacing(12);
    body->addWidget(rail);
    auto *summaryCard = new SettingsCard({});
    summaryCard->body()->setSpacing(4);
    summaryCard->body()->addWidget(caption("この入力"));
    summary = new QLabel;
    summary->setObjectName("inputPreviewSummary");
    summary->setProperty("cardTitle", true);
    summary->setWordWrap(true);
    summaryCard->body()->addWidget(summary);
    summaryTask = new QLabel;
    summaryTask->setWordWrap(true);
    summaryCard->body()->addWidget(summaryTask);
    auto *facts = new QFormLayout;
    facts->setContentsMargins(0, 6, 0, 0);
    facts->setHorizontalSpacing(14);
    facts->setVerticalSpacing(3);
    for (const auto &[name, label] : {std::pair{"組成", &summaryFormula},
                                      {"電子数", &summaryElectrons},
                                      {"電荷・多重度", &summaryCharge},
                                      {"メモリ", &summaryMemory}}) {
        *label = new QLabel;
        (*label)->setWordWrap(true);
        auto *term = new QLabel(name);
        term->setProperty("muted", true);
        facts->addRow(term, *label);
    }
    summaryCard->body()->addLayout(facts);
    railLayout->addWidget(summaryCard);
    auto *validation = new SettingsCard({});
    checkCard = validation;
    validation->setObjectName("inputPreviewCheck");
    status = new QLabel;
    status->setObjectName("inputPreviewStatus");
    status->setWordWrap(true);
    validation->body()->addWidget(status);
    diagnostics = new QTextBrowser;
    diagnostics->setObjectName("inputPreviewDiagnostics");
    diagnostics->setOpenLinks(false);
    diagnostics->setOpenExternalLinks(false);
    diagnostics->setFrameShape(QFrame::NoFrame);
    validation->body()->addWidget(diagnostics);
    railLayout->addWidget(validation);
    auto *pathCard = new SettingsCard({});
    pathCard->setObjectName("inputPreviewPathCard");
    pathsToggle = new QToolButton;
    pathsToggle->setObjectName("inputPreviewPathsToggle");
    pathsToggle->setCheckable(true);
    pathsToggle->setProperty("flat", true);
    pathsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pathCard->header()->insertWidget(0, pathsToggle);
    paths = new QLabel;
    paths->setObjectName("inputPreviewPaths");
    paths->setWordWrap(true);
    paths->setTextFormat(Qt::RichText);
    pathCard->body()->addWidget(paths);
    railLayout->addWidget(pathCard);
    railLayout->addStretch();
    pathsToggle->setChecked(window->settings.value("inputPreview/showPaths", true).toBool());
    paths->setVisible(pathsToggle->isChecked());
    pathsToggle->setArrowType(pathsToggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    connect(pathsToggle, &QToolButton::toggled, this, [this](bool on) {
        paths->setVisible(on);
        pathsToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        window->settings.setValue("inputPreview/showPaths", on);
    });
    const auto openLink = [this](const QString &link) {
        bool ok = false;
        const int tab = QUrl(link).path().toInt(&ok);
        if (QUrl(link).scheme() == "tab" && ok)
            navigate(tab);
    };
    connect(paths, &QLabel::linkActivated, this, openLink);
    connect(diagnostics, &QTextBrowser::anchorClicked, this,
            [openLink](const QUrl &url) { openLink(url.toString()); });

    connect(mode, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    connect(copy, &QPushButton::clicked, this, [this] { exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { exportInput(true); });
    connect(search, &QLineEdit::returnPressed, this, [this] { findText(false); });
    connect(search, &QLineEdit::textChanged, searchStatus, &QLabel::clear);
    connect(input, &QPlainTextEdit::cursorPositionChanged, this, &InputPreviewPanel::followCursor);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        search->setFocus();
        search->selectAll();
    });
}
void InputPreviewPanel::showLine(int line) {
    window->workspaceTabs->setCurrentWidget(this);
    window->refreshPanel(this);
    QTextCursor cursor(input->document()->findBlockByNumber(line));
    input->setTextCursor(cursor);
    input->centerCursor();
    input->setFocus();
}
void InputPreviewPanel::navigate(int tab) {
    if (tab >= 0 && tab < window->workspaceTabs->count())
        window->workspaceTabs->setCurrentIndex(tab);
}
void InputPreviewPanel::openSource(int tab, const QString &setting, const QString &group) {
    navigate(tab - 1);
    if (tab == 3 && !setting.isEmpty())
        window->calculation->showField(setting);
    else if (tab == 4 && !group.isEmpty())
        window->advanced->showGroup(group);
}
void InputPreviewPanel::rebuildOutline() {
    const QSignalBlocker blocker(outlineList);
    outlineList->clear();
    const ChromeColors c(palette());
    for (int i = 0; i < outline.blocks.size(); ++i) {
        const auto &b = outline.blocks[i];
        auto *item = new QListWidgetItem(outlineList);
        item->setData(Qt::UserRole, b.line);
        item->setToolTip(b.title);
        auto *row = new QWidget;
        row->setObjectName(QString("inputPreviewBlock%1").arg(b.line + 1));
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(8, 3, 6, 3);
        layout->setSpacing(6);
        auto *number = new QLabel(QString::number(b.line + 1));
        number->setProperty("mono", true);
        number->setProperty("muted", true);
        number->setMinimumWidth(number->fontMetrics().horizontalAdvance("9999"));
        layout->addWidget(number);
        auto *title = new ElidedLabel(b.title);
        title->setProperty("blockTitle", true);
        layout->addWidget(title, 1);
        for (const int tab : b.tabs) {
            auto *badge = new QToolButton;
            badge->setObjectName(QString("inputPreviewSource%1_%2").arg(b.line + 1).arg(tab));
            badge->setText(QString::number(tab));
            badge->setProperty("source", tab == 4                     ? "advanced"
                                         : b.setting == "extra_input" ? "extra"
                                                                      : "tab");
            badge->setToolTip("「" + tabNames.value(tab) + "」で編集");
            badge->setAccessibleName(tabNames.value(tab) + "で編集");
            badge->setFixedSize(22, 22);
            layout->addWidget(badge);
            connect(badge, &QToolButton::clicked, this,
                    [this, tab, setting = b.setting, group = b.group] { openSource(tab, setting, group); });
        }
        item->setSizeHint(QSize(0, row->sizeHint().height()));
        outlineList->setItemWidget(item, row);
    }
}
void InputPreviewPanel::followCursor() {
    const auto cursor = input->textCursor();
    const int line = cursor.blockNumber();
    position->setText(QString("行 %1 / 列 %2").arg(line + 1).arg(cursor.positionInBlock() + 1));
    int found = -1;
    for (int i = 0; i < outline.blocks.size(); ++i)
        if (line >= outline.blocks[i].line && line < outline.blocks[i].line + outline.blocks[i].lines)
            found = i;
    {
        const QSignalBlocker blocker(outlineList);
        outlineList->setCurrentRow(found);
    }
    if (found < 0 || contents.isEmpty()) {
        input->setExtraSelections({});
        sourceChip->hide();
        return;
    }
    const auto &b = outline.blocks[found];
    input->markBlock(b.line, b.lines);
    // Where this block is set, as links to the tabs.
    sourceTitle->setText(b.keyword + " ブロックの編集元");
    while (auto *item = sourceLinks->layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const int tab : b.tabs) {
        QString text = tabNames.value(tab);
        if (tab == 3 && !b.setting.isEmpty() && b.setting != "extra_input") {
            if (const auto section = window->calculation->sectionTitle(b.setting); !section.isEmpty())
                text += " › " + section;
        } else if (tab == 3 && b.setting == "extra_input")
            text += " › 追加入力";
        else if (tab == 4)
            for (const auto &g : nwchemAdvancedGroups())
                if (g.value == b.group)
                    text += " › " + (g.value == "visualization" ? QString("CUBE / MOLDEN") : g.label);
        auto *link = new QPushButton(text);
        link->setProperty("link", true);
        link->setCursor(Qt::PointingHandCursor);
        sourceLinks->layout()->addWidget(link);
        connect(link, &QPushButton::clicked, this,
                [this, tab, setting = b.setting, group = b.group] { openSource(tab, setting, group); });
    }
    sourceChip->setVisible(!b.tabs.isEmpty());
    sourceChip->adjustSize();
}
void InputPreviewPanel::refresh() {
    const auto &d = window->displayDocument();
    const auto s = nwchemSettings(d);
    const auto report = validateNwchem(d);
    NwchemValidation advanced;
    advanced.reference = report.reference;
    validateNwchemAdvanced(d, advanced);
    auto geometryErrors = d.validate();
    try {
        if (!d.atoms.isEmpty())
            nwchemGeometry(d, CoordinateUnit::Angstrom);
    } catch (const std::exception &e) {
        geometryErrors << QString::fromUtf8(e.what());
    }
    const bool resolved = mode->currentData() == "execution";
    modes->sync();
    QStringList errors = report.errors;
    QString html;
    auto append = [&](const QString &label, const QString &message, int tab, const QColor &color) {
        html +=
            QString("<p><b style=\"color:%1\">%2</b> %3</p>").arg(color.name(), label, tabLink(tab, message));
    };
    const ChromeColors c(palette());
    for (const auto &error : report.errors)
        append("エラー:", error,
               advanced.errors.contains(error)  ? 3
               : geometryErrors.contains(error) ? 1
                                                : 2,
               c.bad);
    QString text;
    try {
        if (resolved) {
            const auto base =
                window->path.isEmpty() ? QDir::currentPath() : QFileInfo(window->path).absolutePath();
            text = executionPreviewPlan(d, base).input;
        } else if (errors.isEmpty())
            text = nwchemInput(d);
    } catch (const std::exception &e) {
        for (const auto &error : QString::fromUtf8(e.what()).split('\n'))
            if (!errors.contains(error)) {
                errors << error;
                append("エラー:", error, 4, c.bad);
            }
    }
    for (const auto &warning : report.warnings) {
        // The line a check is about, when the input names it ("frequencies numerical").
        QString where;
        if (warning.contains("数値Hessian"))
            for (const auto &line : text.split('\n'))
                if (line.contains("frequencies numerical"))
                    where = QString("（%1 行目）").arg(text.left(text.indexOf(line)).count('\n') + 1);
        append("確認:", warning + where, advanced.warnings.contains(warning) ? 3 : 2, c.muted);
    }
    if (errors.isEmpty())
        html.prepend(QString("<p style=\"color:%1\">分子・計算条件・詳細設定の検証を通過しました。</p>")
                         .arg(c.muted.name()));
    diagnostics->document()->setDefaultStyleSheet(
        QString("a { color: %1; text-decoration: none; }").arg(c.accentText.name()));
    diagnostics->setHtml(html);
    // As tall as its messages, up to a limit.
    diagnostics->document()->setTextWidth(diagnostics->width() > 60 ? diagnostics->viewport()->width() : 280);
    diagnostics->setFixedHeight(std::min(300, int(diagnostics->document()->size().height()) + 4));
    pathsToggle->setText(resolved ? "実行時のパス" : "書き出し後のパス");
    paths->setText(
        (resolved ? QString("入力movecsは参照基準から絶対パスへ変換します。"
                            "相対出力先・scratch・permanentは新しいジョブフォルダーを基準にします。"
                            "この画面を開いてもフォルダーは作成しません。")
                  : QString("相対パスは変更せず出力します。外部で実行するときは、"
                            "NWChemの作業ディレクトリを基準に必要なファイル・保存先を用意してください。")) +
        "<br><br>実行ファイル・MPI・基底ライブラリ・参照ファイルの存在と書込み権限は " +
        QString("<a href=\"tab:4\" style=\"color:%1\">5 実行設定</a>").arg(c.accentText.name()) +
        " で確認してください。ここではNWChemを起動しません。");
    const QString method = s["method"].toString();
    QString task = s["task"].toString(), solvent;
    for (const auto &choice : nwchemChoices("task"))
        if (choice.value == task) {
            task = choice.label;
            break;
        }
    if (s["solvent"] != "none") {
        for (const auto &choice : nwchemChoices("solvent"))
            if (choice.value == s["solvent"].toString())
                solvent = " · " + choice.label.section("（", 0, 0);
        for (const auto &choice : nwchemChoices("solvent_name"))
            if (choice.value == s["solvent_name"].toString() && s["solvent_name"] != "dielectric")
                solvent += "（" + choice.label.section("（", 0, 0) + "）";
    }
    const QString referenceName = method == "dft" && report.reference != "rohf"
                                      ? (report.reference == "rhf" ? "RKS" : "UKS")
                                      : report.reference.toUpper();
    summary->setText((method == "dft" ? nwchemFunctionalLabel(s) : method.toUpper()) + " / " +
                     nwchemBasisLabel(s));
    summaryTask->setText(task + solvent);
    summaryFormula->setText(d.atoms.isEmpty() ? QString("原子なし")
                                              : QString("%1 · %2 原子").arg(d.formula()).arg(d.atoms.size()));
    summaryElectrons->setText(
        QString("%1（α %2 / β %3）").arg(report.electrons).arg(report.alpha).arg(report.beta));
    summaryCharge->setText(QString("%1 · %2%3")
                               .arg(d.job()["charge"].toInt())
                               .arg(d.job()["multiplicity"].toInt())
                               .arg(referenceName.isEmpty() ? QString() : "（" + referenceName + "）"));
    summaryMemory->setText(QString("%1 MB / MPIプロセス").arg(s["memory_mb"].toInt()));
    const bool pending = window->hasCandidate() || window->worker;
    hint->setText(window->hasCandidate()
                      ? "未確定の分子候補を表示しています。確定または取消後にコピー・保存できます。"
                  : window->worker ? "分子の整形中です。終了後にコピー・保存できます。"
                                   : QString());
    banner->setVisible(pending);
    if (pending)
        status->setText("候補・整形の完了待ち（出力不可）");
    else if (!errors.isEmpty())
        status->setText(QString("入力を生成できません（エラー %1 件）").arg(errors.size()));
    else
        status->setText(report.warnings.isEmpty()
                            ? QString("✓ 入力を生成できます")
                            : QString("✓ 入力を生成できます（確認 %1 件）").arg(report.warnings.size()));
    const bool ok = !pending && errors.isEmpty();
    status->setProperty("good", ok);
    status->setProperty("bad", !ok);
    checkCard->setProperty("state", ok ? "ok" : "error");
    for (QWidget *w : {static_cast<QWidget *>(status), static_cast<QWidget *>(checkCard)}) {
        w->style()->unpolish(w);
        w->style()->polish(w);
    }
    contents = errors.isEmpty() ? text : QString();
    stats->setText(contents.isEmpty() ? QString()
                                      : QString("%1 行 · UTF-8 · %2 バイト")
                                            .arg(contents.count('\n'))
                                            .arg(QLocale().toString(contents.toUtf8().size())));
    if (input->toPlainText() != contents) {
        const int scroll = input->verticalScrollBar()->value();
        // As numbers: a cursor kept across setPlainText would follow the inserted text to its end.
        const int anchor = input->textCursor().anchor(), at = input->textCursor().position();
        // Marks first: the text change repaints the gutter.
        outline = nwchemInputOutline(d, contents);
        input->advanced = outline.advanced;
        input->extra = outline.extra;
        input->setPlainText(contents);
        QTextCursor restored(input->document());
        restored.setPosition(std::min(anchor, int(contents.size())));
        restored.setPosition(std::min(at, int(contents.size())), QTextCursor::KeepAnchor);
        input->setTextCursor(restored);
        input->verticalScrollBar()->setValue(scroll);
        searchStatus->clear();
        rebuildOutline();
    }
    followCursor();
    copy->setEnabled(!pending && !contents.isEmpty());
    save->setEnabled(copy->isEnabled());
    setBadge(!d.atoms.isEmpty() && !errors.isEmpty(), status->text());
}
void InputPreviewPanel::refreshBadge() {
    const auto &d = window->displayDocument();
    const auto errors = validateNwchem(d).errors;
    setBadge(!d.atoms.isEmpty() && !errors.isEmpty(),
             errors.isEmpty() ? QString("✓ 入力を生成できます")
                              : QString("入力を生成できません（エラー %1 件）").arg(errors.size()));
}
void InputPreviewPanel::setBadge(bool failed, const QString &tip) {
    const int index = window->workspaceTabs->indexOf(this);
    if (index >= 0) {
        window->workspaceTabs->tabBar()->setTabTextColor(index, failed ? errorColor(palette()) : QColor());
        window->workspaceTabs->setTabToolTip(index, tip);
        window->syncWorkflowSteps();
    }
}
QString InputPreviewPanel::prepareExport() {
    window->execution->commitPendingEdits();
    if (auto *focused = QApplication::focusWidget())
        focused->clearFocus();
    if (window->hasCandidate() || window->worker)
        fail("分子候補の確定・取消、または整形の終了後に出力してください。");
    refresh();
    if (contents.isEmpty())
        fail("入力を生成できません。検証結果のエラーを修正してください。");
    return contents;
}
void InputPreviewPanel::saveInput(const QString &path) {
    const auto text = prepareExport();
    writeFile(path, text.toUtf8());
}
void InputPreviewPanel::exportInput(bool toFile) {
    window->safe([&] {
        const auto text = prepareExport();
        if (toFile) {
            auto path = QFileDialog::getSaveFileName(
                this, "NWChem入力を保存", nwchemSettings(window->document())["start"].toString() + ".nw",
                "NWChem (*.nw)");
            if (path.isEmpty())
                return;
            if (!path.endsWith(".nw", Qt::CaseInsensitive))
                path += ".nw";
            writeFile(path, text.toUtf8());
        } else
            QApplication::clipboard()->setText(text);
        window->statusBar()->showMessage(
            toFile ? "NWChem入力を保存しました。" : "NWChem入力をコピーしました。", 5000);
    });
}
void InputPreviewPanel::findText(bool backwards) {
    if (search->text().isEmpty())
        return;
    const auto original = input->textCursor();
    const auto flags = backwards ? QTextDocument::FindBackward : QTextDocument::FindFlags{};
    bool found = input->find(search->text(), flags);
    if (!found) {
        input->moveCursor(backwards ? QTextCursor::End : QTextCursor::Start);
        found = input->find(search->text(), flags);
    }
    if (!found)
        input->setTextCursor(original);
    searchStatus->setText(found ? "一致する文字列を選択しました。" : "一致する文字列はありません。");
}
void InputPreviewPanel::applyChromeStyle() {
    const auto pal = static_cast<QWidget *>(window)->palette();
    const auto advanced = advancedMark(pal).name(), extra = extraMark(pal).name();
    setStyleSheet(chromeRules(settingsRules() + R"(
        #inputPreviewTab { background: @window; }
        #inputPreviewOutlineColumn { background: @base; border: none; border-right: 1px solid @line; }
        QListWidget#inputPreviewOutline::item { border-radius: 6px; }
        QListWidget#inputPreviewOutline::item:hover { background: @hover; }
        QListWidget#inputPreviewOutline::item:selected { background: @tint; }
        QListWidget#inputPreviewOutline QLabel[blockTitle="true"] { font-family: monospace; color: @text; }
        QListWidget#inputPreviewOutline QToolButton { border: 1px solid @line; border-radius: 11px; background: @base;
                                                      color: @text; font-size: 11px; font-weight: bold; }
        QListWidget#inputPreviewOutline QToolButton:hover { background: @hover; }
        QListWidget#inputPreviewOutline QToolButton[source="advanced"] { border-color: ADVANCED; color: ADVANCED; }
        QListWidget#inputPreviewOutline QToolButton[source="extra"] { border-color: EXTRA; color: EXTRA; }
        #inputPreviewCode { background: @base; border: 1px solid @line; border-radius: 10px; }
        #inputPreviewFooter { background: @window; border: none; border-top: 1px solid @line;
                              border-bottom-left-radius: 10px; border-bottom-right-radius: 10px; }
        #inputPreviewFooter QLabel { color: @muted; }
        #inputPreviewSource { background: @base; border: 1px solid @line; border-radius: 8px; }
        QLabel#inputPreviewSourceTitle { color: @muted; }
        QTextBrowser#inputPreviewDiagnostics { background: transparent; }
        #inputPreviewRail { background: @window; }
        QLabel#inputPreviewLegendAdvanced { background: ADVANCED; border-radius: 1px; }
        QLabel#inputPreviewLegendExtra { background: EXTRA; border-radius: 1px; }
    )",
                              pal)
                      .replace("ADVANCED", advanced)
                      .replace("EXTRA", extra));
}
} // namespace mf
