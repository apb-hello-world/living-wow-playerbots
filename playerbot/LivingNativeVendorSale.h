#pragma once
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
#include "LivingCapacityPreparation.h"
class Item;
namespace LivingActivity {
struct ProfessionJob;
bool NativeCapacityNeed(Player& actor,const Task& task,const ProfessionJob& job,
    const UnsettledClaimBatch& claims,ItemGainSpec& need,std::string& blocker);
bool NativeCapacityItemProtected(Player& actor,const ProfessionJob& job,Item& item);
struct NativeSaleQuote {
    CapacitySaleFacts item;
    uint32_t copper=0,countBefore=0,vendorEntry=0,capacityEntry=0,capacityQuantity=0;
    uint64_t vendor=0;
    uint16_t from=0;
};
std::string EncodeNativeSaleQuote(const NativeSaleQuote& quote);
bool DecodeNativeSaleQuote(const std::string& value,NativeSaleQuote& quote);
// Read-only native preparation. A missing safe candidate never authorizes a
// destructive fallback, bank contents remain remote until normal collection.
bool PlanNativeCapacitySale(Player& actor,const Task& task,NativeSaleQuote& quote,
    ResourceClaim& existing,std::string& blocker);
class NativeCapacityReservation final : public NativeReservationAdapter {
public:
    bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
};
class NativeVendorSale final : public NativeOperationAdapter {
public:
    explicit NativeVendorSale(NativeSaleQuote value):quote(std::move(value)) {}
    const char* OperationKind() const override {return "capacity_vendor_sale";}
    uint32_t OperationEffects() const override {return Mask(Effect::Inventory)|Mask(Effect::Money);}
    bool SupportsClaimedConsumption() const override {return true;}
    NativePersistence PersistencePolicy() const override {return NativePersistence::Inventory;}
    bool ValidateNative(Player&,const OperationRequest&,std::string&) override;
    NativeObservation ExecuteNative(Player&,const OperationRequest&) override;
    std::string PersistedNativeProof(Player&,const OperationRequest&,const Task&) const override;
private:
    NativeSaleQuote quote;
};
}
