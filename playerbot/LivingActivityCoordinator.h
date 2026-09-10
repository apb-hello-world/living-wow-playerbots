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
    // Trusted domain producers only, on the native world thread. No native
    // operation or lease is started by submission or persistence callbacks.
    LivingActivity::AdmissionResult SubmitTask(const LivingActivity::TaskRequest& request);
    LivingActivity::AdmissionResult SubmitResourceReservation(const LivingActivity::ReservationRequest& request,
        LivingActivity::NativeReservationAdapter& adapter);
    // Read-only immutable projection. Never permission to consume a reserved
    // item; a journalled service adapter must prove its own claim separately.
    LivingActivity::ResourceReader ResourceReservations() const;
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
    void RunIsolatedBoundaryFixture();
    void RunIsolatedAdmissionFixture();
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    void RunIsolatedGameplayFixture();
#endif
};
#define sLivingActivityCoordinator LivingActivityCoordinator::instance()
#endif
