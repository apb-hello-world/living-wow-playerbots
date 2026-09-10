#ifndef LIVING_ACTIVITY_OPERATIONS_H
#define LIVING_ACTIVITY_OPERATIONS_H
#include "LivingActivityRequests.h"
#include "LivingActivityEffects.h"
#include "LivingActivityClaimConsumption.h"
class Player;
namespace LivingActivity {
    struct OperationRequest {
        TaskRequest transition; // Preparing/traveling -> executing, receipt is operation ID.
        ActionContext authorization; // Exact saved predecessor's scoped authority.
        std::string kind, beforeState = "{}";
        uint32_t effects = 0;
        std::vector<ClaimConsumption> consumption;
    };
    struct NativeObservation {
        OperationState state = OperationState::Reconciling;
        std::string nativeReference, evidence = "native_outcome_uncertain", afterState = "{}";
    };
    // Finite, compiled native service adapters. Never a model/RPC callback,
    // script interpreter, or pointer retained across asynchronous persistence.
    class NativeOperationAdapter {
    public:
        virtual ~NativeOperationAdapter() = default;
        virtual const char* OperationKind() const = 0;
        virtual uint32_t OperationEffects() const = 0;
        // Gains and transfers require their own native identity adapters. They
        // are not disguised as consumption or inferred from an effect bit.
        virtual bool SupportsClaimedConsumption() const { return false; }
        virtual bool ValidateNative(Player& actor, const OperationRequest& request, std::string& blocker) = 0;
        virtual NativeObservation ExecuteNative(Player& actor, const OperationRequest& request) = 0;
    };
    bool ValidateOperationRequest(const OperationRequest& request, const Task& saved,
        const WorldContext& current, const Task* root, uint64_t wallNow, std::string& blocker);
    // Effects are fingerprinted with native before-state, not transient hints.
    WritePlan OperationRequestWrite(const OperationRequest& request);
    bool ValidateNativeObservation(const NativeObservation& result);
    bool ValidateOperationResources(const OperationRequest& request, const ResourceClaimBook& claims,
        const std::vector<NativeResourceBalance>& balances, std::string& blocker);
    bool VerifyConsumedNativeResources(const OperationRequest& request,
        const std::vector<NativeResourceBalance>& before, const std::vector<NativeResourceBalance>& after,
        std::string& blocker);
    struct DispatchResult {
        AdmissionResult admission;
        bool executed = false, outcomeQueued = false;
    };
}
#endif
