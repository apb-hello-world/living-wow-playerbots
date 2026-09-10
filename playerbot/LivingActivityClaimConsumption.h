#ifndef LIVING_ACTIVITY_CLAIM_CONSUMPTION_H
#define LIVING_ACTIVITY_CLAIM_CONSUMPTION_H
#include "LivingActivityResources.h"
namespace LivingActivity {
    // A compiled native adapter supplies the exact held claim and the quantity
    // its operation will consume. This is not proof that consumption occurred.
    struct ClaimConsumption { ResourceClaim before; uint32_t used = 0; };
    struct ClaimedOutcome { WritePlan journal; std::vector<ClaimReceiptChange> changes; };
    // Used in the intent BEFORE execution, and in the verified outcome. The
    // journal binds identical claim identities/revisions/quantities to both.
    std::string ClaimedNativeState(const std::string& nativeState,
        const std::vector<ClaimConsumption>& consumption, size_t limit = 4096);
    // Only successful native outcomes may settle a consumed claim. Rejection
    // or uncertainty retains protection and goes through normal reconciliation.
    ClaimedOutcome ConsumedOperationWrite(const Task& task, uint64_t expected,
        const OperationResult& result, const std::string& receipt,
        const std::string& nativeAfter, const std::vector<ClaimConsumption>& consumption);
}
#endif
