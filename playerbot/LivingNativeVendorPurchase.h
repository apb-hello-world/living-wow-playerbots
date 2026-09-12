#ifndef LIVING_NATIVE_VENDOR_PURCHASE_H
#define LIVING_NATIVE_VENDOR_PURCHASE_H
#include "LivingActivityOperations.h"
#include "LivingVendorQuote.h"
#include <utility>

namespace LivingActivity {
    bool InspectNativeVendorQuote(Player& actor,uint64_t vendor,uint32_t entry,uint32_t quantity,
        NativeVendorQuote& quote,std::string& blocker);

    // A required compiled producer boundary. The native purchase mechanism is
    // not its own planner or allowance factory. Its caller must revalidate the
    // exact unpaid demand, incoming orders, policy, discretionary budget and
    // existing reservations at BOTH intent admission and actual dispatch.
    // There is deliberately no permissive default or model/RPC implementation.
    class NativePurchasePrerequisites {
    public:
        virtual ~NativePurchasePrerequisites() = default;
        virtual bool ValidateCommittedDemandAndBudget(Player& actor,const OperationRequest& request,
            const NativeVendorQuote& quote,std::string& blocker) const = 0;
    };
    class NativeVendorPurchase final : public NativeOperationAdapter {
    public:
        NativeVendorPurchase(NativeVendorQuote quote,const NativePurchasePrerequisites& prerequisites)
            : quote(std::move(quote)), prerequisites(prerequisites) {}
        const char* OperationKind() const override { return "vendor_purchase"; }
        uint32_t OperationEffects() const override { return Mask(Effect::Money)|Mask(Effect::Inventory); }
        bool SupportsClaimedConsumption() const override { return true; }
        bool SupportsItemGain() const override { return true; }
        NativePersistence PersistencePolicy() const override { return NativePersistence::Inventory; }
        bool ValidateNative(Player& actor,const OperationRequest& request,std::string& blocker) override;
        bool PrepareDispatch(Player& actor,const OperationRequest& request,std::string& blocker) override;
        NativeObservation ExecuteNative(Player& actor,const OperationRequest& request) override;
        std::string PersistedNativeProof(Player& actor,const OperationRequest& request,const Task& outcome) const override;
    private:
        NativeVendorQuote quote; // Value-only; never retain a Creature/Item/Player pointer.
        const NativePurchasePrerequisites& prerequisites; // Synchronous adapter lifetime only.
    };
}
#endif
