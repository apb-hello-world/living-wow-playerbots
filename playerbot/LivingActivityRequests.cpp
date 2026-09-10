#include "LivingActivityRequests.h"
#include <limits>

namespace LivingActivity {
    const char* Name(AdmissionCode code) {
        switch (code) {
#define CASE(value, text) case AdmissionCode::value: return text
        CASE(Pending, "persistence_pending"); CASE(Saved, "saved");
        CASE(Disabled, "execution_disabled"); CASE(NotReady, "startup_reconciliation");
        CASE(InvalidRequest, "invalid_task_request"); CASE(StaleRevision, "stale_task_revision");
        CASE(StaleContext, "stale_native_context"); CASE(ConflictingWrite, "task_write_pending");
        CASE(Backpressure, "task_admission_backpressure");
        CASE(ReconciliationRequired, "native_reconciliation_required");
#undef CASE
        }
        return "invalid_admission_code";
    }
    namespace {
        bool ValidContext(const WorldContext& context) {
            return context.actor && context.actorGeneration && context.mapGeneration &&
                context.policyRevision && IsUuid(context.boot) && context.session.size() <= 120 &&
                (context.session.empty() == (context.sessionRevision == 0));
        }
        bool SameIdentity(const Task& a, const Task& b) {
            return a.id == b.id && a.source == b.source && a.sourceKey == b.sourceKey &&
                a.actor == b.actor && a.kind == b.kind && a.root == b.root && a.parent == b.parent &&
                a.createdAtMs == b.createdAtMs;
        }
    }
    AdmissionCode ValidateTaskRequest(const TaskRequest& request, const Task* saved,
        const WorldContext& current, std::string& reason) {
        const Task& task = request.task;
        if (!Validate(task, reason) || !IsUuid(request.receipt) || !IsSourceKey(task.sourceKey) ||
            request.expectedRevision >= std::numeric_limits<uint64_t>::max() - 1 ||
            task.revision != request.expectedRevision + 1) {
            reason = "invalid_task_request"; return AdmissionCode::InvalidRequest;
        }
        if (!ValidContext(current) || !(task.context == current)) {
            reason = "stale_native_context"; return AdmissionCode::StaleContext;
        }
        // Terminal outcomes use the operation/claim reconciler, not admission.
        // A model, planner refresh, or caller's boolean cannot prove completion.
        if (Terminal(task.phase)) {
            reason = "terminal_outcome_requires_native_proof";
            return AdmissionCode::ReconciliationRequired;
        }
        if (!saved) {
            if (request.expectedRevision) {
                reason = "task_not_loaded"; return AdmissionCode::StaleRevision;
            }
            if (task.phase != Phase::Queued || task.ownerGeneration || !task.parent.empty()) {
                reason = "new_root_must_be_queued"; return AdmissionCode::InvalidRequest;
            }
        } else {
            if (saved->revision != request.expectedRevision) {
                reason = "stale_task_revision"; return AdmissionCode::StaleRevision;
            }
            if (!SameIdentity(task, *saved) || task.ownerGeneration != saved->ownerGeneration ||
                task.updatedAtMs < saved->updatedAtMs ||
                task.checkpoint.activeElapsedMs < saved->checkpoint.activeElapsedMs ||
                (saved->accepted && !task.accepted) ||
                (saved->mode == Mode::Active && task.mode != Mode::Active)) {
                reason = "committed_identity_cannot_be_replanned"; return AdmissionCode::InvalidRequest;
            }
            if (!(saved->context == current) && task.phase != Phase::Reconciling) {
                reason = "changed_context_requires_reconciliation";
                return AdmissionCode::ReconciliationRequired;
            }
            if (!CanTransition(*saved, task.phase)) {
                reason = "invalid_task_transition"; return AdmissionCode::InvalidRequest;
            }
        }
        reason.clear(); return AdmissionCode::Pending;
    }
    bool SameRequest(const WritePlan& a, const WritePlan& b) {
        return !a.receiptQuery.empty() && a.task == b.task && a.revision == b.revision &&
            a.receiptQuery == b.receiptQuery && a.statements == b.statements;
    }
    bool SavedTaskExecutable(const Task& saved, uint64_t revision,
        const WorldContext& current, uint64_t wallNow, std::string& reason) {
        if (saved.revision != revision) reason = "stale_task_revision";
        else if (!ValidContext(current) || !(saved.context == current)) reason = "stale_native_context";
        else if (saved.mode != Mode::Active ||
            (saved.phase != Phase::Preparing && saved.phase != Phase::Traveling && saved.phase != Phase::Executing))
            reason = "task_not_executable";
        else if (saved.retryAtMs > wallNow) reason = "task_backoff";
        else if (!saved.checkpoint.blocker.empty()) reason = saved.checkpoint.blocker;
        else { reason.clear(); return true; }
        return false;
    }
}
