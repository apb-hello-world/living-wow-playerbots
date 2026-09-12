#ifndef LIVING_ACTIVITY_OPERATIONS_H
#define LIVING_ACTIVITY_OPERATIONS_H
#include "LivingActivityRequests.h"
#include "LivingActivityEffects.h"
#include "LivingActivityClaimConsumption.h"
#include "LivingActivityItemGain.h"
#include <memory>
class Player;
namespace LivingActivity {
    class NativeCraftCast;
    enum class NativePersistence { JournalOnly, Inventory, Profession };
    struct OperationRequest {
        TaskRequest transition; // Preparing/traveling -> executing, receipt is operation ID.
        ActionContext authorization; // Exact saved predecessor's scoped authority.
        std::string kind, beforeState = "{}";
        uint32_t effects = 0;
        NativePersistence persistence = NativePersistence::JournalOnly;
        std::vector<ClaimConsumption> consumption;
        ItemGainSpec itemGain; // Exact output bound into intent before purchase.
        ResourceClaim itemTransfer; // Same GUID/quantity, bank or mail -> bags; never consumption.
    };
    struct NativeObservation {
        OperationState state = OperationState::Reconciling;
        std::string nativeReference, evidence = "native_outcome_uncertain", afterState = "{}";
        // Exact surviving native stack after a verified whole-stack transfer.
        // Count includes preexisting stock; only the transferred claim's own
        // quantity follows it. Never supplied by a model or inferred on restart.
        NativeResourceBalance transferredItem;
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
        virtual bool SupportsItemGain() const { return false; }
        virtual bool SupportsItemTransfer() const { return false; }
        virtual NativePersistence PersistencePolicy() const { return NativePersistence::JournalOnly; }
        // Compiled native after-state predicate, checked in the SAME transaction
        // as the native save and journal. No player/model SQL enters this API.
        virtual std::string PersistedNativeProof(Player&, const OperationRequest&, const Task&) const { return {}; }
        virtual bool ValidateNative(Player& actor, const OperationRequest& request, std::string& blocker) = 0;
        // A bounded asynchronous prerequisite may need a fresh read after
        // intent admission. False waits WITHOUT dispatching or recording a
        // rejected native effect. The exact native validator still runs later.
        virtual bool PrepareDispatch(Player&,const OperationRequest&,std::string&) { return true; }
        // Only the finite native crafting adapter uses a cross-update cast.
        // Reserve a value-only capture BEFORE launch; never retain this adapter
        // or its caller across ticks. Other native services remain synchronous.
        virtual bool DeferredNativeCast() const { return false; }
        virtual std::shared_ptr<NativeCraftCast> ReserveNativeCast(const OperationRequest&,
            const Task&,const ActionContext&) const { return {}; }
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
