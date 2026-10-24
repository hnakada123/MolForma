#include "mcp/policy.h"
#include "nwchem/nwchem_advanced.h"
#include "nwchem/nwchem_execution.h"
#include <QDir>
#include <QFileInfo>

namespace mf {
namespace {
// Missing trailing components are kept, so a file that is about to be written resolves too.
QString resolvedPath(const QString &path, const QString &base) {
    if (path.trimmed().isEmpty())
        fail("Specify a path.");
    // Expand "~" exactly as execution does, so the checked path is the one that is used.
    QString current = absolutePath(path, base.isEmpty() ? QDir::currentPath() : base);
    QStringList missing;
    for (;;) {
        const QFileInfo info(current);
        if (info.exists())
            break;
        if (info.isSymLink())
            fail("Symbolic link without a target: " + current);
        const auto parent = info.absolutePath();
        if (parent == current)
            break;
        missing.prepend(info.fileName());
        current = parent;
    }
    auto resolved = QFileInfo(current).canonicalFilePath();
    if (resolved.isEmpty())
        resolved = current;
    for (const auto &part : missing)
        resolved += '/' + part;
    return QDir::cleanPath(resolved);
}
} // namespace
QJsonObject protectedExecution(const Document &document) {
    const auto s = executionSettings(document);
    return {{"executable", s["executable"]}, {"launcher", s["launcher"]}, {"environment", s["environment"]}};
}
bool isProtectedExecutionKey(const QString &key) {
    return key == "executable" || key == "launcher" || key == "environment";
}
void McpPolicy::allowDirectory(const QString &path) {
    const QFileInfo info(path);
    if (!info.isDir())
        fail("Allowed MCP directory does not exist: " + path);
    directories.append(info.canonicalFilePath());
}
void McpPolicy::checkPath(const QString &path, const QString &base) const {
    if (!restricted())
        return;
    const auto resolved = resolvedPath(path, base);
    for (const auto &root : directories)
        if (resolved == root || resolved.startsWith(root.endsWith('/') ? root : root + '/'))
            return;
    fail("Path is outside the directories allowed for MCP (--mcp-allow-dir): " + resolved);
}
void McpPolicy::checkRun(const Document &document, const QString &base) const {
    if (!restricted())
        return;
    const auto s = executionSettings(document);
    checkPath(s["job_root"].toString(), base);
    const auto reference = s["reference_directory"].toString();
    if (!reference.isEmpty())
        checkPath(reference, base);
    const auto advanced = nwchemAdvanced(document);
    const auto input = advanced["vectors_input"].toString();
    if (advanced["vectors_guess"] == "file" && !input.isEmpty())
        checkPath(input, reference.isEmpty() ? base : QDir(base).absoluteFilePath(reference));
    // Relative locations stay inside the job folder, which is under job_root.
    for (const auto *key : {"scratch_dir", "permanent_dir", "vectors_output"})
        if (const auto value = advanced[key].toString(); QDir::isAbsolutePath(value))
            checkPath(value);
}
QJsonObject McpPolicy::toJson() const {
    return {{"allowExecutionSettings", allowExecutionSettings},
            {"allowedDirectories",
             restricted() ? QJsonValue(QJsonArray::fromStringList(directories)) : QJsonValue()}};
}
} // namespace mf
