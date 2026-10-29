// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QJsonValue>
#include <QStringList>
#include <QVector>
#include <optional>
#include <stdexcept>

namespace mf {
enum class DiagnosticSeverity { Error, Warning };
struct DiagnosticFix {
    QString setting, label;
    QJsonValue value;
};
struct Diagnostic {
    QString code, setting, message;
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    int tab = 1; // Workspace index: molecule 0, calculation 1, execution 2.
    std::optional<DiagnosticFix> fix;
    QString identity() const {
        return code + ':' + setting;
    }
};
struct ValidationReport {
    // Text projections retained for the existing MCP responses and validation callers.
    QStringList errors, warnings;
    QVector<Diagnostic> diagnostics;
    void add(Diagnostic diagnostic) {
        auto &messages = diagnostic.severity == DiagnosticSeverity::Error ? errors : warnings;
        for (const auto &existing : diagnostics)
            if (existing.identity() == diagnostic.identity() && existing.message == diagnostic.message &&
                existing.severity == diagnostic.severity)
                return;
        if (!messages.contains(diagnostic.message))
            messages << diagnostic.message;
        diagnostics << std::move(diagnostic);
    }
    void error(const QString &code, const QString &setting, const QString &message,
               std::optional<DiagnosticFix> fix = {}, int tab = 1) {
        add({code, setting, message, DiagnosticSeverity::Error, tab, std::move(fix)});
    }
    void warning(const QString &code, const QString &setting, const QString &message,
                 std::optional<DiagnosticFix> fix = {}, int tab = 1) {
        add({code, setting, message, DiagnosticSeverity::Warning, tab, std::move(fix)});
    }
};
// A refusal that keeps the diagnostics behind it, so that a caller can still point at each setting.
// what() is the error messages, one per line.
class ValidationFailure : public std::runtime_error {
  public:
    explicit ValidationFailure(const ValidationReport &report)
        : std::runtime_error(report.errors.join('\n').toStdString()), report(report) {}
    ValidationReport report;
};
} // namespace mf
