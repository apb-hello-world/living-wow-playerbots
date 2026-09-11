#include "LivingActivity.h"
#include "LivingActivityReceipts.h"
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
    {
        ObservationQueue off;
        off.restoreOwnership=off.due=true;off.claimsLoaded=false;
        assert(NextObservationWork(off)==ObservationWork::Probe);
        off.schemaReady=true;off.retained=off.historyLimit; // Read-only load is not outbox admission.
        assert(NextObservationWork(off)==ObservationWork::Load);
        off.incoming=2;
        assert(NextObservationWork(off)==ObservationWork::Decode);
        off.incoming=0;off.loaded=true;
        assert(NextObservationWork(off)==ObservationWork::RestoreClaims);
        off.claimsLoaded=true;off.pending=2; // No speculative import or uncommitted task write.
        assert(NextObservationWork(off)==ObservationWork::Wait);
        off.nativeOutcomes=1;
        assert(NextObservationWork(off)==ObservationWork::Flush);
        off.ioPending=true;
        assert(NextObservationWork(off)==ObservationWork::Wait);
        off.ioPending=false;off.nativeOutcomes=0;off.loaded=false;off.due=false;
        assert(NextObservationWork(off)==ObservationWork::Wait);
        off.due=true;off.cached=off.cacheLimit;
        assert(NextObservationWork(off)==ObservationWork::CachePressure);
    }
    {
        ObservationQueue stopped;
        stopped.schemaReady=true;stopped.pending=1;stopped.nativeOutcomes=1;
        stopped.retained=stopped.historyLimit; // Already admitted result has reserved capacity.
        assert(!stopped.enabled && !stopped.due);
        assert(NextObservationWork(stopped)==ObservationWork::Flush);
        stopped.ioPending=true;
        assert(NextObservationWork(stopped)==ObservationWork::Wait);
        stopped.ioPending=false;stopped.nativeOutcomes=0;
        assert(NextObservationWork(stopped)==ObservationWork::Wait); // No new task work while off.
        stopped.nativeOutcomes=1;stopped.pending=0;
        assert(NextObservationWork(stopped)==ObservationWork::Wait); // Retry deadline has not arrived.
        stopped.pending=stopped.nativeOutcomes=17;
        assert(NextObservationWork(stopped)==ObservationWork::Wait); // Cannot invent unbounded reserved slots.
        stopped.pending=stopped.nativeOutcomes=16;
        assert(NextObservationWork(stopped)==ObservationWork::Flush);
        stopped.schemaReady=false;
        assert(NextObservationWork(stopped)==ObservationWork::Wait);
    }
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
    // A quiet import rotation sets its next check sixty seconds ahead. That
    // must not stretch native-save retries of 5/10/30 seconds to one minute each.
    ObservationQueue retryQueue;
    retryQueue.enabled = retryQueue.schemaReady = retryQueue.loaded = true;
    ReceiptRetry receiptRetry;
    uint64_t retryNow = 1000;
    for (const uint64_t delay : {5000,10000,30000}) {
        receiptRetry.Missed(retryNow);
        assert(receiptRetry.dueAtMs == retryNow + delay);
        retryQueue.due = false; retryQueue.pending = 0;
        assert(NextObservationWork(retryQueue) == ObservationWork::Wait);
        retryNow = receiptRetry.dueAtMs;
        retryQueue.pending = 1;
        assert(NextObservationWork(retryQueue) == ObservationWork::Flush);
        retryQueue.ioPending = true;
        assert(NextObservationWork(retryQueue) == ObservationWork::Wait);
        retryQueue.ioPending = false;
    }
    retryQueue.schemaReady = false;
    assert(NextObservationWork(retryQueue) == ObservationWork::Wait);
    retryQueue.schemaReady = true; retryQueue.retained = retryQueue.historyLimit;
    assert(NextObservationWork(retryQueue) == ObservationWork::HistoryPressure);
    retryQueue.enabled = false;
    assert(NextObservationWork(retryQueue) == ObservationWork::Wait);
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
    // Receipt acknowledgements are per write, not all-or-nothing CAS success.
    struct PendingWrite { WritePlan plan; ReceiptRetry retry; std::string nativeEvidence; };
    WritePlan second = plan; second.task = Receipt;
    std::deque<PendingWrite> pending{{plan,{},"original_native_result"},{second,{},"second"}};
    assert(PrepareReceiptBatch(pending,32,1000) == 2);
    unsigned acknowledged = 0;
    auto countReceipt = [&](const PendingWrite& write) { assert(write.plan.task == Receipt); ++acknowledged; };
    assert(SettleReceiptBatch(pending,2,true,{{Receipt,1}},1000,countReceipt) == 1);
    assert(acknowledged == 1 && pending.size() == 1 && pending[0].plan.task == Id);
    assert(pending[0].retry.failures == 1 && pending[0].retry.dueAtMs == 6000);
    assert(pending[0].nativeEvidence == "original_native_result");
    assert(PrepareReceiptBatch(pending,32,5999) == 0);
    pending.push_back({second,{},"new"});
    assert(PrepareReceiptBatch(pending,32,2000,true) == 1 && pending[0].plan.task == Receipt);
    assert(SettleReceiptBatch(pending,1,true,{{Receipt,1}},2000,countReceipt) == 1);
    pending.push_back({second,{},"fresh"});
    assert(PrepareReceiptBatch(pending,32,6000,true) == 1 && pending[0].plan.task == Id);
    assert(SettleReceiptBatch(pending,1,true,{},6000,countReceipt) == 0);
    assert(pending.back().retry.failures == 2 && pending.back().retry.dueAtMs == 16000);
    assert(PrepareReceiptBatch(pending,32,6000) == 1 && pending[0].plan.task == Receipt);
    // Missing healthy sentinel (query failure) never grants even a supplied row.
    assert(SettleReceiptBatch(pending,1,false,{{Receipt,1}},6000,countReceipt) == 0);
    assert(pending.size() == 2 && acknowledged == 2);
    // After a transaction-wide failure, retry one write, never the poisoned batch.
    assert(PrepareReceiptBatch(pending,32,20000) == 1);
    assert(SettleReceiptBatch(pending,3,true,{},20000,countReceipt) == 0 && pending.size() == 2);
    ReceiptRetry retry; retry.Missed(std::numeric_limits<uint64_t>::max()-1);
    assert(retry.dueAtMs == std::numeric_limits<uint64_t>::max());
    for (unsigned i=0;i<100;++i) retry.Missed(1);
    assert(retry.dueAtMs == 60001);
    task.checkpoint.data.resize(8193, 'x'); assert(!Validate(task, error));
    task = Sample(); task.phase = Phase::Completed; assert(!Validate(task, error));
    task = Sample(); task.ownerGeneration = std::numeric_limits<uint64_t>::max();
    rejected = false;
    try { AfterRestart(task, 6000); } catch (const std::overflow_error&) { rejected = true; }
    assert(rejected);
}
