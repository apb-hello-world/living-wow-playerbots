#pragma once
#include "LivingCommissionTradeEvidence.h"
#include "LivingProfessionEvidence.h"
#include "LivingProfessionResume.h"

namespace LivingActivity {
// Only an acknowledged, digest-bound native journal can prove delivery after
// restart. Current inventory totals or a vanished trade window cannot do so.
bool DecodeStoredCommissionTrade(const Task&,const StoredCraftOperation&,CommissionTradeQuote&,std::string&);
bool PrepareCommissionTradeSettlement(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,uint64_t,const std::string&,ProfessionPreparation&,std::string&);
// Restore only verified-craft custody, never an uncertain exchange or payment.
bool PrepareCommissionTradeReadyRestore(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,
    uint64_t,const std::string&,ProfessionPreparation&,std::string&);
}
