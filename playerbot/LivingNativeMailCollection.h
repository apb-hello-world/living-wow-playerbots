#pragma once
#include "LivingActivityOperations.h"
class Player;
namespace LivingActivity {
struct NativeMailQuote {
    uint32_t actor=0,mail=0,guid=0,entry=0,quantity=0,mailboxEntry=0;
    uint64_t mailbox=0,deliveredAt=0,expiresAt=0;
    uint32_t moneyBefore=0,mailMoney=0,attachmentsBefore=0,bagBefore=0,totalBefore=0;
    uint16_t to=0;
};
// Identity inspection does not grant permission to collect, guess a recipe, or
// count a mail attachment as bag stock. Only acknowledged task claims link it.
bool ReadNativeMailBalance(Player& actor,const ResourceClaim& claim,NativeResourceBalance& balance);
uint64_t NativeNearbyMailbox(Player& actor);
std::string EncodeNativeMailQuote(const NativeMailQuote& quote);
bool DecodeNativeMailQuote(const std::string& value,NativeMailQuote& quote);
bool PlanNativeMailCollection(Player& actor,const Task& task,const ResourceClaim& claim,
    NativeMailQuote& quote,std::string& blocker);
class NativeMailCollection final : public NativeOperationAdapter {
public:
    explicit NativeMailCollection(NativeMailQuote quote):quote(std::move(quote)) {}
    const char* OperationKind() const override {return "mail_collect";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory);}
    bool SupportsItemTransfer() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
    NativeObservation ExecuteNative(Player& actor,const OperationRequest& request) override;
    std::string PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const override;
private:
    NativeMailQuote quote;
};
}
