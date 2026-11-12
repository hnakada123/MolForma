// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/preferences_dialog.h"
#include "editing/xtb.h"
#include "foundation/error.h"
#include "foundation/i18n.h"
#include "model/settings.h"
#include "nwchem/execution_environment.h"
#include "nwchem/nwchem_execution.h"
#include "ui/chrome_layouts.h"
#include "ui/editor_context.h"
#include "ui/execution_form.h"
#include "ui/remote_hosts_dialog.h"
#include "ui/settings_widgets.h"
#include "ui/theme.h"
#include <QtWidgets>

namespace mf {
void preferencesDialog(QWidget *parent, UiServices &services,
                       const std::function<QJsonObject()> &currentExecution) {
    QDialog dialog(parent);
    dialog.setObjectName("preferencesDialog");
    dialog.setProperty("mcpProtected", true); // Same protection as the executable fields in "3 実行設定".
    dialog.setProperty("canvas", true);
    dialog.setWindowTitle(tr("環境設定"));
    dialog.resize(640, 700);
    // The cards and controls of "3 実行設定", for the settings new documents start with.
    dialog.setStyleSheet(
        chromeRules(settingsRules() + "QDialog { background: @window; }", parent->palette()));
    auto *outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 14);
    outer->setSpacing(12);
    // The cards scroll; the buttons stay below them.
    auto *scroll = new QScrollArea;
    scroll->setObjectName("preferencesScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setProperty("canvas", true);
    auto *page = new QWidget;
    page->setProperty("canvas", true);
    scroll->setWidget(page);
    outer->addWidget(scroll, 1);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(16, 14, 16, 0);
    layout->setSpacing(12);
    auto *intro =
        new QLabel(tr("新規作成、およびプロジェクト以外の構造ファイル（XYZ・SDF・PDB等）を開いたときの"
                      "実行設定です。開いているプロジェクトの設定は「3 実行設定」で変更します。"));
    intro->setWordWrap(true);
    intro->setProperty("muted", true);
    layout->addWidget(intro);
    // The cards of "3 実行設定" for the machine-wide settings; the dialog keeps what they show.
    auto chosen = executionPreferences();
    auto *form = new ExecutionForm(ExecutionForm::Scope::Preferences, "preferences");
    form->setSettings(chosen);
    QObject::connect(form, &ExecutionForm::changed, &dialog,
                     [&chosen, form](const QString &key, const QJsonValue &value) {
                         chosen[key] = value;
                         form->setSettings(chosen);
                     });
    QObject::connect(form, &ExecutionForm::remoteHostsRequested, &dialog, [&] {
        RemoteHostsDialog hosts(chosen["executable"].toString(), chosen["launcher"].toString(), &dialog);
        hosts.select(chosen["remote"].toString());
        hosts.exec();
        form->setSettings(chosen);
    });
    layout->addWidget(form);
    const auto card = [&](const QString &title) {
        auto *c = new SettingsCard(title);
        auto *rows = new FormLayout;
        rows->setHorizontalSpacing(12);
        rows->setVerticalSpacing(8);
        c->body()->addLayout(rows);
        layout->addWidget(c);
        return rows;
    };
    auto *script = card(tr("ジョブスクリプト（別の計算機で実行）"));
    auto *prelude = new QPlainTextEdit(executionJobPrelude());
    prelude->setObjectName("preferences_job_prelude");
    prelude->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    prelude->setLineWrapMode(QPlainTextEdit::NoWrap);
    prelude->setFixedHeight(prelude->fontMetrics().lineSpacing() * 4 + 16);
    prelude->setPlaceholderText(tr("例: module load nwchem/7.3.1"));
    prelude->setToolTip(
        tr("job.sh でNWChemを起動する前に実行する行です（モジュールの読込みなど）。プロジェクトには保存"
           "せず、このPCで作るすべてのジョブスクリプトに加えます。"));
    script->addRow(tr("準備の行"), prelude);
    // The headers of Slurm and PBS scripts, for saved jobs and remote destinations alike.
    QMap<QString, QPlainTextEdit *> headers;
    for (const auto &kind : {QString("slurm"), QString("pbs")}) {
        auto *text = new QPlainTextEdit(jobHeaderTemplate(kind));
        text->setObjectName("preferences_job_header_" + kind);
        text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setTabChangesFocus(true);
        text->setFixedHeight(text->fontMetrics().lineSpacing() * 6 + 16);
        headers[kind] = text;
        script->addRow(kind == "slurm" ? tr("Slurmの見出し") : tr("PBSの見出し"), text);
    }
    auto *placeholders = new QLabel(tr("見出しの行は # で始めます。プレースホルダー: "
                                       "%1。値のないプレースホルダー（制限時間なしの {time}、"
                                       "キューなしの {queue}）を含む行は書きません。")
                                        .arg("{" + jobHeaderPlaceholders().join("} {") + "}"));
    placeholders->setProperty("caption", true);
    placeholders->setWordWrap(true);
    placeholders->setTextFormat(Qt::PlainText);
    auto *resetHeaders = new QPushButton(tr("見出しを既定に戻す"));
    resetHeaders->setObjectName("preferencesResetHeaders");
    QObject::connect(resetHeaders, &QPushButton::clicked, &dialog, [headers] {
        for (auto it = headers.begin(); it != headers.end(); ++it)
            it.value()->setPlainText(defaultJobHeader(it.key()));
    });
    auto *headerHelp = new QVBoxLayout;
    headerHelp->addWidget(placeholders);
    headerHelp->addWidget(resetHeaders, 0, Qt::AlignLeft);
    script->addRow(QString(), headerHelp);
    auto *programs = card(tr("外部プログラム"));
    auto *xtbProgram = new QLineEdit(services.preference(Preference::xtbExecutable).toString());
    xtbProgram->setObjectName("preferences_xtb");
    xtbProgram->setPlaceholderText("xtb");
    xtbProgram->setToolTip(
        tr("「ツール → xTBで構造を最適化…」で起動するxtbです。名前（PATHから検索）またはパスで"
           "指定します。空欄ではPATHのxtbを使います。"));
    programs->addRow(tr("xTB実行ファイル"), xtbProgram);
    auto *xtbFound = new QLabel;
    xtbFound->setProperty("muted", true);
    xtbFound->setWordWrap(true);
    xtbFound->setTextFormat(Qt::PlainText);
    const auto showXtb = [xtbProgram, xtbFound] {
        const auto found = xtbExecutable(xtbProgram->text());
        xtbFound->setText(found.isEmpty() ? tr("見つかりません") : found);
    };
    QObject::connect(xtbProgram, &QLineEdit::textChanged, &dialog, showXtb);
    showXtb();
    programs->addRow(QString(), xtbFound);
    auto *startup = card(tr("起動・表示"));
    auto *water = new QCheckBox(tr("起動時に水分子を読み込む"));
    water->setObjectName("preferences_startupWater");
    water->setChecked(services.preference(Preference::startupWater, false).toBool());
    startup->addRow(tr("新しいウィンドウ"), water);
    auto *renderer = new QComboBox;
    renderer->addItem(tr("自動（VTKを優先）"), "auto");
    renderer->addItem(tr("QPainter（CPU）"), "painter");
    renderer->setCurrentIndex(services.preference(Preference::viewRenderer).toString() == "painter" ? 1 : 0);
    startup->addRow(tr("3D描画（再起動後に反映）"), renderer);
    // Named in both languages, so that either reader finds it.
    auto *language = new QComboBox;
    language->setObjectName("preferences_language");
    language->addItem(tr("システムに合わせる"), "");
    language->addItem("日本語", "ja"); // i18n-exempt
    language->addItem("English", "en");
    language->setCurrentIndex(
        std::max(0, language->findData(services.preference(Preference::language).toString())));
    startup->addRow(tr("表示言語 / Language（再起動後に反映）"), language);
    layout->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setProperty("primary", true);
    auto *current = buttons->addButton(tr("現在の実行設定を読み込む"), QDialogButtonBox::ResetRole);
    buttons->setContentsMargins(16, 0, 16, 0);
    outer->addWidget(buttons);
    QObject::connect(current, &QPushButton::clicked, &dialog, [&] {
        const auto s = currentExecution();
        for (const auto &key : executionPreferenceKeys())
            chosen[key] = s[key];
        form->setSettings(chosen);
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        services.safe([&] {
            form->commitEnvironment();
            // Rows left empty are dropped.
            QJsonArray rows;
            for (const auto &row : chosen["environment"].toArray())
                if (const auto entry = row.toObject();
                    !entry["name"].toString().isEmpty() || !entry["value"].toString().isEmpty())
                    rows.append(entry);
            chosen["environment"] = rows;
            // Check the values with the rules of "3 実行設定", leaving out the molecule's own.
            Document probe;
            for (auto it = chosen.begin(); it != chosen.end(); ++it)
                setExecutionSetting(probe, it.key(), it.value());
            QStringList errors;
            for (const auto &diagnostic : validateExecution(probe, QDir::homePath(), false).diagnostics)
                if (diagnostic.severity == DiagnosticSeverity::Error &&
                    diagnostic.target == SettingTarget::Execution)
                    errors << diagnostic.message;
            for (auto it = headers.begin(); it != headers.end(); ++it)
                for (const auto &error : jobHeaderErrors(it.value()->toPlainText()))
                    errors
                        << (it.key() == "slurm" ? tr("Slurmの見出し: %1") : tr("PBSの見出し: %1")).arg(error);
            if (!errors.isEmpty())
                fail(errors.join('\n'));
            saveExecutionJobPrelude(prelude->toPlainText());
            for (auto it = headers.begin(); it != headers.end(); ++it)
                saveJobHeaderTemplate(it.key(), it.value()->toPlainText());
            services.setPreference(Preference::xtbExecutable, xtbProgram->text().trimmed());
            saveExecutionPreferences(chosen);
            services.setPreference(Preference::startupWater, water->isChecked());
            services.setPreference(Preference::viewRenderer, renderer->currentData().toString());
            services.setPreference(Preference::language, language->currentData().toString());
            dialog.accept();
        });
    });
    dialog.exec();
}
} // namespace mf
