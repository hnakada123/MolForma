// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/help_dialogs.h"
#include "foundation/i18n.h"
#include "ui/editor_context.h"
#include <QtWidgets>
// Embedded manual and icon (docs/manual.qrc in this static library); outside namespace mf.
static void initResources() {
    Q_INIT_RESOURCE(manual);
}
namespace mf {
void initManualResources() {
    initResources();
}
void shortcutsDialog(QWidget *parent, const QMenuBar *menus, const QAction *undoAction,
                     const QAction *redoAction) {
    QDialog dialog(parent);
    dialog.setObjectName("shortcutsDialog");
    dialog.setWindowTitle(tr("キーボードショートカット"));
    dialog.resize(560, 600);
    auto *layout = new QVBoxLayout(&dialog);
    auto *table = new QTableWidget(0, 3);
    table->setObjectName("shortcutsTable");
    table->setHorizontalHeaderLabels({tr("キー"), tr("操作"), tr("有効な場所")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    QList<QAction *> actions;
    std::function<void(QMenu *)> collect = [&](QMenu *menu) {
        for (auto *a : menu->actions()) {
            if (a->menu())
                collect(a->menu());
            else if (!a->shortcut().isEmpty() && !actions.contains(a))
                actions << a;
        }
    };
    for (auto *a : menus->actions())
        if (a->menu())
            collect(a->menu());
    const auto row = [&](const QString &key, const QString &text, const QString &where) {
        const int r = table->rowCount();
        table->insertRow(r);
        table->setItem(r, 0, new QTableWidgetItem(key));
        table->setItem(r, 1, new QTableWidgetItem(text));
        table->setItem(r, 2, new QTableWidgetItem(where));
    };
    for (auto *a : actions) {
        QStringList keys;
        for (const auto &k : a->shortcuts())
            keys << k.toString(QKeySequence::NativeText);
        // Undo/Redo name the next operation in the menu; the list names the command itself.
        const auto text = a == undoAction   ? tr("元に戻す（候補があれば取消）")
                          : a == redoAction ? tr("やり直す")
                                            : QString(a->text()).remove('&');
        row(keys.join(" / "), text,
            a->shortcutContext() == Qt::WidgetWithChildrenShortcut ? tr("「1 分子」") : tr("全体", "where"));
    }
    for (const auto &[key, text] : {std::pair{tr("左ドラッグ"), tr("視点を回転（画面の縦・横軸まわり）")},
                                    {tr("右ドラッグ"), tr("視点を平行移動")},
                                    {tr("移動ツールで原子をドラッグ"), tr("原子の座標を動かす")},
                                    {tr("ホイール"), tr("カーソル位置を中心に拡大・縮小")},
                                    {tr("クリック"), tr("原子を選択（選択単位に従う）")},
                                    {tr("Shift／Ctrl＋クリック"), tr("選択へ追加・解除（順番を保持）")},
                                    {tr("Shift／Ctrlドラッグ"), tr("範囲選択（Ctrlは追加）")},
                                    {"Ctrl+Tab", tr("次のタブ（タブ内にフォーカスがあるとき）")}})
        row(key, text, tr("3D画面・タブ"));
    table->resizeColumnsToContents();
    layout->addWidget(table, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
void manualDialog(QWidget *parent, UiServices &services,
                  const std::function<bool(const QString &resource)> &useSample) {
    initManualResources();
    QDialog dialog(parent);
    dialog.setObjectName("manualDialog");
    dialog.setWindowTitle(tr("MolForma 操作ガイド・マニュアル"));
    dialog.resize(900, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *tips = new QLabel(
        tr("左のパレットで部品・原子を選び、3D画面をクリックするかドロップして配置します。"
           "左端の列でツールを切り替え、3D画面の上にツールの設定、下にクリックしたときの動作を表示します。"
           "原子をクリックで選択、Shift／Ctrl＋クリックで順に追加し、2〜4原子で距離・角度・二面角を"
           "測定できます。配位子・削除・整形などは「変更を確定」（Ctrl+Enter）で適用し、Escで"
           "候補を取消、Ctrl+Zで確定済みの変更を戻します。計算は「計算 → 計算を実行」（F5）で"
           "始めます。キー操作の一覧は「ヘルプ → キーボードショートカット一覧」にあります。"));
    tips->setWordWrap(true);
    layout->addWidget(tips);
    auto *bar = new QHBoxLayout;
    layout->addLayout(bar);
    auto *pages = new QComboBox;
    pages->setObjectName("manualPage");
    // The English manual beside the Japanese one.
    const QString docs = english() ? ":/docs/en/" : ":/docs/";
    pages->addItem(tr("機能と操作（はじめに・各タブの説明）"), docs + "features.md");
    pages->addItem(tr("題材から選ぶ実習（分子を組んで計算するまで）"), docs + "recipes.md");
    pages->addItem(tr("MCPサーバー（AIクライアントからの操作）"), docs + "mcp.md");
    auto *back = new QPushButton(tr("戻る"));
    back->setObjectName("manualBack");
    back->setEnabled(false);
    bar->addWidget(back);
    bar->addWidget(pages);
    auto *find = new QLineEdit;
    find->setObjectName("manualSearch");
    find->setPlaceholderText(tr("検索（Enterで次へ）"));
    find->setClearButtonEnabled(true);
    bar->addWidget(find, 1);
    auto *browser = new QTextBrowser;
    browser->setObjectName("manualText");
    layout->addWidget(browser, 1);
    // Links are followed here: the practice files beside the recipes are used rather than shown.
    browser->setOpenLinks(false);
    QObject::connect(browser, &QTextBrowser::anchorClicked, &dialog,
                     [&services, &useSample, browser, &dialog](const QUrl &url) {
                         if (url.scheme() != "qrc")
                             QDesktopServices::openUrl(url);
                         else if (!url.path().startsWith("/docs/recipes/samples/"))
                             browser->setSource(url);
                         else
                             services.safe([&] {
                                 if (useSample(":" + url.path()))
                                     dialog.accept();
                             });
                     });
    // Pages are shown by their resource URL, so links between them and their images resolve.
    const auto show = [browser, pages] {
        const auto path = pages->currentData().toString();
        if (QFile::exists(path))
            browser->setSource(QUrl("qrc" + path), QTextDocument::MarkdownResource);
        else
            browser->setPlainText(tr("マニュアルを読み込めません。"));
    };
    QObject::connect(back, &QPushButton::clicked, browser, &QTextBrowser::backward);
    QObject::connect(browser, &QTextBrowser::backwardAvailable, back, &QPushButton::setEnabled);
    // Screenshots in the recipes are wider than the page: they are shown smoothly scaled to its width
    // (the document itself would scale them pixel by pixel).
    const auto fitImages = [browser] {
        auto *document = browser->document();
        const int width = browser->viewport()->width() - 2 * int(document->documentMargin()) - 8;
        const double ratio = browser->devicePixelRatioF();
        QTextCursor cursor(document);
        for (auto block = document->begin(); block.isValid(); block = block.next()) {
            // Figures inside the steps of a list are indented (Markdown continuation paragraphs).
            const auto format = block.blockFormat();
            double indent = format.leftMargin() + format.indent() * document->indentWidth();
            if (auto *list = block.textList())
                indent += list->format().indent() * document->indentWidth();
            const int fit = width - std::max(0, int(indent));
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const auto fragment = it.fragment();
                if (!fragment.charFormat().isImageFormat())
                    continue;
                auto image = fragment.charFormat().toImageFormat();
                const auto url = document->baseUrl().resolved(QUrl(image.name()));
                QImage original(url.scheme() == "qrc" ? ":" + url.path() : url.toLocalFile());
                if (original.isNull() || original.width() <= fit)
                    continue;
                auto fitted = original.scaledToWidth(int(fit * ratio), Qt::SmoothTransformation);
                fitted.setDevicePixelRatio(ratio);
                const QUrl name =
                    document->baseUrl().resolved(QString("fitted/%1/%2").arg(fit).arg(image.name()));
                document->addResource(QTextDocument::ImageResource, name, fitted);
                image.setName(name.toString());
                image.setWidth(fit);
                image.setHeight(original.height() * double(fit) / original.width());
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
                cursor.setCharFormat(image);
            }
        }
    };
    QObject::connect(browser, &QTextBrowser::sourceChanged, &dialog, fitImages);
    show();
    QObject::connect(pages, &QComboBox::currentIndexChanged, &dialog, show);
    QObject::connect(find, &QLineEdit::returnPressed, &dialog, [browser, find] {
        if (!browser->find(find->text())) {
            browser->moveCursor(QTextCursor::Start); // Wrap around.
            browser->find(find->text());
        }
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
} // namespace mf
