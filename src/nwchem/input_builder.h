// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "nwchem/nwchem.h"
#include "nwchem/nwchem_plan.h"

namespace mf {
// Internal writer: all source attribution is supplied by the generator, never inferred from text.
class NwchemInputBuilder {
  public:
    NwchemGeneratedInput result;

    void append(const QString &text) {
        begin(text);
        result.text += text;
        line += text.count('\n');
    }
    void append(const NwchemGeneratedInput &part) {
        begin(part.text);
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
        begin(text);
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
               const QString &group = {}, QList<SettingTarget> targets = {SettingTarget::Calculation},
               QString title = {}) {
        if (body.text.isEmpty())
            return;
        begin(body.text);
        int first = 0;
        while (first < body.text.size() && body.text[first] == '\n')
            ++first;
        auto visible = body.text.mid(first);
        while (visible.endsWith('\n'))
            visible.chop(1);
        if (title.isEmpty())
            title = visible.section('\n', 0, 0).trimmed();
        NwchemInputBlock block{line + first, int(visible.count('\n') + 1), keyword, title, targets, setting,
                               group};
        if (body.outline.advanced.isEmpty())
            block.group.clear();
        if (!body.outline.advanced.isEmpty() && !block.targets.contains(SettingTarget::Calculation))
            block.targets << SettingTarget::Calculation;
        result.outline.blocks << block;
        append(body);
    }
    void block(const QString &keyword, const QString &text, const QString &setting = {},
               const QString &group = {}, QList<SettingTarget> targets = {SettingTarget::Calculation},
               const QString &title = {}) {
        block(keyword, NwchemGeneratedInput{text, {}}, setting, group, targets, title);
    }
    void advancedBlock(const QString &keyword, const QString &text, const QStringList &keys,
                       const QString &group) {
        NwchemInputBuilder body;
        body.sourced(text, keys, true);
        block(keyword, body.result, {}, group);
    }
    // The one way a task gets into the input: the plan's next generated task, which the generator expects
    // in this phase, as a block of its line with the settings (`keys`) the line comes from. Nothing is
    // written for a task that a continued restart leaves out.
    void task(NwchemTaskCursor &tasks, NwchemTaskPhase phase, const QString &setting,
              const QStringList &keys = {}) {
        task(tasks.take(phase), keys, false, setting, {});
    }
    // The same for a task that the details of "2 計算条件" add (see advancedBlock).
    void advancedTask(NwchemTaskCursor &tasks, NwchemTaskPhase phase, const QStringList &keys,
                      const QString &group) {
        task(tasks.take(phase), keys, true, {}, group);
    }

  private:
    int line = 0;
    // A skipped task took the empty line before it along (see task), as is right when an empty line or
    // nothing follows: text that goes on without an empty line gets that line back.
    bool taken = false;

    // Before text is written, and before its lines are counted from `line`.
    void begin(const QString &text) {
        if (!text.isEmpty())
            resume(text.startsWith('\n'));
    }
    void resume(bool emptyLine) {
        if (taken && !emptyLine) {
            result.text += '\n';
            ++line;
        }
        taken = false;
    }
    void task(const NwchemTaskStep &step, const QStringList &keys, bool advanced, const QString &setting,
              const QString &group) {
        NwchemInputBuilder body;
        body.sourced(step.command + '\n', keys, advanced);
        if (!step.skipped) {
            block("task", body.result, setting, group);
            return;
        }
        // The input reads as if the task's line had been taken out of the complete one: the blocks around
        // it stay, and where the line stood between two empty lines (the input's end counts as one), one
        // of them goes with it. The settings of the line stay known to the outline, without a line.
        resume(false);
        for (const auto &key : keys)
            if (!result.outline.settings.contains(key))
                result.outline.settings.insert(key, {});
        if (result.text.endsWith("\n\n") || result.text == "\n") {
            result.text.chop(1);
            --line;
            taken = true;
        }
    }
};
} // namespace mf
