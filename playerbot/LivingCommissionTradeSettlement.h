#pragma once
#include "LivingCommissionTradeEvidence.h"
#include "LivingProfessionEvidence.h"
#include "LivingProfessionResume.h"
#include "LivingCommissionPartition.h"

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
// Only a process-restored non-consuming offer may be rejected without a new
// native operation. Exact output custody remains held; no delivery is inferred.
bool PrepareInterruptedCommissionOffer(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,
    uint64_t,const std::string&,ProfessionPreparation&,std::string&);
// A pending inventory split is repeatable only after exact unchanged native
// source, destination, wallet and claims prove that its atomic save did not run.
bool DecodeStoredCommissionPartition(const Task&,const StoredCraftOperation&,CommissionPartitionQuote&,uint32_t&,std::string&);
bool DecodeInterruptedCommissionPartition(const Task&,const StoredCraftOperation&,CommissionPartitionQuote&);
struct CommissionPartitionRestoreState {
    uint32_t money=0,destinationBag=0;
    NativeItemStack source;
    bool destinationEmpty=false;
};
bool PrepareInterruptedCommissionPartition(const Task&,const WorldContext&,const ProfessionHistory&,
    const UnsettledClaimBatch&,const std::vector<NativeResourceBalance>&,const CommissionPartitionRestoreState&,
    uint64_t,const std::string&,ProfessionPreparation&,std::string&);
}
