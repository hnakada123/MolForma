// SPDX-License-Identifier: GPL-2.0-or-later
#include "foundation/i18n.h"
#include "results/result_parsing.h"
#include "results/results.h"

namespace mf::result_detail {
void xyz(ResultData &r, const QStringList &lines) {
    int i = 0;
    while (i < lines.size()) {
        if (lines[i].trimmed().isEmpty()) {
            ++i;
            continue;
        }
        const int count = integer(lines[i++].trimmed(), 1, maxAtoms);
        if (i + count >= lines.size())
            fail(tr("XYZの構造が途中で切れています。"));
        const QString title = lines[i++];
        Document d;
        for (int n = 0; n < count; ++n) {
            const auto row = words(lines[i++]);
            if (row.size() != 4)
                fail(tr("結果XYZは元素・X・Y・Zの4列で指定してください。"));
            d.atoms << atom(row[0], point(row, 1));
        }
        addFrame(r, d, title);
    }
}

} // namespace mf::result_detail
