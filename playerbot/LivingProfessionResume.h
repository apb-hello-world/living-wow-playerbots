#pragma once
#include "LivingProfessionJob.h"
#include "LivingActivityResources.h"

namespace LivingActivity {
struct ProfessionPreparation {
    Task task;
    WritePlan plan;
};
// Rebind an accepted, non-atomic job to freshly inspected native context.
// This is bookkeeping only: no new task, lease, claim, purchase or native
// effect. Every old claim and operation remains unchanged. Callers supply
// native snapshots and acknowledged history, never model assertions.
bool PrepareProfessionResumption(const Task& saved,const WorldContext& current,
    const ProfessionSnapshot& snapshot,const UnsettledClaimBatch& claims,
    const std::vector<NativeResourceBalance>& balances,uint64_t nowMs,
    const std::string& receipt,ProfessionPreparation& result,std::string& blocker);
}
