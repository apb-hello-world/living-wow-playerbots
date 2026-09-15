#pragma once
#include "LivingProfessionJob.h"
#include "LivingActivityResources.h"
#include "LivingProfessionEvidence.h"

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
bool PrepareInterruptedProfession(const Task& saved,const WorldContext& current,
    const ProfessionHistory&,const UnsettledClaimBatch&,const CraftFrame&,
    uint64_t nowMs,const std::string& receipt,ProfessionPreparation&,std::string& blocker,
    const std::vector<NativeItemStack>& preservedBank={},const EnchantSubject* subject=nullptr,
    const std::vector<NativeItemStack>& preservedBags={});
struct CapacityRestoreSnapshot {
    NativeItemStack item;
    uint16_t position=0;
    uint32_t money=0,entryCount=0,totalCount=0,destinationBag=0;
    bool destinationEmpty=false;
};
bool PrepareInterruptedCapacity(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,const CapacityRestoreSnapshot&,
    uint64_t now,const std::string& receipt,
    ProfessionPreparation&,std::string&);
}
