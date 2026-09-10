#include "LivingActivityGameplay.h"
#include "LivingActivityScope.h"
#include <cassert>
using namespace LivingActivity;
namespace {
    struct NativePlayer {
        bool inWorld = true, transferring = false, grouped = true, alive = true;
        unsigned map = 1, instance = 0;
        bool IsInWorld() const { return inWorld; }
        bool IsBeingTeleported() const { return transferring; }
        const void* GetGroup() const { return grouped ? this : nullptr; }
        unsigned GetObjectGuid() const { return 497; }
        bool IsAlive() const { return alive; }
        unsigned GetMapId() const { return map; }
        unsigned GetInstanceId() const { return instance; }
    };
    struct NativeRoll {
        int vote = 7;
        int GetPlayerVote(unsigned guid) const { assert(guid == 497); return vote; }
    };
    struct NativeSpell {
        uint32_t Effect[3]{}, EffectTriggerSpell[3]{};
        int32_t Reagent[8]{};
    };
}
int main() {
    assert(AttackEffectMask() == (Mask(Effect::Movement) | Mask(Effect::Spell)));
    assert(SpellEffectMask(true) == (Mask(Effect::Spell) | Mask(Effect::Movement)));
    assert(SpellEffectMask(false) == (Mask(Effect::Spell) | Mask(Effect::Movement) | Mask(Effect::Inventory)));
    NativeSpell direct;
    assert(!InventoryFreeDirectHeal(direct, true, false, 10, 67));
    direct.Effect[0] = 10;
    assert(InventoryFreeDirectHeal(direct, true, false, 10, 67));
    assert(!InventoryFreeDirectHeal(direct, false, false, 10, 67));
    assert(!InventoryFreeDirectHeal(direct, true, true, 10, 67));
    direct.Reagent[7] = 1; assert(!InventoryFreeDirectHeal(direct, true, false, 10, 67)); direct.Reagent[7] = 0;
    direct.Effect[2] = 24; assert(!InventoryFreeDirectHeal(direct, true, false, 10, 67)); direct.Effect[2] = 0;
    direct.EffectTriggerSpell[1] = 1; assert(!InventoryFreeDirectHeal(direct, true, false, 10, 67)); direct.EffectTriggerSpell[1] = 0;
    direct.Effect[0] = 67; assert(InventoryFreeDirectHeal(direct, true, false, 10, 67));
    NativePlayer player; NativeRoll roll;
    assert(ReadyForNativeLootVote(player, &roll, 7, false));
    assert(!ReadyForNativeLootVote(player, &roll, 7, true));
    assert(!ReadyForNativeLootVote(player, static_cast<NativeRoll*>(nullptr), 7, false));
    roll.vote = 0; assert(!ReadyForNativeLootVote(player, &roll, 7, false)); roll.vote = 7;
    player.grouped = false; assert(!ReadyForNativeLootVote(player, &roll, 7, false)); player.grouped = true;
    player.transferring = true; assert(!ReadyForNativeLootVote(player, &roll, 7, false)); player.transferring = false;
    player.inWorld = false; assert(!ReadyForNativeLootVote(player, &roll, 7, false)); player.inWorld = true;
    NativePlayer target;
    NativePlayer pet;
    assert(ReadyForNativePetCaster(player, pet, true, true, true));
    assert(!ReadyForNativePetCaster(player, pet, false, true, true));
    assert(!ReadyForNativePetCaster(player, pet, true, false, true));
    assert(!ReadyForNativePetCaster(player, pet, true, true, false));
    pet.alive = false; assert(!ReadyForNativePetCaster(player, pet, true, true, true)); pet.alive = true;
    ++pet.map; assert(!ReadyForNativePetCaster(player, pet, true, true, true)); --pet.map;
    ++pet.instance; assert(!ReadyForNativePetCaster(player, pet, true, true, true)); --pet.instance;
    player.transferring = true; assert(!ReadyForNativePetCaster(player, pet, true, true, true)); player.transferring = false;
    assert(ReadyForNativeEngagedAttack(player, target, true, true, true));
    assert(!ReadyForNativeEngagedAttack(player, target, true, false, true));
    assert(!ReadyForNativeEngagedAttack(player, target, false, true, true));
    assert(!ReadyForNativeEngagedAttack(player, target, true, true, false));
    target.alive = false; assert(!ReadyForNativeEngagedAttack(player, target, true, true, true)); target.alive = true;
    ++target.instance; assert(!ReadyForNativeEngagedAttack(player, target, true, true, true)); --target.instance;
    assert(ReadyForNativeCombatMovement(player, target, true, false, true));
    assert(ReadyForNativeCombatMovement(player, target, false, true, true));
    assert(!ReadyForNativeCombatMovement(player, target, true, false, false));
    assert(!ReadyForNativeCombatMovement(player, target, false, true, false));
    assert(!ReadyForNativeCombatMovement(player, target, false, false, true));
    target.alive = false; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); target.alive = true;
    target.inWorld = false; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); target.inWorld = true;
    ++target.map; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); --target.map;
    ++target.instance; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); --target.instance;
    player.transferring = true; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); player.transferring = false;
    player.alive = false; assert(!ReadyForNativeCombatMovement(player, target, true, false, true)); player.alive = true;
    assert(ReadyForNativeHealing(player, target, true, true, true));
    assert(!ReadyForNativeHealing(player, target, false, true, true));
    assert(!ReadyForNativeHealing(player, target, true, false, true));
    assert(!ReadyForNativeHealing(player, target, true, true, false));
    target.alive = false; assert(!ReadyForNativeHealing(player, target, true, true, true)); target.alive = true;
    target.inWorld = false; assert(!ReadyForNativeHealing(player, target, true, true, true)); target.inWorld = true;
    ++target.map; assert(!ReadyForNativeHealing(player, target, true, true, true)); --target.map;
    ++target.instance; assert(!ReadyForNativeHealing(player, target, true, true, true)); --target.instance;
    player.transferring = true; assert(!ReadyForNativeHealing(player, target, true, true, true)); player.transferring = false;
    assert(ReadyForNativeOffense(player, target, true, false, true, true));
    assert(!ReadyForNativeOffense(player, target, false, false, true, true));
    assert(!ReadyForNativeOffense(player, target, true, true, true, true));
    assert(!ReadyForNativeOffense(player, target, true, false, false, true));
    assert(!ReadyForNativeOffense(player, target, true, false, true, false));
    player.alive = false;
    assert(!ReadyForNativeHealing(player, target, true, true, true));
    assert(!ReadyForNativeOffense(player, target, true, false, true, true));
    player.alive = true;
    target.alive = false; assert(!ReadyForNativeOffense(player, target, true, false, true, true)); target.alive = true;
    ++target.instance; assert(!ReadyForNativeOffense(player, target, true, false, true, true)); --target.instance;

    Task task; task.id = task.root = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    task.source = "profession"; task.sourceKey = "1"; task.actor = task.context.actor = 497;
    task.context.boot = "ff2efbdf-f0ec-4539-b840-299847970c00";
    task.context.actorGeneration = task.context.mapGeneration = task.context.policyRevision = 1;
    task.createdAtMs = task.updatedAtMs = 1; task.mode = Mode::Active; task.phase = Phase::Preparing;
    ExecutionAuthority authority; authority.Observe(task.context, 0);
    const auto lease = authority.Acquire(task, Mask(Effect::Movement), 100, 1000);
    assert(lease.Granted());
    PermissionPublisher publisher; const auto reader = publisher.Reader(); publisher.Publish(authority.Read(497));
    const Effects vote{Mask(Effect::Inventory) | Mask(Effect::Social), Lane::Roll, true};
    NativePermit permit{task.context, Lane::Roll, vote.mask, uint32_t(Safety::Combat), true};
    {
        ExecutionScope scope(permit);
        assert(ExecutionScope::Check(reader, vote, task.context, 200) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, vote, task.context, 200, uint32_t(Safety::Combat)) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Movement), Lane::Managed, true}, task.context, 200) == AuthorityCode::StaleLease);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Money), Lane::Roll, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {}, task.context, 200) == AuthorityCode::UnknownAction);
        const auto op = "ff2efbdf-f0ec-4539-b840-299847970c01";
        assert(authority.BeginAtomic(lease.lease, op, 200).code == AuthorityCode::Allowed);
        publisher.Publish(authority.Read(497));
        assert(ExecutionScope::Check(reader, vote, task.context, 200) == AuthorityCode::AtomicPending);
        authority.FinishAtomic(lease.lease, op); publisher.Publish(authority.Read(497));
    }
    assert(ExecutionScope::Check(reader, vote, task.context, 200) == AuthorityCode::EffectsDenied);
    const Effects combatMove{Mask(Effect::Movement), Lane::Combat, true};
    {
        const NativePermit attack{task.context, Lane::Combat, AttackEffectMask(), uint32_t(Safety::Combat), true};
        ExecutionScope scope(attack);
        assert(ExecutionScope::Check(reader, {AttackEffectMask(), Lane::Combat, true}, task.context, 200) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {AttackEffectMask(), Lane::Managed, true}, task.context, 200) != AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {Mask(Effect::TravelTarget), Lane::Combat, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Inventory), Lane::Combat, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {AttackEffectMask(), Lane::Combat, true}, task.context, 200,
            uint32_t(Safety::Transport)) == AuthorityCode::SafetyPaused);
    }
    {
        const NativePermit movement{task.context, Lane::Combat, combatMove.mask, uint32_t(Safety::Combat), true};
        ExecutionScope scope(movement);
        assert(ExecutionScope::Check(reader, combatMove, task.context, 200) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, combatMove, task.context, 200, uint32_t(Safety::Combat)) == AuthorityCode::Allowed);
        // A concrete combat mover is authorized; another generic movement
        // action still cannot borrow its native exception or change a journey.
        assert(ExecutionScope::Check(reader, {Mask(Effect::Movement), Lane::Managed, true}, task.context, 200) != AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {Mask(Effect::TravelTarget), Lane::Combat, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Inventory), Lane::Combat, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Spell), Lane::Combat, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        for (const auto safety : {Safety::Death, Safety::Transfer, Safety::Taxi, Safety::Transport, Safety::Falling})
            assert(ExecutionScope::Check(reader, combatMove, task.context, 200, uint32_t(safety)) != AuthorityCode::Allowed);
        auto stale = task.context; ++stale.mapGeneration;
        assert(ExecutionScope::Check(reader, combatMove, stale, 200) != AuthorityCode::Allowed);
    }
    assert(ExecutionScope::Check(reader, combatMove, task.context, 200) == AuthorityCode::EffectsDenied);
    const Effects spell{Mask(Effect::Spell) | Mask(Effect::Inventory), Lane::Healing, true};
    for (const auto lane : {Lane::Healing, Lane::Combat}) {
        NativePermit nativeSpell{task.context, lane, spell.mask, uint32_t(Safety::Combat), true};
        ExecutionScope scope(nativeSpell);
        const Effects effect{spell.mask, lane, true};
        assert(ExecutionScope::Check(reader, effect, task.context, 200) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, effect, task.context, 200, uint32_t(Safety::Combat)) == AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {Mask(Effect::TravelTarget), Lane::Managed, true}, task.context, 200) != AuthorityCode::Allowed);
        assert(ExecutionScope::Check(reader, {Mask(Effect::Money), lane, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        for (const auto safety : {Safety::Death, Safety::Transfer, Safety::Taxi, Safety::Transport, Safety::Falling})
            assert(ExecutionScope::Check(reader, effect, task.context, 200, uint32_t(safety)) != AuthorityCode::Allowed);
        const auto op = "ff2efbdf-f0ec-4539-b840-299847970c02";
        assert(authority.BeginAtomic(lease.lease, op, 200).code == AuthorityCode::Allowed);
        publisher.Publish(authority.Read(497));
        assert(ExecutionScope::Check(reader, effect, task.context, 200) == AuthorityCode::AtomicPending);
        const NativePermit freeHeal{task.context, Lane::Healing, SpellEffectMask(true), uint32_t(Safety::Combat), true};
        {
            ExecutionScope heal(freeHeal);
            // A database receipt for an unrelated purchase cannot stop a
            // reagent-free heal. Its scope still cannot consume any inventory.
            assert(ExecutionScope::Check(reader, {SpellEffectMask(true), Lane::Healing, true}, task.context, 200) == AuthorityCode::Allowed);
            assert(ExecutionScope::Check(reader, {SpellEffectMask(false), Lane::Healing, true}, task.context, 200) == AuthorityCode::EffectsDenied);
        }
        authority.FinishAtomic(lease.lease, op); publisher.Publish(authority.Read(497));
    }
    permit.validated = false;
    ExecutionScope unvalidated(permit);
    assert(ExecutionScope::Check(reader, vote, task.context, 200) == AuthorityCode::EffectsDenied);
}
