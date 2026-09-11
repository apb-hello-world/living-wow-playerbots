#ifndef LIVING_PROFESSION_SETTLEMENT_H
#define LIVING_PROFESSION_SETTLEMENT_H
#include "LivingProfessionJob.h"
#include "LivingActivityResources.h"

namespace LivingActivity {
    struct ProfessionSettlement {
        Task task;
        WritePlan plan;
        std::vector<ClaimReceiptChange> claims;
    };
    // Finite, receipt-acknowledged bookkeeping after a proven skill-gain job.
    // Releases only backed personal stock/speculative demand; never consumes,
    // transfers, withdraws, sells or destroys goods. Requested/equipment output
    // and intermediate handoffs require their own recipient validators.
    // More than 16 claims settle in bounded batches while remaining verifying.
    bool PrepareProfessionSettlement(const Task& before,const ProfessionSnapshot& snapshot,
        const UnsettledClaimBatch& batch,const std::vector<NativeResourceBalance>& balances,
        uint64_t nowMs,const std::string& receipt,ProfessionSettlement& result,std::string& blocker);
}
#endif
