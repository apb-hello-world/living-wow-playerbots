#ifndef LIVING_ACTIVITY_HUNTER_SUPPORT_H
#define LIVING_ACTIVITY_HUNTER_SUPPORT_H
#include <cstdint>

namespace LivingActivity {
    template<class NativeSpell>
    bool HunterSupportHasNoMaterialEffect(const NativeSpell& spell) {
        for (const auto item : spell.EffectItemType) if (item) return false;
        for (const auto reagent : spell.Reagent) if (reagent > 0) return false;
        return true;
    }
    template<class NativeSpell>
    bool AuditedTbcHunterPetRecovery(const NativeSpell& spell, uint32_t actorClass) {
        // Pinned TBC native effects: Call Pet loads this hunter's saved pet;
        // Revive Pet restores that owned pet through the normal spell handler.
        // Neither tames, grants a pet, changes the roster, or spends inventory.
        // CheckCast still verifies the real saved pet, life state and mana.
        if (actorClass != 3 || !HunterSupportHasNoMaterialEffect(spell)) return false;
        for (const auto trigger : spell.EffectTriggerSpell) if (trigger) return false;
        for (const auto misc : spell.EffectMiscValue) if (misc) return false;
        if (spell.Effect[2] || spell.EffectApplyAuraName[0] || spell.EffectApplyAuraName[2]) return false;
        if (spell.Id == 883)
            return spell.SpellFamilyName == 0 && spell.Effect[0] == 56 &&
                !spell.Effect[1] && !spell.EffectApplyAuraName[1];
        // 982's second native effect is a dummy aura, not a general permission
        // for dummy/scripted auras. Match the exact original record.
        return spell.Id == 982 && spell.SpellFamilyName == 9 && spell.Effect[0] == 109 &&
            spell.Effect[1] == 6 && spell.EffectApplyAuraName[1] == 4;
    }
    template<class NativeSpell>
    bool AuditedTbcHunterHawk(const NativeSpell& spell, const NativeSpell* quickShots, uint32_t actorClass) {
        // The original Hawk ranks include a proc effect even without Improved
        // Hawk. Audit its actual Quick Shots payload rather than allowing all
        // proc-trigger auras, which can consume or create items.
        if (actorClass != 3 || spell.SpellFamilyName != 9 || !quickShots ||
            !HunterSupportHasNoMaterialEffect(spell) || !HunterSupportHasNoMaterialEffect(*quickShots)) return false;
        switch (spell.Id) {
            case 13165: case 14318: case 14319: case 14320:
            case 14321: case 14322: case 25296: case 27044: break;
            default: return false;
        }
        if (spell.Effect[0] != 6 || spell.Effect[1] != 6 || spell.Effect[2] ||
            spell.EffectApplyAuraName[0] != 124 || spell.EffectApplyAuraName[1] != 42 || spell.EffectApplyAuraName[2] ||
            spell.EffectTriggerSpell[0] || spell.EffectTriggerSpell[1] != 6150 || spell.EffectTriggerSpell[2]) return false;
        if (quickShots->Id != 6150 || quickShots->SpellFamilyName != 9 || quickShots->Effect[0] != 6 ||
            quickShots->Effect[1] || quickShots->Effect[2] || quickShots->EffectApplyAuraName[0] != 140 ||
            quickShots->EffectApplyAuraName[1] || quickShots->EffectApplyAuraName[2]) return false;
        for (const auto trigger : quickShots->EffectTriggerSpell) if (trigger) return false;
        for (const auto misc : spell.EffectMiscValue) if (misc) return false;
        for (const auto misc : quickShots->EffectMiscValue) if (misc) return false;
        return true;
    }
}
#endif
