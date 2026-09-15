#pragma once
#include "LivingCommissionTradeContract.h"
#include "LivingCommissionTradeEvidence.h"
#include "LivingActivityOperations.h"

namespace LivingActivity {
// Finite final acceptance only. Preparing an offer and travelling to its
// recipient remain separate saved steps; this adapter never edits consent.
bool PlanNativeCommissionTrade(Player&,const Task&,CommissionTradeQuote&,
    std::vector<ClaimConsumption>&,std::string&);
class NativeCommissionTrade final : public NativeOperationAdapter {
public:
    explicit NativeCommissionTrade(CommissionTradeQuote quote):quote(std::move(quote)) {}
    const char* OperationKind() const override {return "commission_trade";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money);}
    bool SupportsClaimedConsumption() const override {return true;}
    bool SupportsCommissionTrade() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    std::vector<uint32_t> RelatedActors() const override {return {quote.recipient};}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    CommissionTradeQuote quote;
    struct Location {uint32_t item=0,count=0,bag=0;uint8_t slot=0;};
    std::vector<Location> delivered;
    bool verified=false,unchanged=false;
};
}
