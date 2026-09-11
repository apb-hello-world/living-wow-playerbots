#ifndef LIVING_ACTIVITY_GAMEPLAY_H
#define LIVING_ACTIVITY_GAMEPLAY_H
#include "LivingActivityEffects.h"
class PlayerbotAI;
class Unit;
class Item;
namespace LivingActivity {
    // Native roll ownership, not the cached 'active rolls' flag, establishes
    // eligibility. World context, effect limits and atomic-operation exclusion
    // are separately checked at the common permission boundary.
    template<class NativePlayer, class NativeRoll, class Vote>
    bool ReadyForNativeLootVote(NativePlayer& actor, NativeRoll* roll, Vote pending, bool humanWait) {
        return actor.IsInWorld() && !actor.IsBeingTeleported() && actor.GetGroup() && roll &&
            roll->GetPlayerVote(actor.GetObjectGuid()) == pending && !humanWait;
    }
    template<class NativePlayer, class NativeUnit>
    bool ReadyForNativeHealing(NativePlayer& actor, NativeUnit& target, bool known, bool healing, bool friendly) {
        return known && healing && friendly && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            target.IsInWorld() && target.IsAlive() && actor.GetMapId() == target.GetMapId() &&
            actor.GetInstanceId() == target.GetInstanceId();
    }
    template<class NativePlayer, class NativeUnit>
    bool ReadyForNativeCombatMovement(NativePlayer& actor, NativeUnit& target, bool hostile, bool partyAlly, bool engaged) {
        return (hostile || partyAlly) && engaged && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            target.IsInWorld() && target.IsAlive() && actor.GetMapId() == target.GetMapId() &&
            actor.GetInstanceId() == target.GetInstanceId();
    }
    template<class NativePlayer, class NativeUnit>
    bool ReadyForNativeOffense(NativePlayer& actor, NativeUnit& target, bool known, bool positive, bool hostile, bool engaged) {
        return known && !positive && hostile && engaged && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            target.IsInWorld() && target.IsAlive() && actor.GetMapId() == target.GetMapId() &&
            actor.GetInstanceId() == target.GetInstanceId();
    }
    constexpr uint32_t AttackEffectMask() { return Mask(Effect::Movement) | Mask(Effect::Spell); }
    template<class NativePlayer, class NativeUnit>
    bool ReadyForNativeEngagedAttack(NativePlayer& actor, NativeUnit& target, bool hostile, bool engaged, bool inSight) {
        return inSight && ReadyForNativeCombatMovement(actor, target, hostile, false, engaged);
    }
    template<class NativePlayer, class NativePet>
    bool ReadyForNativePetCaster(NativePlayer& actor, NativePet& pet, bool known, bool owned, bool ready) {
        return known && owned && ready && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            pet.IsInWorld() && pet.IsAlive() && actor.GetMapId() == pet.GetMapId() &&
            actor.GetInstanceId() == pet.GetInstanceId();
    }
    // Inspects the actual native spell, known-spell record, target and CheckCast.
    // No talent change, role inference, synthetic spell or rotation selection.
    constexpr uint32_t SpellEffectMask(bool inventoryFreeNativeSpell) {
        // Native casting can face the target and interrupt an existing move.
        return Mask(Effect::Spell) | Mask(Effect::Movement) | (inventoryFreeNativeSpell ? 0 : Mask(Effect::Inventory));
    }
    template<class NativeSpell, class Predicate>
    bool HasOnlyNativeEffects(const NativeSpell& spell, Predicate allowed) {
        bool present = false;
        for (const auto effect : spell.Effect) {
            if (!effect) continue;
            if (!allowed(effect)) return false;
            present = true;
        }
        return present;
    }
    template<class NativeSpell, class Predicate>
    bool HasOnlyNativeAuras(const NativeSpell& spell, Predicate allowed) {
        for (const auto aura : spell.EffectApplyAuraName) if (aura && !allowed(aura)) return false;
        return true;
    }
    template<class NativeSpell, class Predicate, class AuraPredicate>
    bool InventoryFreeNativeSpell(const NativeSpell& spell, bool known, bool itemCast,
        bool equipmentOrAmmunition, Predicate allowed, AuraPredicate allowedAura) {
        if (!known || itemCast || equipmentOrAmmunition || !HasOnlyNativeEffects(spell, allowed) ||
            !HasOnlyNativeAuras(spell, allowedAura)) return false;
        for (const auto trigger : spell.EffectTriggerSpell) if (trigger) return false;
        for (const auto reagent : spell.Reagent) if (reagent > 0) return false;
        return true;
    }
    template<class NativeSpell>
    bool InventoryFreeDirectHeal(const NativeSpell& spell, bool known, bool itemCast, uint32_t heal, uint32_t maxHeal) {
        if (!known || itemCast) return false;
        bool healing = false;
        for (const auto effect : spell.Effect) {
            if (effect == heal || effect == maxHeal) healing = true;
            else if (effect) return false;
        }
        for (const auto trigger : spell.EffectTriggerSpell) if (trigger) return false;
        for (const auto reagent : spell.Reagent) if (reagent > 0) return false;
        return healing;
    }
    // Only known, classified, reagent/trigger/equipment-free native effects can
    // shed inventory authority. Unknown effects and item/ammunition casts retain it.
    Effects NativeSpellEffects(PlayerbotAI& ai, uint32_t spell, bool itemCast = false);
    NativePermit NativeSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target, Item* item = nullptr);
    // The owner's spellbook is not a pet spellbook. This validates the actual
    // owned native pet and its native cast requirements before pet commands.
    NativePermit NativePetSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target);
    // Explicit combat-positioning callers only. Native victim/attacker/threat
    // relations establish engagement; a combat flag or a nearby NPC does not.
    NativePermit NativeCombatMovementPermit(PlayerbotAI& ai, Unit* target);
    // Weapon/pet attack control may continue an established native encounter.
    // Neither an old current-target value nor a combat flag authorizes a pull.
    NativePermit NativeEngagedAttackPermit(PlayerbotAI& ai, Unit* target);
}
#endif
