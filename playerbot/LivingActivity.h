#ifndef LIVING_ACTIVITY_H
#define LIVING_ACTIVITY_H

#include <cstdint>
#include <string>
#include <vector>

// Value-only contracts. No Player, Map, Item, or travel pointers may cross the
// persistence boundary. The world thread alone owns the mutable task cache.
namespace LivingActivity
{
    enum class Mode { Off, Observe, Active };
    enum class Phase {
        Queued, Preparing, Traveling, Executing, Verifying, Completed,
        WaitingExternal, Paused, Deferred, Failed, Cancelled, Reconciling
    };
    enum class Priority : uint8_t {
        Human = 10, Preparation = 20, Scheduled = 30, Delivery = 40,
        Progression = 50, Optional = 60
    };
    enum class Kind {
        HumanRequest, PartyErrand, Profession, GuildDelivery, GuildEvent,
        Progression, Maintenance, Commission, CollectionReconciliation
    };
    enum class Effect : uint32_t {
        Inspect = 0, Movement = 1, Group = 2, Inventory = 4, Money = 8,
        Spell = 16, Equipment = 32, Guild = 64, Social = 128, TravelTarget = 256
    };
    enum class OperationState { Intent, Reconciling, Verified, Rejected };

    const char* Name(Phase value);
    const char* Name(Kind value);
    const char* Name(Mode value);
    bool ParsePhase(const std::string& value, Phase& result);
    bool ParseKind(const std::string& value, Kind& result);
    Kind LegacyEconomyKind(const std::string& goalType);
    bool IsUuid(const std::string& value);
    bool IsToken(const std::string& value, size_t limit = 64, bool empty = false);
    bool IsSourceKey(const std::string& value);
    bool Terminal(Phase phase);
    bool ConsumesActiveTime(Phase phase);

    struct WorldContext {
        uint32_t actor = 0;
        uint64_t policyRevision = 0;
        uint32_t map = 0, instance = 0;
        std::string session;
        uint64_t sessionRevision = 0;
        // Ephemeral native lifecycle epochs, not resumable DB authority. Map
        // IDs alone cannot distinguish leaving and returning to the same map.
        uint64_t actorGeneration = 0, mapGeneration = 0;
        // Never persisted as a resumable authority. A new boot invalidates all
        // outstanding worker proposals and execution contexts from the old boot.
        std::string boot;
        bool operator==(const WorldContext& other) const;
    };

    struct Checkpoint {
        uint32_t version = 1;
        std::string step = "legacy_reconciliation";
        // Bounded, domain-typed JSON. Never player text or arbitrary scripts.
        std::string data = "{}";
        std::string blocker;
        uint64_t activeElapsedMs = 0, lastProgressAtMs = 0;
    };

    struct Task {
        std::string id, source, sourceKey, root, parent;
        uint32_t actor = 0;
        Kind kind = Kind::CollectionReconciliation;
        Mode mode = Mode::Observe;
        Phase phase = Phase::Reconciling;
        Priority priority = Priority::Delivery;
        bool accepted = true;
        uint64_t revision = 1, ownerGeneration = 0;
        uint64_t dueAtMs = 0, retryAtMs = 0, createdAtMs = 0, updatedAtMs = 0;
        WorldContext context;
        Checkpoint checkpoint;
    };

    struct ActivityLease {
        uint32_t actor = 0;
        std::string rootTask;
        uint64_t generation = 0;
        WorldContext context;
    };
    bool SameLease(const ActivityLease& left, const ActivityLease& right);
    // A live same-kind lease still belongs to an exact job and caller token.
    // Expiry permits fresh admission, never renewal/release by a stale token.
    bool MayAcquireCompatibilityLease(const ActivityLease& held, const ActivityLease& caller,
        const ActivityLease& requested, bool unexpired);
    struct ActionContext {
        std::string task, rootTask, origin;
        uint64_t revision = 0, ownerGeneration = 0;
        uint32_t permittedEffects = 0;
        WorldContext world;
        // Populated only for one journalled native operation. A timed spell
        // retains immutable attribution; every synchronous effect callback
        // still rechecks authority. Its presence alone grants no replay.
        std::string operation;
    };
    struct ResourceClaim {
        std::string id, task, location, state;
        uint32_t actor = 0, itemGuid = 0, itemEntry = 0;
        uint64_t quantity = 0, copper = 0, nativeReference = 0, revision = 1;
    };
    struct OperationResult {
        std::string id, task, kind, nativeReference, evidence;
        uint64_t taskRevision = 0;
        OperationState state = OperationState::Reconciling;
    };
    struct Transition {
        std::string id, task, code;
        uint64_t revision = 0, atMs = 0;
        Phase phase = Phase::Reconciling;
    };

    // Completion is an executor-owned proof, never inferred from a need flag or
    // legacy state. Observe mode cannot acknowledge gameplay completion.
    struct CompletionProof {
        bool verified = false;
        bool unresolvedOperation = true;
        std::string operation;
    };
    bool CanTransition(const Task& before, Phase after, const CompletionProof& proof = {});
    bool Validate(const Task& task, std::string& error);
    Task AfterRestart(Task task, uint64_t nowMs);
    bool Fresh(const Task& task, const ActionContext& action, const WorldContext& current);
    bool Before(const Task& left, const Task& right);

    // Timer units are monotonic deltas supplied by the world update, not wall
    // clock age. Restore, safety pause and external waiting accrue no work time.
    uint64_t ActiveElapsed(uint64_t prior, Phase phase, uint64_t monotonicDeltaMs);

    // Actual SQL used by the realm and the isolated MariaDB integration test.
    // The receipt SELECT is REQUIRED: CMaNGOS CommitTransactionDirect() does not
    // propagate SqlTransaction::Execute() failure. No caller may treat its bool
    // or an enqueued PExecute as proof that this write committed.
    struct WritePlan {
        std::vector<std::string> statements;
        std::string receiptQuery;
        std::string task;
        uint64_t revision = 0;
    };
    WritePlan TaskWrite(const Task& task, uint64_t expectedRevision,
        const std::string& receipt, const std::string& code, const std::string& dependentFingerprint = "");
    WritePlan OperationIntentWrite(const Task& executingTask, uint64_t expectedRevision,
        const std::string& operation, const std::string& kind, const std::string& beforeJson);
    WritePlan OperationOutcomeWrite(const Task& verifyingTask, uint64_t expectedRevision,
        const OperationResult& result, const std::string& receipt, const std::string& afterJson);
    std::string SqlValue(const std::string& value);
    std::string PersistedTaskProjection();
    bool ReceiptMatches(const WritePlan& plan, const std::string& task, uint64_t revision);
}
#endif
