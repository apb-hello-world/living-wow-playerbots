#pragma once
#include "LivingPartyAuction.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
namespace LivingActivity {
bool PlanNativePartyAuctionBatch(Player&,PartyAuctionJob&,std::string&);
bool PlanNativePartyAuctionPost(Player&,const Task&,AuctionPostQuote&,std::vector<ClaimConsumption>&,std::string&);
class NativeAuctionPostReservation final:public NativeReservationAdapter {
public: bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeAuctionPost final:public NativeOperationAdapter {
public:
    explicit NativeAuctionPost(AuctionPostQuote value):quote(std::move(value)){}
    const char* OperationKind() const override{return "party_auction_post";}
    uint32_t OperationEffects() const override{return Mask(Effect::Inventory)|Mask(Effect::Money);}
    bool SupportsClaimedConsumption() const override{return true;}
    NativePersistence PersistencePolicy() const override{return NativePersistence::Inventory;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    AuctionPostQuote quote;
    AuctionPostReceipt receipt;
};
}
