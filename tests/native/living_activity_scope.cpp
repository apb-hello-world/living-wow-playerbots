#include "LivingActivityScope.h"
#include <cassert>
#include <thread>
using namespace LivingActivity;
int main() {
    Task task; task.id = task.root = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    task.source = "profession"; task.sourceKey = "1"; task.actor = task.context.actor = 497;
    task.context.boot = "ff2efbdf-f0ec-4539-b840-299847970c00";
    task.context.actorGeneration = task.context.mapGeneration = task.context.policyRevision = 1;
    task.createdAtMs = task.updatedAtMs = 1; task.mode = Mode::Active; task.phase = Phase::Traveling;
    ExecutionAuthority authority; authority.Observe(task.context, 0);
    const auto grant = authority.Acquire(task, Mask(Effect::Movement), 100, 1000);
    assert(grant.Granted()); task.ownerGeneration = grant.lease.generation;
    PermissionPublisher publisher; const auto reader = publisher.Reader(); publisher.Publish(authority.Read(task.actor));
    ActionContext action; action.task = action.rootTask = task.id; action.world = task.context;
    action.revision = task.revision; action.ownerGeneration = grant.lease.generation;
    action.origin = "service_adapter"; action.permittedEffects = Mask(Effect::Movement);
    const Effects movement{Mask(Effect::Movement), Lane::Managed, true};
    auto check = [&] { return ExecutionScope::Check(reader, movement, task.context, 200); };
    assert(check() == AuthorityCode::StaleLease);
    assert(ExecutionScope::MutationEffects(task.actor,Mask(Effect::Movement)).lane == Lane::Managed);
    {
        ExecutionScope scope(task, action);
        assert(check() == AuthorityCode::Allowed);
        assert(ExecutionScope::MutationEffects(task.actor,Mask(Effect::Movement)).lane == Lane::Managed);
        assert(ExecutionScope::Origin(task.actor) == "service_adapter");
        {
            EvaluationScope evaluation(true);
            assert(ExecutionScope::Origin(task.actor) == "eligibility_evaluation");
            assert(ExecutionScope::Check(reader,{0,Lane::Inspection,true},task.context,200) == AuthorityCode::Allowed);
            assert(!evaluation.Rejected());
            assert(check() == AuthorityCode::EffectsDenied);
            assert(evaluation.Rejected());
            EvaluationScope nested(true);
            ExecutionScope nestedExecution(task,action);
            assert(check() == AuthorityCode::EffectsDenied && nested.Rejected());
            std::thread independent([&] {
                ExecutionScope explicitExecution(task,action);
                assert(check() == AuthorityCode::Allowed);
            });
            independent.join();
        }
        assert(check() == AuthorityCode::Allowed);
        {
            EvaluationScope observation(false);
            assert(check() == AuthorityCode::EffectsDenied);
            assert(!observation.Rejected()); // Observation cannot change selection.
        }
        {
            EvaluationScope unknown(true);
            assert(ExecutionScope::Check(reader,{},task.context,200) == AuthorityCode::EffectsDenied);
            assert(unknown.Rejected());
        }
        std::thread unscopedWorker([&] { assert(check() == AuthorityCode::StaleLease); });
        unscopedWorker.join();
        {
            auto otherTask = task; otherTask.actor = 498;
            ExecutionScope other(otherTask, action);
            assert(check() == AuthorityCode::StaleLease);
        }
        assert(check() == AuthorityCode::Allowed);
        try {
            auto stale = action; ++stale.revision;
            ExecutionScope wrong(task, stale);
            assert(check() == AuthorityCode::StaleLease);
            throw 1;
        } catch (int) {}
        assert(check() == AuthorityCode::Allowed); // RAII restores attribution, not an obsolete grant.
        authority.Release(grant.lease); publisher.Publish(authority.Read(task.actor));
        assert(check() == AuthorityCode::StaleLease);
    }
    assert(ExecutionScope::Origin(task.actor) == "unscoped");
    NativePermit native{task.context, Lane::Combat, Mask(Effect::Movement), 0, true};
    {
        ExecutionScope combat(native);
        const auto nativeMovement=ExecutionScope::MutationEffects(task.actor,Mask(Effect::Movement));
        assert(nativeMovement.lane == Lane::Combat);
        assert(ExecutionScope::Check(reader,nativeMovement,task.context,200) == AuthorityCode::Allowed);
        assert(ExecutionScope::MutationEffects(task.actor+1,Mask(Effect::Movement)).lane == Lane::Managed);
        assert(ExecutionScope::MutationEffects(task.actor,Mask(Effect::TravelTarget)).lane == Lane::Managed);
        {
            EvaluationScope evaluation(true);
            assert(ExecutionScope::Check(reader,nativeMovement,task.context,200) == AuthorityCode::EffectsDenied);
            assert(evaluation.Rejected());
        }
        assert(ExecutionScope::Check(reader, {Mask(Effect::Movement), Lane::Combat, true}, task.context, 200) == AuthorityCode::Allowed);
        assert(check() == AuthorityCode::StaleLease); // A combat permit never grants service travel.
    }
}
