#pragma once
#include "LivingActivityOperations.h"
#include "LivingLootQuote.h"

namespace LivingActivity {
// Current generated world loot only. The producer must separately obtain the
// node/corpse through native gameplay and an authorized activity commitment.
bool InspectNativeLootQuote(Player&,uint64_t source,uint32_t slot,NativeLootQuote&,std::string&);
class NativeLootCollection final : public NativeOperationAdapter {
public:
    explicit NativeLootCollection(NativeLootQuote quote):quote(quote) {}
    const char* OperationKind() const override {return "loot_collect";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory);}
    bool SupportsItemGain() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    NativeLootQuote quote; // No live Player/Loot/GameObject pointers across a tick.
};
}
