#pragma once
#include "LivingActivity.h"
#include <algorithm>
#include <limits>

namespace LivingActivity {
    // Exact external prerequisites only. Unknown errors, native uncertainty,
    // safety interruptions and unsupported adapters are NOT disguised as waits.
    inline uint64_t PreparationWaitDelay(const std::string& blocker) {
        for(const auto* known : {"purchase_hourly_limit", "purchase_seller_weekly_limit",
            "purchase_daily_limit", "purchase_protected_money_shortfall",
            "profession_material_has_legacy_commitment", "profession_material_source_unavailable",
            "profession_paid_material_in_transit", "vendor_limited_stock_unavailable"})
            if(blocker==known)return 300000;
        return 0;
    }
    inline bool PrepareExternalPreparationWait(const Task& saved,const std::string& blocker,
        uint64_t now,Task& next) {
        next={};const auto delay=PreparationWaitDelay(blocker);
        if(!delay || !saved.accepted || saved.mode!=Mode::Active || !saved.parent.empty() ||
            saved.root!=saved.id || saved.revision==std::numeric_limits<uint64_t>::max() ||
            now<saved.updatedAtMs || now>std::numeric_limits<uint64_t>::max()-delay ||
            (saved.phase!=Phase::Preparing && saved.phase!=Phase::Traveling && saved.phase!=Phase::Verifying))
            return false;
        next=saved;++next.revision;next.phase=Phase::WaitingExternal;next.updatedAtMs=now;
        next.retryAtMs=now+delay;next.checkpoint.blocker=blocker;
        // Preserve the accepted recipe, due time, active elapsed time and every
        // native claim. A wait is neither cancellation nor completion.
        return true;
    }
    inline uint64_t NextPreparationDispatch(uint64_t now,uint64_t retryAtMs) {
        const auto maximum=std::numeric_limits<uint64_t>::max();
        return std::max(now>maximum-5000 ? maximum : now+5000,retryAtMs);
    }
}
