#pragma once
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
#include "LivingAuctionQuote.h"
#include "LivingProfessionJob.h"

namespace LivingActivity {
struct NativeAuctionOffer {uint32_t id=0,entry=0,quantity=0,copper=0,seller=0;};
// Existing AH policy/index, IDs only. Actual listings are re-read at dispatch.
bool NativeAuctionOffers(Player& actor,uint32_t entry,uint32_t maximum,
    std::vector<NativeAuctionOffer>& offers,std::string& blocker,uint64_t auctioneer=0);
bool ValidateNativeAuctionBudget(Player& actor,const Task& task,const std::string& operation,
    uint32_t copper,uint32_t seller,std::string& blocker);
bool InspectNativeAuctionQuote(Player& actor,uint64_t auctioneer,uint32_t auction,
    NativeAuctionQuote& quote,std::string& blocker);
bool PlanNativeAuctionPurchase(Player& actor,const Task& task,const ProfessionReagent& wanted,
    NativeAuctionQuote& quote,std::string& blocker);
bool NativeAuctionSourceAvailable(Player& actor,uint32_t entry,uint32_t maximum,std::string& blocker);
class NativeAuctionMoneyReservation final : public NativeReservationAdapter {
public:
    NativeAuctionMoneyReservation(NativeAuctionQuote quote,std::string operation)
        : quote(std::move(quote)),operation(std::move(operation)) {}
    bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) override;
private:
    NativeAuctionQuote quote;std::string operation;
};
class NativeAuctionPurchase final : public NativeOperationAdapter {
public:
    explicit NativeAuctionPurchase(NativeAuctionQuote quote) : quote(std::move(quote)) {}
    const char* OperationKind() const override {return "auction_purchase";}
    uint32_t OperationEffects() const override {return Mask(Effect::Money)|Mask(Effect::Inventory);}
    bool SupportsClaimedConsumption() const override {return true;}
    bool SupportsMailGain() const override {return true;}
    std::vector<uint32_t> RelatedActors() const override {return quote.bidder ? std::vector<uint32_t>{quote.seller,quote.bidder} : std::vector<uint32_t>{quote.seller};}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool PrepareDispatch(Player& actor,const OperationRequest& request,std::string& blocker) override;
    bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
    NativeObservation ExecuteNative(Player& actor,const OperationRequest& request) override;
    std::string PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const override;
private:
    NativeAuctionQuote quote;
    std::vector<AuctionMail> captured;
};
}
