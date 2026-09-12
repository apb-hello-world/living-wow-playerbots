#pragma once
#include "LivingActivityOperations.h"
#include "LivingProfessionJob.h"
class Player;
namespace LivingActivity {
struct NativeBankQuote {
    uint32_t actor=0,guid=0,entry=0,quantity=0,bankerEntry=0;
    uint64_t banker=0;
    uint16_t from=0,to=0;
    uint32_t bagBefore=0,totalBefore=0;
    uint32_t mergeGuid=0,mergeCount=0;
};
uint64_t NativeNearbyBanker(Player& actor);
std::string EncodeNativeBankQuote(const NativeBankQuote& quote);
bool DecodeNativeBankQuote(const std::string& value,NativeBankQuote& quote);
bool PlanNativeBankWithdrawal(Player& actor,const Task& task,const ProfessionReagent& need,
    NativeBankQuote& quote,std::string& blocker);
class NativeBankWithdrawal final : public NativeOperationAdapter {
public:
    explicit NativeBankWithdrawal(NativeBankQuote quote):quote(std::move(quote)) {}
    const char* OperationKind() const override {return "bank_withdraw";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory);}
    bool SupportsItemTransfer() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
    NativeObservation ExecuteNative(Player& actor,const OperationRequest& request) override;
    std::string PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const override;
private:
    NativeBankQuote quote;
};
}
