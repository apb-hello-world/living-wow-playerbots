#ifndef LIVING_ACTIVITY_COORDINATOR_H
#define LIVING_ACTIVITY_COORDINATOR_H
#include <memory>
#include <string>
#include <cstdint>
#include "LivingActivityEffects.h"
#include "LivingActivity.h"
#include "LivingActivityRequests.h"
#include "LivingActivityAuthority.h"
#include "LivingActivityOperations.h"
#include "LivingActivityReservations.h"
#include "LivingActivityAcquisition.h"
#include "LivingPurchaseBudget.h"
#include "LivingProfessionEvidence.h"
#include <optional>
class PlayerbotAI;
class WorldPacket;

class LivingActivityCoordinator {
public:
    static LivingActivityCoordinator& instance();
    // Called by the existing world update. Persistence uses the existing
    // CharacterDatabase delay/result queues, not a worker/timer per bot.
    void Update();
    std::string StatusJson() const;
    std::string ActorJson(uint32_t guid) const;
    // Map-worker entry point: bounded immutable diagnostics only. Does not read
    // the coordinator's task/lease maps and never authorizes a native effect.
    void ObserveAction(PlayerbotAI& ai, const LivingActivity::Effects& effects, const std::string& action);
    // The same boundary serves engine calls and direct native mutations. Until
    // the cutover gate is accepted it records decisions without rejecting work.
    bool PermitEffects(PlayerbotAI& ai, const LivingActivity::Effects& effects, const std::string& origin);
    // Value-only current context for a compiled native validator. This alone
    // grants nothing; the validator must establish native eligibility and the
    // common effect boundary rechecks scope, safety and pending operations.
    LivingActivity::NativePermit NativeActionContext(PlayerbotAI& ai, LivingActivity::Lane lane,
        uint32_t effects, uint32_t allowedSafety) const;
    enum class LeaseBoundary { Acquire, Renew, Release };
    void ObserveLeaseBoundary(uint32_t guid, LeaseBoundary boundary);
    // Transitional callers keep exact job handles while their executors are
    // migrated. This supplies identity only, NEVER an execution permission.
    // Native resolution is world-thread-only; map callers must defer intent.
    bool CompatibilityContext(uint32_t guid, const std::string& source,
        const std::string& key, LivingActivity::ActivityLease& identity) const;
    bool OnWorldThread() const;
    bool EffectEnforcementEnabled() const;
    // Read-only native full-save guard. The actor-ID projection survives AI
    // object replacement; no Player/Map pointers or DB query cross threads.
    bool DefersNativeSave(uint32_t actor) const;
    struct CompatibilityLease {
        LivingActivity::ActivityLease handle;
        std::string owner,phase,reason;
        uint64_t expiresMs=0;
    };
    // The old manager keeps only domain intent. These methods use the SAME
    // ExecutionAuthority as saved tasks; a legacy handle is never a task grant.
    LivingActivity::Acquisition AcquireCompatibilityLease(uint32_t actor,const std::string& owner,
        const std::string& phase,uint32_t ttlSeconds,const std::string& reason,
        const std::string& jobKey,LivingActivity::ActivityLease& handle);
    bool RenewCompatibilityLease(const LivingActivity::ActivityLease& handle,const std::string& phase,
        uint32_t ttlSeconds,const std::string& reason);
    bool ReleaseCompatibilityLease(const LivingActivity::ActivityLease& handle);
    std::optional<CompatibilityLease> ReadCompatibilityLease(uint32_t actor) const;
    std::vector<CompatibilityLease> CompatibilityLeases() const;
    // Trusted domain producers only, on the native world thread. No native
    // operation or lease is started by submission or persistence callbacks.
    LivingActivity::AdmissionResult SubmitTask(const LivingActivity::TaskRequest& request);
    LivingActivity::AdmissionResult SubmitResourceReservation(const LivingActivity::ReservationRequest& request,
        LivingActivity::NativeReservationAdapter& adapter);
    // Read-only immutable projection. Never permission to consume a reserved
    // item; a journalled service adapter must prove its own claim separately.
    LivingActivity::ResourceReader ResourceReservations() const;
    std::optional<LivingActivity::Task> ReadSavedTask(const std::string& id) const;
    bool TaskResourceAvailability(const std::string& task,uint64_t revision,
        const LivingActivity::NativeResourceBalance& native,uint32_t& available,std::string& blocker) const;
    bool PurchaseLedgerReady() const;
    // Due-queued read of the common native spend ledger. This is NOT authority
    // to buy; the compiled adapter still validates its saved task, claims,
    // demand, quote and effect grant immediately before the native mutation.
    bool ReadPurchaseBudget(uint32_t actor,const std::string& task,uint64_t revision,
        const std::string& currentOperation,LivingActivity::PurchaseSpend& spend,std::string& blocker);
    // Acknowledged, revision-bound craft history. Decoded incrementally through
    // the existing world/DB queue; this neither grants a lease nor resumes work.
    bool ReadProfessionHistory(uint32_t actor,const std::string& task,uint64_t revision,
        LivingActivity::ProfessionHistory& history,std::string& blocker);
    struct TaskGrant {
        LivingActivity::AuthorityResult authority;
        LivingActivity::Task task;
        LivingActivity::ActionContext action;
        std::string blocker;
        bool Permitted() const { return authority.Granted() || authority.code == LivingActivity::AuthorityCode::Allowed; }
    };
    // Read the acknowledged cache, not a planner's copy. The caller must handle
    // any returned displaced owner before performing its finite native step.
    TaskGrant AcquireSavedTask(const std::string& task, uint64_t revision,
        uint32_t effects, uint64_t durationMs, const std::string& origin);
    // A saved finite step shares the root lease. Empty step selects the saved
    // parent again. No lease acquisition/priority contest or implicit scope.
    TaskGrant SelectSavedStep(const LivingActivity::ActivityLease& lease,
        const std::string& step, uint64_t revision, uint32_t effects, const std::string& origin);
    LivingActivity::AuthorityResult ReleaseTaskLease(const LivingActivity::ActivityLease& lease);
    LivingActivity::AdmissionResult SubmitOperationIntent(const LivingActivity::OperationRequest& request,
        LivingActivity::NativeOperationAdapter& adapter);
    LivingActivity::DispatchResult DispatchSavedOperation(const std::string& operation,
        const TaskGrant& grant, LivingActivity::NativeOperationAdapter& adapter);
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    // Test binary only: hold ONE fixture bot's decisions while native Unit/
    // Spell updates continue. No live-environment or configuration path.
    bool IsolatedGameplayActor(uint32_t actor) const;
    void ObserveIsolatedGameplayPacket(PlayerbotAI& ai, const WorldPacket& packet);
#endif
private:
    LivingActivityCoordinator();
    ~LivingActivityCoordinator();
    struct State;
    std::unique_ptr<State> state;
    void RefreshPermission(uint32_t guid, uint64_t actorEpoch);
    bool CollectNativeCraft();
    LivingActivity::DispatchResult FinalizeNativeOperation(const std::string& operation,Player& actor,
        LivingActivity::NativeObservation observation,std::vector<LivingActivity::VerifiedItemGain> gains,
        bool nativeTransactionOpen,bool executed,const LivingActivity::NativeOperationAdapter* adapter=nullptr);
    void RunIsolatedBoundaryFixture();
    void RunIsolatedAdmissionFixture();
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    void RunIsolatedGameplayFixture();
    void RunIsolatedCombatFixture();
    void RunIsolatedPetFixture();
    void RunIsolatedVendorFixture();
    void RunIsolatedCraftFixture();
#endif
};
#define sLivingActivityCoordinator LivingActivityCoordinator::instance()
#endif
