#pragma once
#include "LivingCommissionMail.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
namespace LivingActivity {
bool PlanNativeCommissionMail(Player&,const Task&,CommissionMailQuote&,std::vector<ClaimConsumption>&,std::string&);
bool NativeCommissionAdditionalItemsUnchanged(Player&,const CommissionMailQuote&);
class NativeCommissionReturnReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
    std::string PersistedPurposeGuard() const override {return guard;}
private:
    std::string guard=" AND 1=0";
};
class NativeCommissionMailReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeCommissionMail final : public NativeOperationAdapter {
public:
    explicit NativeCommissionMail(CommissionMailQuote quote):quote(std::move(quote)) {}
    const char* OperationKind() const override {return "commission_mail_send";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money);}
    bool SupportsClaimedConsumption() const override {return true;}
    bool SupportsCommissionMailSend() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    std::vector<uint32_t> RelatedActors() const override {return {quote.receiver};}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    CommissionMailQuote quote;
    AuctionMail sent;
    std::string operation;
    uint32_t surplusItem=0;
};
}
