#include "LivingActivityRequests.h"
#include <cassert>
#include <limits>
using namespace LivingActivity;
static const std::string Id = "637bd562-36d2-5b01-bc01-e2d831c49f38";
static const std::string Receipt = "ff2efbdf-f0ec-4539-b840-299847970c00";
static TaskRequest Request() {
    TaskRequest request; request.receipt = Receipt;
    auto& task = request.task; task.id = task.root = Id; task.source = "profession_job";
    task.sourceKey = "497:2881:order_41"; task.kind = Kind::Profession;
    task.actor = task.context.actor = 497; task.context.policyRevision = 4;
    task.context.actorGeneration = 7; task.context.mapGeneration = 3; task.context.boot = Receipt;
    task.mode = Mode::Active; task.phase = Phase::Queued;
    task.createdAtMs = task.updatedAtMs = 1000; task.checkpoint.step = "materials";
    return request;
}
int main() {
    auto request = Request(); auto current = request.task.context; std::string reason;
    assert(ValidateTaskRequest(request, nullptr, current, reason) == AdmissionCode::Pending);
    assert(reason.empty());
    assert(!SavedTaskExecutable(request.task, 1, current, 1000, reason));
    auto plan = TaskWrite(request.task, 0, request.receipt, "task_admitted");
    assert(SameRequest(plan, TaskWrite(request.task, 0, request.receipt, "task_admitted")));
    auto changed = request; changed.task.checkpoint.data = "{\"quantity\":2}";
    assert(!SameRequest(plan, TaskWrite(changed.task, 0, changed.receipt, "task_admitted")));
    changed = request; changed.receipt = Id;
    assert(!SameRequest(plan, TaskWrite(changed.task, 0, changed.receipt, "task_admitted")));
    assert(!SameRequest({}, {}));
    for (Phase phase : {Phase::Preparing, Phase::Executing, Phase::Completed, Phase::Cancelled}) {
        changed = request; changed.task.phase = phase;
        assert(ValidateTaskRequest(changed, nullptr, current, reason) != AdmissionCode::Pending);
    }
    Task saved = request.task;
    request.expectedRevision = 1; request.task.revision = 2; request.task.phase = Phase::Preparing;
    assert(ValidateTaskRequest(request, &saved, current, reason) == AdmissionCode::Pending);
    assert(!SavedTaskExecutable(saved, request.task.revision, current, 1000, reason));
    saved = request.task;
    assert(SavedTaskExecutable(saved, 2, current, 1000, reason));
    for (int field = 0; field < 7; ++field) {
        auto stale = current;
        switch (field) {
            case 0: ++stale.actorGeneration; break;
            case 1: ++stale.mapGeneration; break;
            case 2: ++stale.map; break;
            case 3: ++stale.instance; break;
            case 4: ++stale.policyRevision; break;
            case 5: stale.session = "group_52"; stale.sessionRevision = 1; break;
            default: stale.boot = Id;
        }
        assert(!SavedTaskExecutable(saved, 2, stale, 1000, reason));
        changed = request; changed.expectedRevision = 2; changed.task.revision = 3;
        assert(ValidateTaskRequest(changed, &saved, stale, reason) == AdmissionCode::StaleContext);
        changed.task.context = stale;
        assert(ValidateTaskRequest(changed, &saved, stale, reason) == AdmissionCode::ReconciliationRequired);
        changed.task.phase = Phase::Reconciling;
        assert(ValidateTaskRequest(changed, &saved, stale, reason) == AdmissionCode::Pending);
        assert(!SavedTaskExecutable(changed.task, 3, stale, 1000, reason));
    }
    for (Phase phase : {Phase::Queued, Phase::Paused, Phase::Deferred, Phase::WaitingExternal,
        Phase::Reconciling, Phase::Verifying, Phase::Completed, Phase::Cancelled, Phase::Failed}) {
        auto waiting = saved; waiting.phase = phase;
        assert(!SavedTaskExecutable(waiting, 2, current, 1000, reason));
    }
    auto waiting = saved; waiting.retryAtMs = 1001;
    assert(!SavedTaskExecutable(waiting, 2, current, 1000, reason));
    assert(SavedTaskExecutable(waiting, 2, current, 1001, reason));
    waiting.checkpoint.blocker = "paid_mail_uncollected";
    assert(!SavedTaskExecutable(waiting, 2, current, 9999999, reason));
    request.expectedRevision = 2; request.task.revision = 3; request.task.phase = Phase::Traveling;
    assert(ValidateTaskRequest(request, &saved, current, reason) == AdmissionCode::Pending);
    for (int field = 0; field < 8; ++field) {
        changed = request;
        switch (field) {
            case 0: changed.task.id = changed.task.root = Receipt; break;
            case 1: changed.task.sourceKey = "replacement_job"; break;
            case 2: changed.task.kind = Kind::Maintenance; break;
            case 3: changed.task.accepted = false; break;
            case 4: changed.task.mode = Mode::Observe; break;
            case 5: ++changed.task.createdAtMs; break;
            case 6: ++changed.task.ownerGeneration; break;
            default: changed.task.source = "other_producer";
        }
        assert(ValidateTaskRequest(changed, &saved, current, reason) == AdmissionCode::InvalidRequest);
    }
    changed = request; changed.expectedRevision = std::numeric_limits<uint64_t>::max(); changed.task.revision = 0;
    assert(ValidateTaskRequest(changed, &saved, current, reason) == AdmissionCode::InvalidRequest);
    assert(ValidateTaskRequest(request, nullptr, current, reason) == AdmissionCode::StaleRevision);
    saved = AfterRestart(saved, 5000);
    assert(!SavedTaskExecutable(saved, saved.revision, current, 5000, reason));
    assert(saved.id == Id && saved.accepted);
}
