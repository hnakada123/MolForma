// SPDX-License-Identifier: GPL-2.0-or-later
#include "editing/chemistry_job.h"
#include "editing/coordinates.h"
#include "editing/operations.h"
#include "model/i18n.h"
#include <QCoreApplication>
#include <QJsonDocument>

namespace mf {
ChemistryJob::ChemistryJob(EditorSession &s, QObject *parent, QString executable)
    : QObject(parent), session(s),
      program(executable.isEmpty() ? QCoreApplication::applicationFilePath() : executable) {}
ChemistryJob::~ChemistryJob() {
    cancel();
}
void ChemistryJob::release() {
    if (auto *p = std::exchange(process, nullptr)) {
        p->disconnect(this);
        p->deleteLater();
    }
    scratch.reset();
    emit changed();
}
bool ChemistryJob::cancel() {
    if (!process)
        return false;
    ++generation;
    process->disconnect(this);
    process->kill();
    process->waitForFinished(2000);
    release();
    return true;
}
void ChemistryJob::start(const ChemistryRequest &request) {
    if (active() || session.candidate())
        fail(tr("処理中または変更候補があります。先に完了・取消してください。"));
    const auto base = session.document();
    if (request.mode == "smiles") {
        auto replacing = base;
        replacing.remove(replacing.ids());
    }
    auto work = std::make_unique<QTemporaryDir>();
    if (!work->isValid())
        fail(tr("一時ディレクトリを作成できません。"));
    const auto input = work->filePath("input.json"), output = work->filePath("output.json");
    writeFile(input, QJsonDocument(QJsonObject{{"mode", request.mode},
                                               {"smiles", request.smiles},
                                               {"moving", idsToJson(request.moving)},
                                               {"count", request.count},
                                               {"document", base.toJson()}})
                         .toJson());
    scratch = std::move(work);
    const auto serial = ++generation;
    const auto revision = session.revision();
    auto *p = process = new QProcess(this);
    p->setProgram(program);
    p->setArguments({"--worker", input, output});
    connect(p, &QProcess::errorOccurred, this, [this, p, request](QProcess::ProcessError error) {
        if (process != p || error != QProcess::FailedToStart)
            return;
        const auto message = p->errorString();
        release();
        emit failed(request, message, true);
    });
    connect(p, &QProcess::finished, this,
            [this, p, request, base, serial, revision, output](int code, QProcess::ExitStatus status) {
                if (process != p)
                    return;
                QString error;
                try {
                    const auto response = parseJson(readFile(output));
                    if (code != 0 || status != QProcess::NormalExit || response.contains("error"))
                        fail(response["error"].toString(tr("整形に失敗しました。")));
                    if (session.revision() != revision)
                        fail(tr("分子が変更されたため整形結果を破棄しました。"));
                    ChemistryResult result{request, base, {}, {}, serial, revision};
                    if (request.mode == "conformers") {
                        for (const auto &v : response["conformers"].toArray()) {
                            const auto c = v.toObject();
                            result.conformers << Conformer{Document::fromJson(c["document"].toObject()),
                                                           c["energy"].toDouble()};
                        }
                        if (result.conformers.isEmpty())
                            fail(tr("配座が見つかりませんでした。"));
                    } else
                        result.document = Document::fromJson(response["document"].toObject());
                    emit ready(result);
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what());
                }
                if (process == p)
                    release();
                if (!error.isEmpty())
                    emit failed(request, error, false);
            });
    p->start();
    emit changed();
}
bool ChemistryJob::propose(const ChemistryResult &result, int conformer) {
    if (!active() || result.generation != generation || session.revision() != result.revision)
        fail(tr("分子が変更されたため整形結果を破棄しました。"));
    auto next = session.document();
    const auto mode = result.request.mode;
    if (mode == "conformers") {
        if (conformer < 0 || conformer >= result.conformers.size())
            fail(tr("配座が見つかりませんでした。"));
        applyResultGeometry(next, result.conformers[conformer].document);
        double moved = 0;
        for (int i = 0; i < next.atoms.size(); ++i)
            moved = std::max(moved, (next.atoms[i].position - result.base.atoms[i].position).norm());
        if (moved < 1e-3)
            return false;
    } else if (mode == "smiles") {
        replaceCoordinateStructure(next, result.document);
        next.charge = next.formalCharge();
    } else if (mode == "fragment")
        insertFragment(next, result.document);
    else {
        next = result.document;
        next.envelope = result.base.envelope;
    }
    const auto name = mode == "smiles"       ? tr("SMILESから分子を作成")
                      : mode == "fragment"   ? tr("SMILESから断片を追加")
                      : mode == "regenerate" ? tr("3Dを再生成")
                      : mode == "conformers" ? tr("配座を適用")
                                             : tr("構造を整形");
    return session.propose(name, next, result.request.remote);
}
int chemistryWorker(const QString &input, const QString &output) {
    try {
        const auto request = parseJson(readFile(input));
        const auto mode = request["mode"].toString();
        if (mode == "conformers") {
            QJsonArray list;
            for (const auto &c : searchConformers(Document::fromJson(request["document"].toObject()),
                                                  request["count"].toInt(10)))
                list.append(QJsonObject{{"document", c.document.toJson()}, {"energy", c.energy}});
            writeFile(output, QJsonDocument(QJsonObject{{"conformers", list}}).toJson());
        } else {
            const auto d = mode == "smiles" || mode == "fragment"
                               ? fromSmiles(request["smiles"].toString())
                               : refine(Document::fromJson(request["document"].toObject()),
                                        mode == "regenerate", idsFromJson(request["moving"]));
            writeFile(output, QJsonDocument(QJsonObject{{"document", d.toJson()}}).toJson());
        }
        return 0;
    } catch (const std::exception &e) {
        try {
            writeFile(output, QJsonDocument(QJsonObject{{"error", QString::fromUtf8(e.what())}}).toJson());
        } catch (...) {
        }
        return 1;
    }
}
} // namespace mf
