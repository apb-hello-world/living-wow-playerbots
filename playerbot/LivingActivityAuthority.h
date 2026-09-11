#ifndef LIVING_ACTIVITY_AUTHORITY_H
#define LIVING_ACTIVITY_AUTHORITY_H
#include "LivingActivity.h"
#include "LivingActivityEffects.h"
#include <map>

namespace LivingActivity {
    enum class AuthorityCode {
        Granted, Renewed, Preempted, Released, NoOwner, Allowed,
        InvalidRequest, StaleContext, StaleLease, StaleRevision, SafetyPaused,
        AtomicPending, PriorityDenied, EffectsDenied, UnknownAction,
        ReconciliationRequired, Capacity, GenerationExhausted
    };
    const char* Name(AuthorityCode code);
    struct AuthorityResult {
        AuthorityCode code = AuthorityCode::InvalidRequest;
        ActivityLease lease, displaced;
        bool Granted() const;
    };
    // Copied by the world owner. Workers never receive its mutable lease book.
    struct AuthoritySnapshot {
        WorldContext current;
        uint32_t safety = 0, effects = 0;
        Task root;
        // One world-approved finite child step. Merely knowing a root lease
        // must not let a stale/unregistered child impersonate its preparation.
        Task step;
        ActivityLease lease;
        uint64_t expires = 0;
        std::string operation;
        bool operationDispatched = false, operationExecuting = false;
        bool invalidated = false;
        // Transitional movement ownership shares THIS lease book. Its native
        // work is still owned by a legacy executor and cannot impersonate an
        // acknowledged durable task or enter a journalled operation.
        bool compatibility = false;
        std::string compatibilityPhase, compatibilityReason;
    };

    // A world-thread-owned lease book, NOT a second scheduler or task store.
    // The coordinator owns the durable queue. Producers supply already validated
    // root tasks; child steps inherit that root rather than competing with it.
    // Every displacement is returned exactly once for the coordinator's handoff.
    class ExecutionAuthority {
    public:
        explicit ExecutionAuthority(size_t actorLimit = 20000) : limit(actorLimit) {}
        AuthorityResult Observe(const WorldContext& current, uint32_t safety);
        AuthorityResult Acquire(const Task& root, uint32_t effects, uint64_t now, uint64_t duration);
        AuthorityResult AcquireCompatibility(const Task& root, uint64_t now, uint64_t duration);
        bool DescribeCompatibility(const ActivityLease& lease,const std::string& phase,const std::string& reason);
        AuthorityResult Release(const ActivityLease& lease);
        AuthorityCode SelectStep(const ActivityLease& lease, const Task* step);
        AuthorityResult Forget(uint32_t actor);
        AuthorityResult BeginAtomic(const ActivityLease& lease, const std::string& operation, uint64_t now);
        // World-only executor boundary, after an exact persisted intent receipt.
        // Never used to automatically replay a restored/uncertain operation.
        AuthorityResult BeginDispatch(const ActivityLease& lease, const std::string& operation, uint64_t now);
        AuthorityResult EndDispatch(const ActivityLease& lease, const std::string& operation);
        AuthorityResult FinishAtomic(const ActivityLease& lease, const std::string& operation);
        AuthorityResult Inspect(uint32_t actor, uint64_t now) const;
        AuthoritySnapshot Read(uint32_t actor) const;
        static AuthorityCode Check(const AuthoritySnapshot& snapshot, const Effects& effects,
            const WorldContext& current, uint64_t now, const Task* task = nullptr,
            const ActionContext* action = nullptr, const NativePermit* permit = nullptr, uint32_t nativeSafety = 0);
        AuthorityCode Authorize(const Effects& effects, const WorldContext& current, uint64_t now,
            const Task* task = nullptr, const ActionContext* action = nullptr,
            const NativePermit* permit = nullptr) const;
        size_t Size() const { return actors.size(); }
        // Structural validation for value-only journal/callback identities.
        // This does not inspect current ownership or grant any native effect.
        static bool ContextValid(const WorldContext& context);
    private:
        using Actor = AuthoritySnapshot;
        static bool Matches(const ActivityLease& left, const ActivityLease& right);
        static bool Executable(const Task& task);
        static bool SameDefinition(const Task& left, const Task& right);
        static uint32_t LaneEffects(Lane lane);
        static AuthorityResult Drop(Actor& actor, AuthorityCode code);
        AuthorityResult AcquireImpl(const Task& root,uint32_t effects,uint64_t now,uint64_t duration,bool compatibility);
        std::map<uint32_t, Actor> actors;
        size_t limit;
        uint64_t generation = 0;
    };
}
#endif
