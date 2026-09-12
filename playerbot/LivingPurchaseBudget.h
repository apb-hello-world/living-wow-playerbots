#ifndef LIVING_PURCHASE_BUDGET_H
#define LIVING_PURCHASE_BUDGET_H
#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace LivingActivity {
    struct PurchaseSpend {
        bool complete = false;
        uint64_t auctionCountHour = 0, spentDay = 0, committed = 0;
        uint64_t sellerPurchasesWeek = 0;
    };
    struct PurchaseLimits { uint32_t auctionsPerHour = 3, dailyPercent = 25; };
    // Existing rolling 24-hour discretionary policy, shared by source. Pending
    // commitments reduce availability but do NOT inflate the earned-wallet base.
    bool WithinPurchaseBudget(const PurchaseSpend& spend,const PurchaseLimits& limits,
        uint32_t wallet,uint32_t discretionary,uint32_t price,bool auction,std::string& blocker);
    // One bounded aggregate, used at existing purchase checks / requested
    // coordinator reads. Never a per-tick query. Excluding an exact current
    // intent avoids counting its quoted price twice; other uncertain intents
    // keep holding funds regardless of age or task cancellation.
    std::string PurchaseSpendQuery(uint32_t actor,uint64_t nowMs,const std::string& currentOperation = "",uint32_t seller=0);
    bool DecodePurchaseSpend(const std::array<std::string,4>& columns,PurchaseSpend& spend);
    bool DecodeSellerPurchaseCount(const std::string& value,PurchaseSpend& spend);

    // Bounded value-only invalidation for asynchronous budget reads. Colliding
    // actor slots may force a reread, never authorize a stale allowance. No
    // pointers, DB work, timers or growing per-character map lives here.
    class PurchaseEpoch {
    public:
        uint64_t Read(uint32_t actor) const;
        void Begin(uint32_t actor);
        void End(uint32_t actor);
        void Changed(uint32_t actor);
    private:
        struct Slot { std::atomic<uint64_t> generation{1}; std::atomic<uint32_t> active{0}; };
        std::array<Slot,4096> slots{};
    };
    PurchaseEpoch& NativePurchaseEpoch();
    class PurchaseMutation {
    public:
        explicit PurchaseMutation(uint32_t actor) : actor(actor) { NativePurchaseEpoch().Begin(actor); }
        ~PurchaseMutation() { NativePurchaseEpoch().End(actor); }
        PurchaseMutation(const PurchaseMutation&) = delete;
        PurchaseMutation& operator=(const PurchaseMutation&) = delete;
    private:
        uint32_t actor;
    };
}
#endif
