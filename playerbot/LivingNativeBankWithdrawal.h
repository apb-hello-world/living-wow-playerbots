#pragma once
#include "LivingActivityOperations.h"
#include "LivingProfessionJob.h"
#include "LivingActivityReservations.h"
class Player;
namespace LivingActivity {
struct NativeBankQuote {
    uint32_t actor=0,guid=0,entry=0,quantity=0,bankerEntry=0;
    uint64_t banker=0;
    uint16_t from=0,to=0;
    uint32_t bagBefore=0,totalBefore=0;
    uint32_t mergeGuid=0,mergeCount=0;
    bool deposit=false;
    uint32_t moneyBefore=0;
};
uint64_t NativeNearbyBanker(Player& actor);
std::string EncodeNativeBankQuote(const NativeBankQuote& quote);
bool DecodeNativeBankQuote(const std::string& value,NativeBankQuote& quote);
bool PlanNativeBankWithdrawal(Player& actor,const Task& task,const ProfessionReagent& need,
    NativeBankQuote& quote,std::string& blocker);
bool PlanNativeBankDeposit(Player& actor,const Task& task,NativeBankQuote& quote,
    ResourceClaim& existing,std::string& blocker);
class NativeCapacityBankReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeBankTransfer : public NativeOperationAdapter {
public:
    const char* OperationKind() const override {return deposit ? "bank_deposit" : "bank_withdraw";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory);}
    bool SupportsItemTransfer() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
    NativeObservation ExecuteNative(Player& actor,const OperationRequest& request) override;
    std::string PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const override;
protected:
    explicit NativeBankTransfer(NativeBankQuote value,bool storing):quote(std::move(value)),deposit(storing) {}
    NativeBankQuote quote;
    const bool deposit;
};
class NativeBankWithdrawal final : public NativeBankTransfer {
public:
    explicit NativeBankWithdrawal(NativeBankQuote quote):NativeBankTransfer(std::move(quote),false) {}
};
class NativeBankDeposit final : public NativeBankTransfer {
public:
    explicit NativeBankDeposit(NativeBankQuote quote):NativeBankTransfer(std::move(quote),true) {}
};
}
