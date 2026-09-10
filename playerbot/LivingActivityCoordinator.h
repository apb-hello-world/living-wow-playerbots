#ifndef LIVING_ACTIVITY_COORDINATOR_H
#define LIVING_ACTIVITY_COORDINATOR_H
#include <memory>
#include <string>
#include <cstdint>
#include "LivingActivityEffects.h"
#include "LivingActivity.h"
class PlayerbotAI;

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
    enum class LeaseBoundary { Acquire, Renew, Release };
    void ObserveLeaseBoundary(uint32_t guid, LeaseBoundary boundary);
    // Transitional callers keep exact job handles while their executors are
    // migrated. This supplies identity only, NEVER an execution permission.
    // Native resolution is world-thread-only; map callers must defer intent.
    bool CompatibilityContext(uint32_t guid, const std::string& source,
        const std::string& key, LivingActivity::ActivityLease& identity) const;
    bool OnWorldThread() const;
private:
    LivingActivityCoordinator();
    ~LivingActivityCoordinator();
    struct State;
    std::unique_ptr<State> state;
    void RefreshPermission(uint32_t guid, uint64_t actorEpoch);
    void RunIsolatedBoundaryFixture();
};
#define sLivingActivityCoordinator LivingActivityCoordinator::instance()
#endif
