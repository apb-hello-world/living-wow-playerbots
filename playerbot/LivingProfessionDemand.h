#ifndef LIVING_PROFESSION_DEMAND_H
#define LIVING_PROFESSION_DEMAND_H
#include "LivingProfessionJob.h"
#include "LivingNativeVendorPurchase.h"
class Player;
namespace LivingActivity {
    struct NativeProfessionDemand {
        std::vector<ProfessionStock> stock;
        std::string blocker;
        uint64_t nativeReference=0;
    };
    // Fresh world-thread inventory and incoming native references. Unknown
    // incoming mail is a collection/reconciliation blocker, NOT a guessed
    // recipe assignment. No database query or native mutation occurs here.
    bool InspectNativeProfessionDemand(Player& actor,const Task& saved,NativeProfessionDemand& demand);
    // Reuses the existing native economy policy and AI discretionary-money
    // calculation; implemented beside the transitional legacy auction adapter.
    bool ValidateNativeProfessionBudget(Player& actor,const Task& saved,const std::string& operation,
        uint32_t price,std::string& blocker);
    class NativeProfessionPurchasePrerequisites final : public NativePurchasePrerequisites {
    public:
        bool ValidateCommittedDemandAndBudget(Player& actor,const OperationRequest& request,
            const NativeVendorQuote& quote,std::string& blocker) const override;
    };
}
#endif
