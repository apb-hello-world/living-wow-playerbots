#include "LivingActivityRecovery.h"
#include "LivingActivityScope.h"
#include <cassert>
using namespace LivingActivity;
namespace {
    struct Actor {
        bool alive = true, inWorld = true, transfer = false;
        bool IsAlive() const { return alive; }
        bool IsInWorld() const { return inWorld; }
        bool IsBeingTeleported() const { return transfer; }
    };
}
int main() {
    Actor actor;
    assert(RequiredLifeTransition(actor, false) == LifeTransition::None);
    actor.alive = false;
    assert(RequiredLifeTransition(actor, false) == LifeTransition::Died);
    assert(RequiredLifeTransition(actor, true) == LifeTransition::None); // No repeated death credit.
    assert(ReadyForCorpseRecovery(actor, CorpseRecovery::Release, false, false, false));
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Find, false, false, false));
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Release, true, true, false));
    assert(ReadyForCorpseRecovery(actor, CorpseRecovery::Find, true, true, false));
    assert(ReadyForCorpseRecovery(actor, CorpseRecovery::Reclaim, true, true, false));
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Reclaim, true, false, false));
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Reclaim, true, true, true));
    actor.transfer = true;
    assert(RequiredLifeTransition(actor, false) == LifeTransition::None);
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Find, true, true, false));
    actor.transfer = false; actor.inWorld = false;
    assert(RequiredLifeTransition(actor, false) == LifeTransition::None);
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Release, false, false, false));
    actor.inWorld = true; actor.alive = true;
    assert(RequiredLifeTransition(actor, true) == LifeTransition::Resurrected);
    assert(!ReadyForCorpseRecovery(actor, CorpseRecovery::Reclaim, true, true, false));

    Task task; task.id = task.root = "dd2c720d-1821-5a37-91d3-21b0ee3c99cf";
    task.source = "guild_procurement"; task.sourceKey = "test"; task.actor = task.context.actor = 444;
    task.context.boot = "ff2efbdf-f0ec-4539-b840-299847970c00";
    task.context.actorGeneration = task.context.mapGeneration = task.context.policyRevision = 1;
    task.createdAtMs = task.updatedAtMs = 1; task.mode = Mode::Active; task.phase = Phase::Executing;
    ExecutionAuthority authority; authority.Observe(task.context, 0);
    const auto lease = authority.Acquire(task, Mask(Effect::Movement), 100, 1000); assert(lease.Granted());
    PermissionPublisher publisher; const auto reader = publisher.Reader();
    publisher.Publish(authority.Read(444));
    const Effects state{0, Lane::State, true};
    NativePermit statePermit{task.context, Lane::State, 0, 127, true};
    assert(ExecutionScope::Check(reader, state, task.context, 200) == AuthorityCode::EffectsDenied);
    {
        ExecutionScope scope(statePermit);
        assert(ExecutionScope::Check(reader, state, task.context, 200, 127) == AuthorityCode::Allowed);
        for (auto effect : {Effect::Movement, Effect::TravelTarget, Effect::Spell, Effect::Group,
            Effect::Inventory, Effect::Money, Effect::Equipment, Effect::Guild, Effect::Social}) {
            assert(ExecutionScope::Check(reader, {Mask(effect), Lane::State, true}, task.context, 200) == AuthorityCode::EffectsDenied);
            assert(ExecutionScope::Check(reader, ExecutionScope::MutationEffects(444, Mask(effect)), task.context, 200) != AuthorityCode::Allowed);
        }
        auto stale = task.context; ++stale.mapGeneration;
        assert(ExecutionScope::Check(reader, state, stale, 200) == AuthorityCode::StaleContext);
        EvaluationScope evaluate(true);
        assert(ExecutionScope::Check(reader, state, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(evaluate.Rejected()); // Mutable AI state is not read-only inspection.
    }
    const auto operation = "ff2efbdf-f0ec-4539-b840-299847970c01";
    assert(authority.BeginAtomic(lease.lease, operation, 200).code == AuthorityCode::Allowed);
    assert(authority.BeginDispatch(lease.lease, operation, 200).code == AuthorityCode::Allowed);
    publisher.Publish(authority.Read(444));
    {
        ExecutionScope scope(statePermit);
        assert(ExecutionScope::Check(reader, state, task.context, 200) == AuthorityCode::AtomicPending);
    }
    {
        NativePermit safety{task.context, Lane::Safety, RecoveryEffects(), uint32_t(Safety::Death), true};
        ExecutionScope scope(safety);
        assert(ExecutionScope::Check(reader, {RecoveryEffects(), Lane::Safety, true}, task.context, 200,
            uint32_t(Safety::Death)) == AuthorityCode::AtomicPending);
    }
    assert(authority.EndDispatch(lease.lease, operation).code == AuthorityCode::Allowed);
    publisher.Publish(authority.Read(444));
    {
        ExecutionScope scope(statePermit);
        assert(ExecutionScope::Check(reader, state, task.context, 200) == AuthorityCode::Allowed);
    }
    const Effects recovery{RecoveryEffects(), Lane::Safety, true};
    const NativePermit nativeRecovery{task.context, Lane::Safety, recovery.mask, uint32_t(Safety::Death), true};
    {
        ExecutionScope scope(nativeRecovery);
        assert(ExecutionScope::Check(reader, recovery, task.context, 200, uint32_t(Safety::Death)) == AuthorityCode::Allowed);
        for (auto block : {Safety::Combat, Safety::Transfer, Safety::Taxi, Safety::Transport, Safety::Falling, Safety::UnsafeOperation})
            assert(ExecutionScope::Check(reader, recovery, task.context, 200,
                uint32_t(Safety::Death) | uint32_t(block)) == AuthorityCode::SafetyPaused);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Inventory), Lane::Safety, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {Mask(Effect::TravelTarget), Lane::Safety, true}, task.context, 200) == AuthorityCode::EffectsDenied);
    }
    // Safety does not erase the accepted root, paid-operation identity or lease.
    assert(authority.Read(444).operation == operation);
    assert(authority.Read(444).root.id == task.id);
    assert(authority.Read(444).root.accepted);
    assert(authority.FinishAtomic(lease.lease, operation).code == AuthorityCode::Allowed);
}
