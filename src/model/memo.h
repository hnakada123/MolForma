// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "model/i18n.h"
#include <QList>

namespace mf {
// The results of a function for the last few arguments it was called with, in each interface
// language. One change refreshes several panels and plans that check the same document; comparing
// documents costs little while their copies share their data (Qt compares shared lists by pointer).
template <typename Key, typename Result, qsizetype Size = 4> class Memo {
  public:
    template <typename Compute> Result get(const Key &key, Compute &&compute) {
        const bool language = english();
        for (qsizetype i = 0; i < entries.size(); ++i)
            if (entries[i].english == language && entries[i].key == key) {
                entries.move(i, 0);
                return entries.first().result;
            }
        Result result = compute();
        entries.prepend({key, language, result});
        if (entries.size() > Size)
            entries.removeLast();
        return result;
    }

  private:
    struct Entry {
        Key key;
        bool english;
        Result result;
    };
    QList<Entry> entries;
};
} // namespace mf
