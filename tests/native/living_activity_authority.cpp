#include "LivingActivityAuthority.h"
#include <cassert>
#include <limits>

using namespace LivingActivity;
static const char* A = "637bd562-36d2-5b01-bc01-e2d831c49f38";
static const char* B = "ff2efbdf-f0ec-4539-b840-299847970c00";
static const char* C = "9411eb6c-d355-4618-b323-1c8e0b0daaa2";
static const char* Boot = "579bcd66-4e4d-449c-85ef-2acde971126a";
static constexpr uint32_t Movement = Mask(Effect::Movement) | Mask(Effect::TravelTarget);
WorldContext Context(uint32_t actor = 497) {
    WorldContext c; c.actor = actor; c.boot = Boot; c.actorGeneration = c.mapGeneration = c.policyRevision = 1;
    return c;
}
Task Root(const char* id = A, Priority priority = Priority::Delivery, uint32_t actor = 497) {
    Task t; t.id = t.root = id; t.actor = actor; t.context = Context(actor); t.mode = Mode::Active;
    t.source = "profession"; t.sourceKey = id; t.phase = Phase::Traveling; t.priority = priority;
    t.createdAtMs = t.updatedAtMs = 1000; return t;
}
ActionContext Action(Task& task, const ActivityLease& lease) {
    task.ownerGeneration = lease.generation;
    ActionContext a; a.task = task.id; a.rootTask = task.root; a.revision = task.revision;
    a.ownerGeneration = lease.generation; a.world = task.context; a.origin = "native_executor";
    a.permittedEffects = Movement; return a;
}
int main() {
    ExecutionAuthority authority(2);
    auto t = Root(); auto current = Context();
    assert(authority.Acquire(t, Movement, 1000, 5000).code == AuthorityCode::StaleContext);
    assert(authority.Observe(current, 0).code == AuthorityCode::Allowed);
    auto first = authority.Acquire(t, Movement, 1000, 5000);
    assert(first.Granted() && first.code == AuthorityCode::Granted && !first.displaced.actor);
    auto a = Action(t, first.lease);
    const Effects move{Movement, Lane::Managed, true};
    assert(authority.Authorize(move, current, 1100, &t, &a) == AuthorityCode::Allowed);
    assert(authority.Authorize({}, current, 1100) == AuthorityCode::UnknownAction);
    assert(authority.Authorize({0, Lane::Inspection, true}, current, 1100) == AuthorityCode::Allowed);
    assert(authority.Authorize({Movement, Lane::Inspection, true}, current, 1100) == AuthorityCode::EffectsDenied);
    auto forbidden = move; forbidden.mask |= Mask(Effect::Group);
    assert(authority.Authorize(forbidden, current, 1100, &t, &a) == AuthorityCode::EffectsDenied);
    auto staleAction = a; ++staleAction.revision;
    assert(authority.Authorize(move, current, 1100, &t, &staleAction) == AuthorityCode::StaleLease);
    auto renew = authority.Acquire(t, Movement, 1100, 5000);
    assert(renew.code == AuthorityCode::Renewed && renew.lease.generation == first.lease.generation);
    auto changed = t; changed.priority = Priority::Human;
    assert(authority.Acquire(changed, Movement, 1100, 5000).code == AuthorityCode::StaleRevision);
    changed = t; changed.checkpoint.data = "{\"recipe\":2881}";
    assert(authority.Acquire(changed, Movement, 1100, 5000).code == AuthorityCode::StaleRevision);
    assert(authority.Authorize(move, current, 1100, &changed, &a) == AuthorityCode::StaleRevision);
    auto optional = Root(B, Priority::Optional);
    assert(authority.Acquire(optional, Movement, 1100, 5000).code == AuthorityCode::PriorityDenied);
    auto human = Root(B, Priority::Human);
    auto second = authority.Acquire(human, Movement, 1100, 5000);
    assert(second.Granted() && second.displaced.generation == first.lease.generation);
    assert(authority.Release(first.lease).code == AuthorityCode::StaleLease);
    assert(authority.Authorize(move, current, 1100, &t, &a) == AuthorityCode::StaleLease);
    a = Action(human, second.lease);
    // Safety pauses the owner without deleting the root or granting background work.
    assert(authority.Observe(current, uint32_t(Safety::Combat)).code == AuthorityCode::SafetyPaused);
    assert(authority.Inspect(497, 1200).lease.generation == second.lease.generation);
    assert(authority.Authorize(move, current, 1200, &human, &a) == AuthorityCode::SafetyPaused);
    NativePermit combat{current, Lane::Combat, Mask(Effect::Movement) | Mask(Effect::Spell), uint32_t(Safety::Combat), true};
    assert(authority.Authorize({combat.effects, Lane::Combat, true}, current, 1200, nullptr, nullptr, &combat) == AuthorityCode::Allowed);
    assert(authority.Authorize({Movement, Lane::Combat, true}, current, 1200, nullptr, nullptr, &combat) == AuthorityCode::EffectsDenied);
    combat.validated = false;
    assert(authority.Authorize({Mask(Effect::Spell), Lane::Combat, true}, current, 1200, nullptr, nullptr, &combat) == AuthorityCode::EffectsDenied);
    authority.Observe(current, 0);
    assert(authority.Authorize(move, current, 1200, &human, &a) == AuthorityCode::Allowed);
    // A preparation child inherits root authority; it never acquires another lease.
    auto child = Root(C, Priority::Preparation); child.root = human.id; child.parent = human.id;
    auto childAction = Action(child, second.lease);
    assert(authority.Acquire(child, Movement, 1300, 5000).code == AuthorityCode::InvalidRequest);
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::StaleRevision);
    assert(authority.SelectStep(second.lease, &child) == AuthorityCode::Allowed);
    auto parentAction = Action(human, second.lease);
    assert(authority.Authorize(move, current, 1300, &human, &parentAction) == AuthorityCode::StaleRevision);
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::Allowed);
    auto nextChild = child; ++nextChild.revision; nextChild.checkpoint.step = "collect_mail";
    assert(authority.SelectStep(second.lease, &nextChild) == AuthorityCode::Allowed);
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::StaleRevision);
    assert(authority.SelectStep(second.lease, &child) == AuthorityCode::StaleRevision);
    child = nextChild; childAction = Action(child, second.lease);
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::Allowed);
    assert(authority.SelectStep(second.lease, nullptr) == AuthorityCode::Allowed);
    assert(authority.Authorize(move, current, 1300, &human, &parentAction) == AuthorityCode::Allowed);
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::StaleRevision);
    child.phase = Phase::WaitingExternal;
    assert(authority.Authorize(move, current, 1300, &child, &childAction) == AuthorityCode::StaleLease);
    assert(authority.Release(second.lease).code == AuthorityCode::Released);
    assert(authority.Release(second.lease).code == AuthorityCode::StaleLease); // Handoff exactly once.
    // Expiry invalidates old execution, but cannot erase an atomic native receipt.
    first = authority.Acquire(t, Movement, 2000, 1000);
    assert(first.Granted());
    assert(authority.BeginAtomic(first.lease, C, 2100).code == AuthorityCode::Allowed);
    assert(authority.SelectStep(first.lease, nullptr) == AuthorityCode::AtomicPending);
    assert(authority.BeginAtomic(first.lease, C, 2100).code == AuthorityCode::AtomicPending);
    assert(authority.Acquire(human, Movement, 4000, 1000).code == AuthorityCode::AtomicPending);
    assert(authority.Release(first.lease).code == AuthorityCode::AtomicPending);
    // A map roundtrip has a new epoch even if map and instance IDs match again.
    ++current.mapGeneration;
    assert(authority.Observe(current, 0).code == AuthorityCode::ReconciliationRequired);
    assert(authority.FinishAtomic(first.lease, B).code == AuthorityCode::StaleLease);
    auto done = authority.FinishAtomic(first.lease, C);
    assert(done.code == AuthorityCode::ReconciliationRequired && done.displaced.generation == first.lease.generation);
    assert(authority.FinishAtomic(first.lease, C).code == AuthorityCode::StaleLease);
    assert(authority.Acquire(t, Movement, 4100, 1000).code == AuthorityCode::StaleContext);
    t.context = current; first = authority.Acquire(t, Movement, 4100, 1000);
    assert(first.Granted()); a = Action(t, first.lease);
    ++current.actorGeneration; // Logout/login does not revive earlier async work.
    auto loggedOut = authority.Observe(current, 0);
    assert(loggedOut.displaced.generation == first.lease.generation);
    assert(authority.Authorize(move, current, 4200, &t, &a) == AuthorityCode::StaleLease);
    assert(authority.Observe(Context(498), 0).code == AuthorityCode::Allowed);
    auto other = Root(B, Priority::Delivery, 498);
    auto otherLease = authority.Acquire(other, Movement, 4200, 1000);
    assert(otherLease.Granted());
    assert(authority.Observe(Context(499), 0).code == AuthorityCode::Capacity);
    assert(authority.Forget(497).code == AuthorityCode::Released && authority.Size() == 1);
    assert(authority.Inspect(498, 4300).lease.generation == otherLease.lease.generation);
    assert(authority.Observe(current, 0).code == AuthorityCode::Allowed);
    // All priority bands respect human precedence; accepted wins within one band.
    for (auto band : {Priority::Preparation, Priority::Scheduled, Priority::Delivery, Priority::Progression, Priority::Optional}) {
        t = Root(A, band); t.context = current; first = authority.Acquire(t, Movement, 5000, 1000);
        assert(first.Granted()); human = Root(B, Priority::Human); human.context = current;
        second = authority.Acquire(human, Movement, 5000, 1000);
        assert(second.Granted()); authority.Release(second.lease);
    }
    t = Root(); t.context = current; t.accepted = false; t.dueAtMs = 1;
    first = authority.Acquire(t, Movement, 5000, 1000); assert(first.Granted());
    auto accepted = Root(B); accepted.context = current; accepted.dueAtMs = 999999;
    second = authority.Acquire(accepted, Movement, 5000, 1000); assert(second.Granted());
    a = Action(accepted, second.lease);
    assert(authority.Authorize(move, current, 6000, &accepted, &a) == AuthorityCode::StaleLease);
    auto refreshed = authority.Acquire(accepted, Movement, 6000, 1000);
    assert(refreshed.Granted() && refreshed.lease.generation != second.lease.generation);
    assert(authority.Release(second.lease).code == AuthorityCode::StaleLease);
    authority.Release(refreshed.lease);
    accepted.ownerGeneration = 10000;
    refreshed = authority.Acquire(accepted, Movement, 7000, 1000);
    assert(refreshed.lease.generation > 10000); // Restart never regresses a durable generation.
    authority.Release(refreshed.lease);
    accepted.ownerGeneration = std::numeric_limits<uint64_t>::max();
    assert(authority.Acquire(accepted, Movement, 7000, 1000).code == AuthorityCode::GenerationExhausted);
    accepted = Root(); accepted.context = current; accepted.mode = Mode::Observe;
    assert(authority.Acquire(accepted, Movement, 7000, 1000).code == AuthorityCode::InvalidRequest);
    // Even an incorrectly broad native adapter cannot waive unrelated safety.
    ExecutionAuthority exceptions; current = Context(); exceptions.Observe(current, 0);
    for (auto lane : {Lane::Combat, Lane::Healing, Lane::Loot, Lane::Roll, Lane::LocalQuest}) {
        const auto mask = lane == Lane::Roll ? Mask(Effect::Social) :
            lane == Lane::LocalQuest || lane == Lane::Loot ? Mask(Effect::Inventory) : Mask(Effect::Spell);
        NativePermit permit{current, lane, mask, uint32_t(Safety::Combat), true};
        const Effects native{mask, lane, true};
        exceptions.Observe(current, uint32_t(Safety::Combat));
        assert(exceptions.Authorize(native, current, 1, nullptr, nullptr, &permit) == AuthorityCode::Allowed);
        for (auto unsafe : {Safety::Death, Safety::Transfer, Safety::Taxi, Safety::Transport, Safety::Falling, Safety::UnsafeOperation}) {
            exceptions.Observe(current, uint32_t(unsafe));
            assert(exceptions.Authorize(native, current, 1, nullptr, nullptr, &permit) == AuthorityCode::SafetyPaused);
            auto broad = permit; broad.allowedSafety |= uint32_t(unsafe);
            assert(exceptions.Authorize(native, current, 1, nullptr, nullptr, &broad) == AuthorityCode::EffectsDenied);
        }
        exceptions.Observe(current, 0); permit.effects = AllEffects;
        assert(exceptions.Authorize(native, current, 1, nullptr, nullptr, &permit) == AuthorityCode::EffectsDenied);
    }
    NativePermit transport{current, Lane::Safety, Mask(Effect::Movement), uint32_t(Safety::Transport), true};
    exceptions.Observe(current, uint32_t(Safety::Transport));
    assert(exceptions.Authorize({Mask(Effect::Movement), Lane::Safety, true}, current, 1, nullptr, nullptr, &transport) == AuthorityCode::Allowed);
    transport.allowedSafety |= uint32_t(Safety::Combat);
    assert(exceptions.Authorize({Mask(Effect::Movement), Lane::Safety, true}, current, 1, nullptr, nullptr, &transport) == AuthorityCode::EffectsDenied);
    NativePermit acknowledgement{current, Lane::Social, Mask(Effect::Social), 127, true};
    exceptions.Observe(current, 127);
    assert(exceptions.Authorize({Mask(Effect::Social), Lane::Social, true}, current, 1, nullptr, nullptr, &acknowledgement) == AuthorityCode::Allowed);
    acknowledgement.effects |= Mask(Effect::Movement);
    assert(exceptions.Authorize({Mask(Effect::Social), Lane::Social, true}, current, 1, nullptr, nullptr, &acknowledgement) == AuthorityCode::EffectsDenied);
    acknowledgement.effects = Mask(Effect::Social); acknowledgement.validated = false;
    assert(exceptions.Authorize({Mask(Effect::Social), Lane::Social, true}, current, 1, nullptr, nullptr, &acknowledgement) == AuthorityCode::EffectsDenied);
}
