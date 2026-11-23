// SPDX-License-Identifier: GPL-2.0-or-later
#include "results/result_bundle.h"
#include "foundation/files.h"
#include "foundation/i18n.h"
#include "results/nwchem_log.h"
#include "results/result_files.h"
#include "results/result_parsing.h"
#include "results/spectrum.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <cmath>

namespace mf {
namespace {
QJsonObject envelope(const QByteArray &bytes) {
    if (bytes.size() > result_detail::maxBytes)
        fail(tr("MolForma結果ファイルのサイズが上限を超えています。"));
    const auto object = parseJson(bytes);
    if (object["format"] != "molforma-result" || object["version"] != 1 || !object["result"].isObject())
        fail(tr("MolForma結果ファイルの形式またはバージョンが不正です。"));
    return object;
}
QString entryName(const QJsonObject &entry) {
    const auto name = entry["name"].toString();
    if (name.isEmpty() || name.size() > 255 || name.contains('/') || name.contains('\\') ||
        name.contains('\n') || name.contains('\r'))
        fail(tr("結果ファイルに埋め込まれたログの名前が不正です。"));
    return name;
}
ResultData readEntry(const QJsonObject &entry) {
    const auto name = entryName(entry);
    const auto text = entry["text"].toString();
    if (text.isEmpty() || text.toUtf8().size() > result_detail::maxBytes)
        fail(tr("結果ファイルに有効なNWChemログが含まれていません。"));
    ResultData result;
    result.source = name;
    readNwchemResult(result, text.toUtf8());
    return result;
}
QByteArray unchangedFile(const QString &path, const QString &stamp) {
    if (stamp.isEmpty() || resultSourceStamp(path) != stamp)
        fail(tr("読込後に結果ファイルが変更されています。再読込してから保存してください。"));
    const auto bytes = result_detail::boundedRead(path);
    if (resultSourceStamp(path) != stamp)
        fail(tr("読込後に結果ファイルが変更されています。再読込してから保存してください。"));
    return bytes;
}
} // namespace
std::optional<NmrDisplay> resultNmrDisplay(const ResultData &result) {
    if (!result.properties.contains("nmr_display"))
        return std::nullopt;
    const auto display = result.properties["nmr_display"].toObject();
    return NmrDisplay{display["references"].toObject(), display["parameters"].toObject(),
                      display["reference_name"].toString(), display["automatic_width"].toBool(true)};
}
QJsonObject resultSpectrumParameters(const ResultData &result, const QJsonObject &overrides) {
    const auto display = resultNmrDisplay(result);
    if (!display || (!overrides["kind"].toString().isEmpty() && overrides["kind"] != "nmr"))
        return overrides;
    auto parameters = display->parameters;
    const auto nucleus = overrides["nucleus"].toString(parameters["nucleus"].toString());
    if (nucleus != parameters["nucleus"].toString()) {
        parameters.remove("minimum");
        parameters.remove("maximum");
    }
    parameters["nucleus"] = nucleus;
    parameters["reference_ppm"] = display->references.value(nucleus);
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        parameters[it.key()] = it.value();
    if (display->automaticWidth && !overrides.contains("width"))
        parameters["width"] = nmrDisplayWidth(nucleus, parameters["coupling"].toBool(),
                                              parameters["spectrometer_mhz"].toDouble(400));
    return parameters;
}
ResultData readResultBundle(const QByteArray &bytes, const QString &source) {
    const auto bundle = envelope(bytes);
    auto result = readEntry(bundle["result"].toObject());
    result.source = source;
    result.properties["result_bundle"] = QJsonObject{{"name", entryName(bundle["result"].toObject())}};
    if (bundle.contains("nmr")) {
        if (!bundle["nmr"].isObject())
            fail(tr("結果ファイルのNMR表示条件が不正です。"));
        const auto nmr = bundle["nmr"].toObject(), reference = nmr["reference"].toObject();
        if (!nmr["parameters"].isObject() || !nmr["reference"].isObject() ||
            (nmr.contains("automatic_width") && !nmr["automatic_width"].isBool()))
            fail(tr("結果ファイルのNMR表示条件が不正です。"));
        QJsonObject values;
        QString name;
        if (reference.contains("text")) {
            values = tmsShieldingReference(readEntry(reference));
            name = entryName(reference);
        } else {
            values = reference["shieldings"].toObject();
            name = tr("手入力の基準値");
        }
        for (const auto &value : result.properties["shielding"].toArray()) {
            const auto element = value.toObject()["element"].toString();
            const auto key = element == "H" ? "1H" : element == "C" ? "13C" : "";
            if (*key && (!values[key].isDouble() || !std::isfinite(values[key].toDouble())))
                fail(tr("NMR結果の保存には、含まれる¹H・¹³Cそれぞれの基準値が必要です。TMSログを読み込んでく"
                        "ださい。"));
        }
        auto parameters = nmr["parameters"].toObject();
        parameters["kind"] = "nmr";
        if (!parameters.contains("nucleus"))
            parameters["nucleus"] = "1H";
        if (!parameters.contains("spectrometer_mhz"))
            parameters["spectrometer_mhz"] = 400.;
        if (!parameters["spectrometer_mhz"].isDouble() || parameters["spectrometer_mhz"].toDouble() < 10 ||
            parameters["spectrometer_mhz"].toDouble() > 2000)
            fail(tr("測定周波数は10〜2000 MHzにしてください。"));
        if (!parameters.contains("width"))
            parameters["width"] = .05;
        result.properties["nmr_display"] =
            QJsonObject{{"references", values},
                        {"reference_name", name},
                        {"parameters", parameters},
                        {"automatic_width", nmr["automatic_width"].toBool(true)}};
        parameters = resultSpectrumParameters(result);
        calculateSpectrum(result,
                          parameters); // Reject invalid presets rather than silently drawing another chart.
        auto display = result.properties["nmr_display"].toObject();
        display["parameters"] = parameters;
        result.properties["nmr_display"] = display;
    }
    return result;
}
void saveResultBundle(const QString &path, const ResultData &result, const ResultBundleRequest &request) {
    if (result.format != "NWChem")
        fail(tr("1ファイルへの結果保存はNWChemの出力に対応しています。"));
    if (QFileInfo(path).absoluteFilePath() == QFileInfo(result.source).absoluteFilePath() ||
        (!request.referencePath.isEmpty() &&
         QFileInfo(path).absoluteFilePath() == QFileInfo(request.referencePath).absoluteFilePath()))
        fail(tr("元のログとは別のファイル名で保存してください。"));
    const auto bytes = unchangedFile(result.source, request.sourceStamp);
    auto bundle = result.properties.contains("result_bundle")
                      ? envelope(bytes)
                      : QJsonObject{{"format", "molforma-result"},
                                    {"version", 1},
                                    {"result", QJsonObject{{"name", QFileInfo(result.source).fileName()},
                                                           {"text", QString::fromUtf8(bytes)}}}};
    if (result.properties.contains("shielding")) {
        QJsonObject reference;
        if (!request.referencePath.isEmpty()) {
            const auto text = unchangedFile(request.referencePath, request.referenceStamp);
            reference = QFileInfo(request.referencePath).suffix().toLower() == "json"
                            ? envelope(text)["result"].toObject()
                            : QJsonObject{{"name", QFileInfo(request.referencePath).fileName()},
                                          {"text", QString::fromUtf8(text)}};
        } else if (const auto display = resultNmrDisplay(result);
                   display && display->references == request.references) {
            reference = bundle["nmr"].toObject()["reference"].toObject();
        } else {
            reference = {{"shieldings", request.references}};
        }
        bundle["nmr"] = QJsonObject{{"reference", reference},
                                    {"parameters", request.parameters},
                                    {"automatic_width", request.automaticWidth}};
    }
    const auto encoded = QJsonDocument(bundle).toJson();
    readResultBundle(encoded, path);
    writeFile(path, encoded);
}
} // namespace mf
