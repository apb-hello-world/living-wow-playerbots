#include "LivingActivity.h"
#include "LivingActivityAdmission.h"
#include <cassert>
#include <limits>
#include <stdexcept>

using namespace LivingActivity;
static const std::string Id = "637bd562-36d2-5b01-bc01-e2d831c49f38";
static const std::string Receipt = "ff2efbdf-f0ec-4539-b840-299847970c00";
Task Sample() {
    Task task; task.id = task.root = Id; task.source = "economy_goal"; task.sourceKey = "42";
    task.actor = task.context.actor = 497; task.kind = Kind::Profession;
    task.createdAtMs = task.updatedAtMs = 1000; task.context.boot = Receipt;
    return task;
}
int main() {
    assert(IsSourceKey("18010501:64"));
    assert(IsSourceKey("chat-v2:ABC_0.1"));
    assert(IsSourceKey("637bd562-36d2-5b01-bc01-e2d831c49f38"));
    for(const auto& key : {"", "arbitrary player text", "../private/path", "line\nbreak", "'sql'"})
        assert(!IsSourceKey(key));
    assert(!IsSourceKey(std::string(161, 'a')));
    ActivityLease held; held.actor=497; held.rootTask=Id; held.generation=1; held.context=Sample().context;
    auto requested=held; requested.generation=0;
    assert(MayAcquireCompatibilityLease({}, {}, requested, false));
    assert(MayAcquireCompatibilityLease(held, held, requested, true));
    assert(!MayAcquireCompatibilityLease(held, {}, requested, true));
    auto stale=held; ++stale.generation;
    assert(!SameLease(held, stale));
    assert(!MayAcquireCompatibilityLease(held, stale, requested, true));
    auto anotherLease=requested; anotherLease.rootTask=Receipt;
    assert(!MayAcquireCompatibilityLease(held, held, anotherLease, true));
    assert(MayAcquireCompatibilityLease(held, held, anotherLease, false));
    auto reassigned=held; ++reassigned.actor;
    assert(!SameLease(held, reassigned));
    assert(!MayAcquireCompatibilityLease(held, held, reassigned, true));
    auto rezoned=held; ++rezoned.context.mapGeneration;
    assert(!SameLease(held, rezoned));
    assert(!SameLease({}, {}));
    ObservationQueue queue;
    assert(NextObservationWork(queue) == ObservationWork::Wait);
    queue.enabled = queue.due = true;
    assert(NextObservationWork(queue) == ObservationWork::Probe);
    queue.schemaReady = true;
    assert(NextObservationWork(queue) == ObservationWork::Load);
    queue.loaded = true;
    assert(NextObservationWork(queue) == ObservationWork::Import);
    queue.incoming = 64;
    assert(NextObservationWork(queue) == ObservationWork::Decode);
    queue.pending = 32; queue.cached = queue.cacheLimit - 32;
    assert(NextObservationWork(queue) == ObservationWork::Flush); // Pending rows cannot block themselves.
    queue.ioPending = true;
    assert(NextObservationWork(queue) == ObservationWork::Wait); // Only one native query/receipt in flight.
    queue.ioPending = false; queue.retained = queue.historyLimit - 32;
    assert(NextObservationWork(queue) == ObservationWork::Flush); // Exact capacity is allowed.
    ++queue.retained;
    assert(NextObservationWork(queue) == ObservationWork::HistoryPressure);
    assert(queue.pending == 32 && queue.incoming == 64); // Backpressure never drops evidence.
    queue.pending = queue.retained = 0; queue.cached = queue.cacheLimit;
    assert(NextObservationWork(queue) == ObservationWork::CachePressure);
    unsigned family = 0, seen[4] = {};
    for (unsigned i = 0; i < 32; ++i) { ++seen[family]; family = NextImportFamily(family); }
    for (auto count : seen) assert(count == 8); // A perpetually busy producer cannot monopolize admission.
    Task task = Sample(); std::string error;
    assert(Validate(task, error));
    assert(LegacyEconomyKind("equipment_upgrade") == Kind::Progression);
    assert(LegacyEconomyKind("profession_skill_up") == Kind::Profession);
    assert(LegacyEconomyKind("storage_pressure") == Kind::Maintenance);
    assert(LegacyEconomyKind("unknown") == Kind::CollectionReconciliation);
    assert(!IsUuid("not-a-task")); assert(!IsUuid("00000000-0000-0000-0000-000000000000"));
    assert(IsUuid(Id)); assert(!IsToken("go; drop table", 64));
    assert(!CanTransition(task, Phase::Completed, {true, false, Receipt}));
    task.mode = Mode::Active; task.phase = Phase::Queued;
    assert(CanTransition(task, Phase::Preparing)); assert(!CanTransition(task, Phase::Executing));
    task.phase = Phase::Executing;
    assert(CanTransition(task, Phase::Verifying)); assert(!CanTransition(task, Phase::Completed, {true, false, Receipt}));
    task.phase = Phase::Verifying;
    assert(CanTransition(task, Phase::Completed, {true, false, Receipt}));
    assert(!CanTransition(task, Phase::Completed, {true, true, Receipt}));
    assert(!CanTransition(task, Phase::Completed, {false, false, Receipt}));
    assert(!CanTransition(task, Phase::Cancelled));
    task.phase = Phase::Completed; assert(!CanTransition(task, Phase::Preparing));
    task = Sample();
    auto restored = AfterRestart(task, 5000);
    assert(restored.id == task.id && restored.sourceKey == task.sourceKey);
    assert(restored.phase == Phase::Reconciling && restored.context.boot.empty());
    assert(restored.revision == task.revision + 1 && restored.ownerGeneration == task.ownerGeneration + 1);
    assert(restored.checkpoint.activeElapsedMs == task.checkpoint.activeElapsedMs);
    assert(ActiveElapsed(100, Phase::Paused, 300000) == 100);
    assert(ActiveElapsed(100, Phase::WaitingExternal, 300000) == 100);
    assert(ActiveElapsed(100, Phase::Reconciling, 300000) == 100);
    assert(ActiveElapsed(100, Phase::Traveling, 125) == 225);
    assert(ActiveElapsed(std::numeric_limits<uint64_t>::max()-5, Phase::Executing, 10) == std::numeric_limits<uint64_t>::max());
    auto other = task; other.priority = Priority::Human; other.createdAtMs = 9000;
    assert(Before(other, task));
    other = task; other.accepted = false; other.dueAtMs = 1; assert(Before(task, other));
    other = task; other.dueAtMs = 5000; assert(Before(other, task));
    ActionContext action; action.task = action.rootTask = task.id;
    action.revision = task.revision; action.ownerGeneration = task.ownerGeneration; action.world = task.context;
    assert(!Fresh(task, action, task.context)); // Shadow never executes.
    task.mode = Mode::Active; assert(Fresh(task, action, task.context));
    auto changed = task.context; ++changed.sessionRevision; assert(!Fresh(task, action, changed));
    changed = task.context; ++changed.policyRevision; assert(!Fresh(task, action, changed));
    changed = task.context; ++changed.map; assert(!Fresh(task, action, changed));
    changed = task.context; ++changed.mapGeneration; assert(!Fresh(task, action, changed));
    changed = task.context; ++changed.actorGeneration; assert(!Fresh(task, action, changed));
    changed = task.context; changed.boot = Id; assert(!Fresh(task, action, changed));
    ++action.revision; assert(!Fresh(task, action, task.context));
    assert(SqlValue("a'\\b") == "X'61275c62'");
    task = Sample(); auto plan = TaskWrite(task, 0, Receipt, "legacy_observed");
    assert(plan.statements.size() == 2 && !plan.receiptQuery.empty());
    assert(!ReceiptMatches(plan, "", 0)); // Queued SQL / failed transaction is NOT a receipt.
    assert(!ReceiptMatches(plan, Id, 2)); assert(ReceiptMatches(plan, Id, 1));
    assert(plan.receiptQuery.find("request_hash=SHA2(") != std::string::npos);
    bool rejected = false;
    try { TaskWrite(task, 1, Receipt, "stale"); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    for (Phase nativePhase : {Phase::Executing, Phase::Verifying}) {
        auto unjournalled = Sample(); unjournalled.mode = Mode::Active; unjournalled.phase = nativePhase;
        ++unjournalled.revision; rejected = false;
        try { TaskWrite(unjournalled, 1, Receipt, "task_admitted"); }
        catch (const std::invalid_argument&) { rejected = true; }
        assert(rejected);
    }
    task.checkpoint.data.resize(8193, 'x'); assert(!Validate(task, error));
    task = Sample(); task.phase = Phase::Completed; assert(!Validate(task, error));
    task = Sample(); task.ownerGeneration = std::numeric_limits<uint64_t>::max();
    rejected = false;
    try { AfterRestart(task, 6000); } catch (const std::overflow_error&) { rejected = true; }
    assert(rejected);
}
