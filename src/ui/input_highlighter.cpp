// SPDX-License-Identifier: GPL-2.0-or-later
#include "ui/input_highlighter.h"
#include "ui/theme.h"
#include <QApplication>
#include <QRegularExpression>
#include <QSyntaxHighlighter>

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
} // namespace
void highlightInput(QTextDocument *document) {
    new InputHighlighter(document);
}
} // namespace mf
