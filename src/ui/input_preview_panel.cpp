// SPDX-License-Identifier: Apache-2.0
// Native NWChem counterpart of the read-only preview workflow in KAGAMI app.py.
#include "ui/input_preview_panel.h"
#include "editing/coordinates.h"
#include "nwchem/nwchem_advanced.h"
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
            if (token.startsWith('#'))
                format.setForeground(accentColor(palette, "#718096", "#a0aec0"));
            else if (token.startsWith('"'))
                format.setForeground(accentColor(palette, "#16836a", "#5fd3b4"));
            else if (token.front().isLetter()) {
                format.setForeground(accentColor(palette, "#487fc4", "#7fb0ff"));
                format.setFontWeight(QFont::Bold);
            } else
                format.setForeground(accentColor(palette, "#ad6bba", "#d9a3e6"));
            setFormat(match.capturedStart(), match.capturedLength(), format);
        }
    }
};
// The gutter is a widget, so copied/saved input never contains line numbers.
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
        return 16 + fontMetrics().horizontalAdvance('9') * QString::number(blockCount()).size();
    }
    void paintNumbers(QPaintEvent *event) {
        QPainter painter(gutter);
        painter.fillRect(event->rect(), palette().alternateBase());
        painter.setPen(palette().color(QPalette::Text));
        auto block = firstVisibleBlock();
        while (block.isValid()) {
            const auto top = blockBoundingGeometry(block).translated(contentOffset()).top();
            if (top > event->rect().bottom())
                break;
            if (block.isVisible())
                painter.drawText(0, qRound(top), gutter->width() - 6, fontMetrics().height(), Qt::AlignRight,
                                 QString::number(block.blockNumber() + 1));
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
    InputEditor() : gutter(new Gutter(this)) {
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { updateGutter(); });
        connect(this, &QPlainTextEdit::updateRequest, gutter, [this] { gutter->update(); });
        new InputHighlighter(document());
        updateGutter();
    }
};
QString tabLink(int index, const QString &text) {
    return QString("<a href=\"tab:%1\">%2</a>").arg(index).arg(text.toHtmlEscaped());
}
} // namespace
InputPreviewPanel::InputPreviewPanel(MainWindow *owner) : QWidget(owner), window(owner) {
    setObjectName("inputPreviewTab");
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    mode = new QComboBox;
    mode->setObjectName("inputPreviewMode");
    mode->addItem("書出し用入力（設定どおりのパス）", "input");
    mode->addItem("実行時の入力（入力パスを解決）", "execution");
    toolbar->addWidget(mode);
    auto *reload = new QPushButton("再検証");
    reload->setObjectName("inputPreviewRefresh");
    toolbar->addWidget(reload);
    toolbar->addStretch();
    copy = new QPushButton("全文をコピー");
    save = new QPushButton("入力を保存…");
    copy->setObjectName("inputPreviewCopy");
    save->setObjectName("inputPreviewSave");
    toolbar->addWidget(copy);
    toolbar->addWidget(save);
    hint = new QLabel;
    hint->setWordWrap(true);
    hint->setTextFormat(Qt::PlainText);
    hint->setObjectName("inputPreviewHint");
    layout->addWidget(hint);
    auto *split = new QSplitter;
    layout->addWidget(split, 1);
    auto *left = new QWidget;
    auto *textLayout = new QVBoxLayout(left);
    textLayout->setContentsMargins(0, 0, 0, 0);
    auto *find = new QHBoxLayout;
    textLayout->addLayout(find);
    sections = new QComboBox;
    sections->setObjectName("inputPreviewSections");
    sections->setMinimumContentsLength(16);
    sections->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    find->addWidget(sections);
    search = new QLineEdit;
    search->setObjectName("inputPreviewSearch");
    search->setPlaceholderText("入力内を検索（Ctrl+F）");
    search->setClearButtonEnabled(true);
    find->addWidget(search, 1);
    for (bool backwards : {true, false}) {
        auto *button = new QPushButton(backwards ? "前へ" : "次へ");
        button->setObjectName(backwards ? "inputPreviewPrevious" : "inputPreviewNext");
        find->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, backwards] { findText(backwards); });
    }
    input = new InputEditor;
    input->setObjectName("inputPreviewText");
    input->setPlaceholderText("分子と計算条件を設定すると、NWChem入力を自動表示します。");
    textLayout->addWidget(input, 1);
    auto *footer = new QHBoxLayout;
    textLayout->addLayout(footer);
    searchStatus = new QLabel;
    searchStatus->setObjectName("inputPreviewSearchStatus");
    footer->addWidget(searchStatus, 1);
    position = new QLabel;
    position->setObjectName("inputPreviewPosition");
    footer->addWidget(position);
    split->addWidget(left);
    auto *right = new QWidget;
    auto *checks = new QVBoxLayout(right);
    checks->setContentsMargins(4, 0, 0, 0);
    summary = new QLabel;
    summary->setObjectName("inputPreviewSummary");
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    checks->addWidget(summary);
    status = new QLabel;
    status->setObjectName("inputPreviewStatus");
    status->setWordWrap(true);
    checks->addWidget(status);
    diagnostics = new QTextBrowser;
    diagnostics->setObjectName("inputPreviewDiagnostics");
    diagnostics->setOpenLinks(false);
    diagnostics->setOpenExternalLinks(false);
    checks->addWidget(diagnostics, 1);
    for (const int tab : {1, 2, 3, 4}) {
        const QStringList labels{"", "2 座標", "3 計算条件", "4 詳細設定", "5 実行設定"};
        auto *button = new QPushButton(labels[tab] + "へ");
        button->setObjectName("inputPreviewGo" + QString::number(tab + 1));
        checks->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, tab] { navigate(tab); });
    }
    split->addWidget(right);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setSizes({850, 350});
    connect(mode, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    connect(copy, &QPushButton::clicked, this, [this] { exportInput(false); });
    connect(save, &QPushButton::clicked, this, [this] { exportInput(true); });
    connect(diagnostics, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        bool ok = false;
        const int tab = url.path().toInt(&ok);
        if (url.scheme() == "tab" && ok)
            navigate(tab);
    });
    connect(sections, &QComboBox::activated, this, [this] {
        const int line = sections->currentData().toInt();
        QTextCursor cursor(input->document()->findBlockByNumber(line));
        input->setTextCursor(cursor);
        input->centerCursor();
        input->setFocus();
    });
    connect(search, &QLineEdit::returnPressed, this, [this] { findText(false); });
    connect(search, &QLineEdit::textChanged, searchStatus, &QLabel::clear);
    connect(input, &QPlainTextEdit::cursorPositionChanged, this, [this] {
        const auto cursor = input->textCursor();
        position->setText(
            QString("行 %1 / 列 %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
    });
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(findShortcut, &QShortcut::activated, this, [this] {
        search->setFocus();
        search->selectAll();
    });
}
void InputPreviewPanel::navigate(int tab) {
    if (tab >= 0 && tab < window->workspaceTabs->count())
        window->workspaceTabs->setCurrentIndex(tab);
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
    QStringList errors = report.errors;
    QString html = "<h3>入力の検証</h3>";
    auto append = [&](const QString &label, const QString &message, int tab) {
        html += "<p><b>" + label + "</b> " + tabLink(tab, message) + "</p>";
    };
    for (const auto &error : report.errors)
        append("エラー:", error,
               advanced.errors.contains(error)  ? 3
               : geometryErrors.contains(error) ? 1
                                                : 2);
    for (const auto &warning : report.warnings)
        append("確認:", warning, advanced.warnings.contains(warning) ? 3 : 2);
    if (report.errors.isEmpty())
        html += "<p>分子・計算条件・詳細設定の検証を通過しました。</p>";
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
                append("エラー:", error, 4);
            }
    }
    if (resolved)
        html += "<h3>実行時のパス</h3><p>入力movecsは参照基準から絶対パスへ変換します。"
                "相対出力先・scratch・permanentは新しいジョブフォルダーを基準にします。"
                "この画面を開いてもフォルダーは作成しません。</p>";
    else
        html += "<h3>書出し後のパス</h3><p>相対パスは変更せず出力します。外部で実行するときは、"
                "NWChemの作業ディレクトリを基準に必要なファイル・保存先を用意してください。</p>";
    html += "<p>実行ファイル・MPI・基底ライブラリ・参照ファイルの存在と書込み権限は " +
            tabLink(4, "5 実行設定") + " で確認してください。ここではNWChemを起動しません。</p>";
    diagnostics->setHtml(html);
    const QString method = s["method"].toString();
    QString task = s["task"].toString();
    for (const auto &choice : nwchemChoices("task"))
        if (choice.value == task) {
            task = choice.label;
            break;
        }
    summary->setText(
        QString("%1 · %2 原子\n%3 / %4\n%5\n全電荷 %6 · 多重度 %7\n電子数 %8（α %9 / β "
                "%10）\nメモリ %11 MB / MPIプロセス")
            .arg(d.formula())
            .arg(d.atoms.size())
            .arg(method == "dft" ? nwchemFunctionalLabel(s) : method.toUpper(), nwchemBasisLabel(s), task)
            .arg(d.job()["charge"].toInt())
            .arg(d.job()["multiplicity"].toInt())
            .arg(report.electrons)
            .arg(report.alpha)
            .arg(report.beta)
            .arg(s["memory_mb"].toInt()));
    const bool pending = window->hasCandidate() || window->worker;
    hint->setText(window->hasCandidate()
                      ? "未確定の分子候補を表示しています。確定または取消後にコピー・保存できます。"
                  : window->worker ? "分子の整形中です。終了後にコピー・保存できます。"
                                   : "読み取り専用です。変更は「2 座標」「3 計算条件」「4 詳細設定」「5 "
                                     "実行設定」で行います。表示は現在の編集内容に追従します。");
    if (pending)
        status->setText("候補・整形の完了待ち（出力不可）");
    else if (!errors.isEmpty())
        status->setText(QString("入力を生成できません（エラー %1 件）").arg(errors.size()));
    else
        status->setText(QString("✓ 入力を生成できます\n%1 行 · UTF-8 %2 バイト · 確認 %3 件")
                            .arg(text.count('\n'))
                            .arg(text.toUtf8().size())
                            .arg(report.warnings.size()));
    contents = errors.isEmpty() ? text : QString();
    if (input->toPlainText() != contents) {
        const int scroll = input->verticalScrollBar()->value();
        const auto cursor = input->textCursor();
        input->setPlainText(contents);
        QTextCursor restored(input->document());
        restored.setPosition(std::min(cursor.anchor(), int(contents.size())));
        restored.setPosition(std::min(cursor.position(), int(contents.size())), QTextCursor::KeepAnchor);
        input->setTextCursor(restored);
        input->verticalScrollBar()->setValue(scroll);
        searchStatus->clear();
        const QSignalBlocker blocker(sections);
        sections->clear();
        const auto lines = contents.split('\n');
        for (int i = 0; i < lines.size(); ++i)
            if (!lines[i].isEmpty() && !lines[i][0].isSpace() && !lines[i].startsWith('#') &&
                lines[i] != "end")
                sections->addItem(QString("%1: %2").arg(i + 1).arg(lines[i]), i);
    }
    copy->setEnabled(!pending && !contents.isEmpty());
    save->setEnabled(copy->isEnabled());
    const int index = window->workspaceTabs->indexOf(this);
    if (index >= 0) {
        window->workspaceTabs->tabBar()->setTabTextColor(
            index, !d.atoms.isEmpty() && !errors.isEmpty() ? errorColor(palette()) : QColor());
        window->workspaceTabs->setTabToolTip(index, status->text());
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
} // namespace mf
