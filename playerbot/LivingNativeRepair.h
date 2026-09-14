#pragma once
#include "LivingRepairQuote.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
namespace LivingActivity {
bool HasNativeCriticalRepair(Player& actor);
bool PlanNativeCriticalRepair(Player&,const Task&,NativeRepairQuote&,ResourceClaim&,std::string&);
class NativeRepairReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeCriticalRepair final : public NativeOperationAdapter {
public:
    explicit NativeCriticalRepair(NativeRepairQuote value):quote(value) {}
    const char* OperationKind() const override {return "critical_equipment_repair";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Equipment);}
    bool SupportsClaimedConsumption() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    NativeRepairQuote quote;
};
}
