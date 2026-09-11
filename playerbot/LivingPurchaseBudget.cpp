#include "LivingPurchaseBudget.h"
#include "LivingActivity.h"
#include <charconv>
#include <limits>
#include <stdexcept>

namespace LivingActivity {
    bool WithinPurchaseBudget(const PurchaseSpend& s,const PurchaseLimits& limits,
        uint32_t wallet,uint32_t discretionary,uint32_t price,bool auction,std::string& blocker) {
        auto reject=[&](const char* why){blocker=why; return false;};
        if (!s.complete) return reject("purchase_budget_unavailable");
        if (!limits.dailyPercent || limits.dailyPercent > 100 || !price)
            return reject("purchase_budget_policy_invalid_or_disabled");
        if (auction && s.auctionCountHour >= limits.auctionsPerHour) return reject("purchase_hourly_limit");
        if (price > wallet || price > discretionary) return reject("purchase_protected_money_shortfall");
        constexpr auto max=std::numeric_limits<uint64_t>::max();
        if (s.spentDay > max-wallet || s.committed > max-s.spentDay ||
            s.spentDay+s.committed > max-price) return reject("purchase_budget_overflow");
        const uint64_t basis=uint64_t(wallet)+s.spentDay;
        // Divide first, retaining the remainder; no multiplication overflow.
        const uint64_t allowed=(basis/100)*limits.dailyPercent+(basis%100)*limits.dailyPercent/100;
        if (s.spentDay+s.committed+price > allowed) return reject("purchase_daily_limit");
        blocker.clear(); return true;
    }
    std::string PurchaseSpendQuery(uint32_t actor,uint64_t now,const std::string& operation) {
        if (!actor || now < 86400000 || (!operation.empty() && !IsUuid(operation)))
            throw std::invalid_argument("Exact native purchase budget scope required");
        const auto actorSql=std::to_string(actor), day=std::to_string(now-86400000), hour=std::to_string(now-3600000);
        // Preserve the existing AH accounting (including conservative unit
        // rounding). Aggregate first, so multiple tasks do not multiply history.
        const std::string used="JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.claimed_consumption[0].used'))";
        const std::string valid="COALESCE(JSON_LENGTH(JSON_EXTRACT(o.before_state,'$.native.claimed_consumption'))=1"
            " AND JSON_UNQUOTE(JSON_EXTRACT(o.before_state,'$.native.claimed_consumption[0].location'))='money'"
            " AND "+used+" REGEXP '^[1-9][0-9]{0,9}$' AND CAST("+used+" AS UNSIGNED)<=2147483647,0)";
        const std::string scope=" FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid="+actorSql+" AND o.kind='vendor_purchase' AND o.state<>'rejected' AND "
            "(o.state IN ('intent','reconciling') OR o.updated_at_ms>"+day+")"+
            (operation.empty() ? "" : " AND NOT(o.operation_id="+SqlValue(operation)+" AND o.state='intent')");
        return "SELECT a.purchases,a.spent+v.spent,v.committed,v.invalid FROM "
            "(SELECT COALESCE(SUM(UNIX_TIMESTAMP(occurred_at)*1000>"+hour+"),0) purchases,"
            "COALESCE(SUM(CAST(unit_price_copper AS DECIMAL(30,0))*quantity),0) spent"
            " FROM organic_economy_auction_history WHERE buyer_guid="+actorSql+
            " AND outcome IN ('bid','sold') AND occurred_at>FROM_UNIXTIME("+day+"/1000)) a CROSS JOIN "
            "(SELECT COALESCE(SUM(CASE WHEN o.state='verified' AND "+valid+" THEN CAST("+used+
            " AS DECIMAL(30,0)) ELSE 0 END),0) spent,COALESCE(SUM(CASE WHEN o.state IN ('intent','reconciling') AND "+valid+
            " THEN CAST("+used+" AS DECIMAL(30,0)) ELSE 0 END),0) committed,COALESCE(SUM(NOT("+valid+")),0) invalid"+scope+") v";
    }
    bool DecodePurchaseSpend(const std::array<std::string,4>& columns,PurchaseSpend& spend) {
        spend={}; std::array<uint64_t,4> numbers{};
        for (size_t i=0;i!=columns.size();++i) {
            const auto& value=columns[i];
            if (value.empty()) return false;
            const auto result=std::from_chars(value.data(),value.data()+value.size(),numbers[i]);
            if (result.ec!=std::errc{} || result.ptr!=value.data()+value.size()) return false;
        }
        if (numbers[3]) return false;
        spend={true,numbers[0],numbers[1],numbers[2]}; return true;
    }
    uint64_t PurchaseEpoch::Read(uint32_t actor) const {
        if (!actor) return 0;
        const auto& slot=slots[actor%slots.size()];
        const auto first=slot.generation.load(std::memory_order_acquire);
        if (slot.active.load(std::memory_order_acquire)) return 0;
        const auto last=slot.generation.load(std::memory_order_acquire);
        return first==last ? first : 0;
    }
    void PurchaseEpoch::Changed(uint32_t actor) {
        if (!actor) return;
        auto& value=slots[actor%slots.size()].generation;
        auto old=value.load(std::memory_order_relaxed);
        while (old && !value.compare_exchange_weak(old,old==std::numeric_limits<uint64_t>::max() ? 0 : old+1,
            std::memory_order_release,std::memory_order_relaxed)) {}
    }
    void PurchaseEpoch::Begin(uint32_t actor) {
        if (!actor) return;
        slots[actor%slots.size()].active.fetch_add(1,std::memory_order_acq_rel); Changed(actor);
    }
    void PurchaseEpoch::End(uint32_t actor) {
        if (!actor) return;
        Changed(actor); slots[actor%slots.size()].active.fetch_sub(1,std::memory_order_release);
    }
    PurchaseEpoch& NativePurchaseEpoch() { static PurchaseEpoch epochs; return epochs; }
}
