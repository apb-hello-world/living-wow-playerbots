#ifndef LIVING_ACTIVITY_RECEIPTS_H
#define LIVING_ACTIVITY_RECEIPTS_H
#include "LivingActivity.h"
#include <algorithm>
#include <deque>
#include <limits>
#include <set>
#include <utility>

namespace LivingActivity {
    struct ReceiptRetry {
        unsigned failures = 0;
        uint64_t dueAtMs = 0;
        void Missed(uint64_t now) {
            if (failures < 1000000) ++failures;
            const uint64_t delay = failures == 1 ? 5000 : failures == 2 ? 10000 : failures == 3 ? 30000 : 60000;
            dueAtMs = now > std::numeric_limits<uint64_t>::max()-delay ?
                std::numeric_limits<uint64_t>::max() : now+delay;
        }
    };
    using ReceiptSet = std::set<std::pair<std::string,uint64_t>>;

    // Same bounded coordinator queue, not a second scheduler. Fresh writes are
    // batched; a previously unacknowledged write is retried ALONE so a native
    // SQL error cannot repeatedly roll back unrelated valid transactions.
    template<class Pending> size_t PrepareReceiptBatch(std::deque<Pending>& pending, size_t limit, uint64_t now,
        bool preferRetry = false) {
        if (limit && preferRetry) for (size_t i = 0; i < pending.size(); ++i)
            if (pending[i].retry.failures && pending[i].retry.dueAtMs <= now) {
                std::rotate(pending.begin(),pending.begin()+i,pending.begin()+i+1);
                return 1;
            }
        size_t selected = 0;
        for (size_t i = 0; i < pending.size() && selected < limit; ++i) {
            if (pending[i].retry.failures || pending[i].retry.dueAtMs > now) continue;
            std::rotate(pending.begin()+selected,pending.begin()+i,pending.begin()+i+1);
            ++selected;
        }
        if (selected || !limit) return selected;
        for (size_t i = 0; i < pending.size(); ++i) if (pending[i].retry.dueAtMs <= now) {
            std::rotate(pending.begin(),pending.begin()+i,pending.begin()+i+1);
            return 1;
        }
        return 0;
    }
    // A successful SQL transaction may contain rejected CAS statements. Exact
    // receipts acknowledge each valid write independently. Missing receipts keep
    // the ENTIRE immutable write (including native after-state) for reconciliation;
    // this helper never cancels work, releases claims, or replays a native effect.
    template<class Pending,class Acknowledge> size_t SettleReceiptBatch(std::deque<Pending>& pending,
        size_t count, bool healthyQuery, const ReceiptSet& receipts, uint64_t now, Acknowledge acknowledge) {
        if (count > pending.size()) return 0;
        size_t accepted = 0;
        for (size_t i = 0; i < count; ++i) {
            Pending write = std::move(pending.front()); pending.pop_front();
            if (healthyQuery && receipts.count({write.plan.task,write.plan.revision})) {
                acknowledge(write); ++accepted;
            } else {
                write.retry.Missed(now); pending.push_back(std::move(write));
            }
        }
        return accepted;
    }
}
#endif
