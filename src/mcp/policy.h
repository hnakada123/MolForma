#pragma once
#include "model/document.h"
#include <QStringList>

namespace mf {
// Settings that choose the program a calculation runs and the environment it inherits, and the
// unchecked additional NWChem input (which could start other programs through NWChem).
QJsonObject protectedExecution(const Document &document);
bool isProtectedExecutionKey(const QString &key);
// NWChem settings with the same protection: the additional input lines.
bool isProtectedNwchemKey(const QString &key);

// Restrictions fixed on the command line. MCP requests can read but never change them.
struct McpPolicy {
    // --mcp-allow-execution-settings: MCP may change the executable, launcher and environment.
    bool allowExecutionSettings = false;
    // --mcp-allow-dir: canonical directories MCP file access stays within; empty means unrestricted.
    QStringList directories;
    bool restricted() const {
        return !directories.isEmpty();
    }
    void allowDirectory(const QString &path);
    // Rejects a path that resolves (following symbolic links) outside the allowed directories.
    void checkPath(const QString &path, const QString &base = {}) const;
    // Rejects a calculation whose job, reference, movecs or scratch locations are outside them.
    void checkRun(const Document &document, const QString &base) const;
    QJsonObject toJson() const;
};
} // namespace mf
