#pragma once
#include "LivingGuildDeposit.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
namespace LivingActivity {
// Planning is read-only. Every permission, goal, stack and native service is
// revalidated at the operation boundary; cached proximity grants nothing.
bool PlanNativeGuildDeposit(Player& actor,const Task& task,GuildDepositQuote& quote,
    ResourceClaim& existing,std::string& blocker);
class NativeGuildDeliveryReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeGuildMailReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeGuildDeposit final : public NativeOperationAdapter {
public:
    explicit NativeGuildDeposit(GuildDepositQuote value):quote(std::move(value)) {}
    const char* OperationKind() const override {return "guild_bank_deposit";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Guild);}
    bool SupportsClaimedConsumption() const override {return true;}
    uint32_t RelatedGuild() const override {return quote.job.guild;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    GuildDepositQuote quote;
};
}
