#pragma once
#include "LivingGuildMailHandoff.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
namespace LivingActivity {
bool PlanNativeGuildMail(Player&,const Task&,GuildMailQuote&,std::vector<ClaimConsumption>&,
    std::string&,uint32_t selectedReceiver=0);
class NativeGuildSendReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeGuildMail final : public NativeOperationAdapter {
public:
    explicit NativeGuildMail(GuildMailQuote q):quote(std::move(q)) {}
    const char* OperationKind() const override {return "guild_mail_send";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Guild);}
    bool SupportsClaimedConsumption() const override {return true;}
    bool SupportsGuildMailHandoff() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    std::vector<uint32_t> RelatedActors() const override {return {quote.receiver};}
    uint32_t RelatedGuild() const override {return quote.job.guild;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    GuildMailQuote quote;
    NativeResourceBalance sent;
    uint64_t deliveredAt=0,expiresAt=0;
};
}
