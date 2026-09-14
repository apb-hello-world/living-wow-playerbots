#pragma once
#include "LivingGatherQuote.h"
#include "LivingProfessionEvidence.h"
#include "LivingGuildProcurementRecovery.h"
namespace LivingActivity {
// A completed OPEN_LOCK/SKINNING cast is not an item acquisition. After an
// actual restart its volatile loot may be gone. Preserve the old observation
// and resume the same obligation only with exact zero-item/current-state proof.
bool DecodeInterruptedGather(const Task&,const StoredCraftOperation&,NativeGatherResult&,std::string&);
bool PrepareInterruptedGather(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const NativeGatherResult&,uint64_t,const std::string&,
    GuildProcurementRecovery&,std::string&);
}
