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
    bool ReadyForNativeOffense(NativePlayer& actor, NativeUnit& target, bool known, bool positive, bool hostile, bool engaged) {
        return known && !positive && hostile && engaged && actor.IsInWorld() && actor.IsAlive() && !actor.IsBeingTeleported() &&
            target.IsInWorld() && target.IsAlive() && actor.GetMapId() == target.GetMapId() &&
            actor.GetInstanceId() == target.GetInstanceId();
    }
    // Inspects the actual native spell, known-spell record, target and CheckCast.
    // No talent change, role inference, synthetic spell or rotation selection.
    constexpr uint32_t SpellEffectMask(bool inventoryFreeDirectHeal) {
        return Mask(Effect::Spell) | (inventoryFreeDirectHeal ? 0 : Mask(Effect::Inventory));
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
    // Conservative default. Only a known, direct, reagent-free native heal can
    // shed the inventory effect; unclassified spells retain it.
    Effects NativeSpellEffects(PlayerbotAI& ai, uint32_t spell, bool itemCast = false);
    NativePermit NativeSpellPermit(PlayerbotAI& ai, uint32_t spell, Unit* target, Item* item = nullptr);
}
#endif
