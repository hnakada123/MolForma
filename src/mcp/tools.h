#pragma once
#include "nwchem/nwchem_execution.h"
#include "results/results.h"
#include <optional>

namespace mf {
// Tool definitions and result views shared by the GUI (McpGui) and headless (McpHeadless) servers.

// Pages stay below LLM clients' inline tool-output limits; a larger entry is returned alone.
constexpr qsizetype McpPageBytes = 40 * 1024, McpEntryBytes = 2 * 1024 * 1024;
// Text resources are cut to their end so that an attached log stays readable.
constexpr qsizetype McpResourceChars = 100000;
// Claude Code keeps results of these tools inline up to this many characters.
constexpr int McpLargeResultChars = 100000;
bool appendPage(QJsonArray &items, const QJsonObject &item, qsizetype &bytes);

// documentId is always required; mutations also require expectedRevision.
QJsonObject mcpTool(const QString &name, const QString &title, const QString &description,
                    QJsonObject properties, QJsonArray required = {}, bool mutation = false,
                    bool readOnly = true, const QJsonObject &output = {}, const QJsonObject &meta = {});
QJsonObject resultsTool();
// confirm: Claude Code asks a person before every call (when MCP may choose the program).
QJsonObject calculationTool(const QString &description, const QJsonObject &output, bool confirm);

QJsonObject calculationJson(const NwchemRun &run, const QString &savedBundle);
// get_results: summary plus one paginated section of the loaded result.
QJsonObject resultsJson(const ResultData &result, const QJsonObject &args, QJsonObject summary);

// Calculation output and result resources, offered by both MCP modes.
QJsonArray runtimeResources();
std::optional<QJsonObject> runtimeResource(const QString &uri, const QJsonObject &state,
                                           const ResultData &result, const ExecutionPlan &run);
} // namespace mf
