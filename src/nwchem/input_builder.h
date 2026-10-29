#pragma once
#include "nwchem/nwchem.h"

namespace mf {
// Internal writer: all source attribution is supplied by the generator, never inferred from text.
class NwchemInputBuilder {
  public:
    NwchemGeneratedInput result;

    void append(const QString &text) {
        result.text += text;
        line += text.count('\n');
    }
    void append(const NwchemGeneratedInput &part) {
        for (auto block : part.outline.blocks) {
            block.line += line;
            result.outline.blocks << block;
        }
        for (const auto i : part.outline.advanced)
            result.outline.advanced.insert(line + i);
        for (const auto i : part.outline.extra)
            result.outline.extra.insert(line + i);
        for (auto it = part.outline.settings.begin(); it != part.outline.settings.end(); ++it)
            for (const auto i : it.value())
                result.outline.settings[it.key()].insert(line + i);
        append(part.text);
    }
    void sourced(const QString &text, const QStringList &keys, bool advanced = false, bool extra = false) {
        const auto lines = text.split('\n');
        for (int i = 0; i < lines.size(); ++i) {
            if (lines[i].trimmed().isEmpty())
                continue;
            for (const auto &key : keys)
                result.outline.settings[key].insert(line + i);
            if (advanced)
                result.outline.advanced.insert(line + i);
            if (extra)
                result.outline.extra.insert(line + i);
        }
        append(text);
    }
    void block(const QString &keyword, const NwchemGeneratedInput &body, const QString &setting = {},
               const QString &group = {}, QList<int> tabs = {2}, QString title = {}) {
        if (body.text.isEmpty())
            return;
        int first = 0;
        while (first < body.text.size() && body.text[first] == '\n')
            ++first;
        auto visible = body.text.mid(first);
        while (visible.endsWith('\n'))
            visible.chop(1);
        if (title.isEmpty())
            title = visible.section('\n', 0, 0).trimmed();
        NwchemInputBlock block{line + first, int(visible.count('\n') + 1), keyword, title, tabs, setting,
                               group};
        if (body.outline.advanced.isEmpty())
            block.group.clear();
        if (!body.outline.advanced.isEmpty() && !block.tabs.contains(2))
            block.tabs << 2;
        result.outline.blocks << block;
        append(body);
    }
    void block(const QString &keyword, const QString &text, const QString &setting = {},
               const QString &group = {}, QList<int> tabs = {2}, const QString &title = {}) {
        block(keyword, NwchemGeneratedInput{text, {}}, setting, group, tabs, title);
    }
    void advancedBlock(const QString &keyword, const QString &text, const QStringList &keys,
                       const QString &group) {
        NwchemInputBuilder body;
        body.sourced(text, keys, true);
        block(keyword, body.result, {}, group);
    }

  private:
    int line = 0;
};
} // namespace mf
