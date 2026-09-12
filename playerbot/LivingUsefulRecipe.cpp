#include "playerbot/playerbot.h"
#include "LivingUsefulRecipe.h"
#include "PlayerbotOrganicEconomy.h"
#include "ServerFacade.h"
#include "Spells/SpellMgr.h"

namespace LivingActivity {
    RecipeLearningFacts InspectUsefulRecipe(Player& actor,const ItemPrototype* item) {
        RecipeLearningFacts facts;
        if (!item || item->Class!=ITEM_CLASS_RECIPE) return facts;
        facts.item=item->ItemId;
        // Decode only an actual teaching spell. A nonzero spell_3 is not proof
        // that an item teaches that spell. Conflicting teachers are unsupported.
        uint32_t taught=0;
#ifndef MANGOSBOT_ZERO
        if (item->Spells[0].SpellId==SPELL_ID_GENERIC_LEARN &&
            item->Spells[1].SpellTrigger==ITEM_SPELLTRIGGER_LEARN_SPELL_ID)
            taught=item->Spells[1].SpellId;
#endif
        if (!taught) for (const auto& use : item->Spells) {
            if (use.SpellTrigger!=ITEM_SPELLTRIGGER_ON_USE || !use.SpellId) continue;
            const auto* spell=sServerFacade.LookupSpellInfo(use.SpellId);
            if (!spell) return facts;
            for (uint8 i=0;i<MAX_EFFECT_INDEX;++i) if (spell->Effect[i]==SPELL_EFFECT_LEARN_SPELL) {
                if (!spell->EffectTriggerSpell[i] || (taught && taught!=spell->EffectTriggerSpell[i])) return facts;
                taught=spell->EffectTriggerSpell[i];
            }
        }
        const auto* learned=sServerFacade.LookupSpellInfo(taught);
        if (!learned) return facts;
        facts.recipe=taught;facts.known=actor.HasSpell(taught);
        facts.nativeUsable=actor.CanUseItem(item)==EQUIP_ERR_OK && actor.IsSpellFitByClassAndRace(taught);
        facts.identityAllowed=sPlayerbotOrganicEconomy.CanLearnProfessionSpell(&actor,taught);
        facts.requiredSkill=item->RequiredSkillRank;
        auto skill=[&](uint32_t id) {
            if (!id || (facts.skill && facts.skill!=id)) return false;
            facts.skill=id;return true;
        };
        if (item->RequiredSkill && !skill(item->RequiredSkill)) return facts;
        if (const auto* tier=sSpellMgr.GetSpellLearnSkill(taught)) {
            if (!skill(tier->skill)) {facts.skill=0;return facts;}
            facts.learnedTier=tier->step;facts.currentTier=actor.GetSkillStep(tier->skill);
            facts.supported=true;
        }
        const auto bounds=sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(taught);
        for (auto it=bounds.first;it!=bounds.second;++it) {
            const auto* line=it->second;
            if (!line || (!LivingProfessions::Primary(line->skillId) && line->skillId!=SKILL_COOKING &&
                line->skillId!=SKILL_FIRST_AID && line->skillId!=SKILL_FISHING)) continue;
            if ((line->racemask && !(line->racemask & actor.getRaceMask())) ||
                (line->classmask && !(line->classmask & actor.getClassMask()))) continue;
            if (!skill(line->skillId)) {facts.skill=0;return facts;}
            facts.greyAt=std::max(facts.greyAt,line->max_value);
            facts.requiredSkill=std::max(facts.requiredSkill,line->req_skill_value);
        }
        if (!facts.skill) return facts;
        facts.skillValue=actor.GetSkillValuePure(facts.skill);
        facts.skillMaximum=actor.GetSkillMaxPure(facts.skill);
        bool operation=false,unsupported=false;
        for (uint8 i=0;i<MAX_EFFECT_INDEX;++i) {
            if (!learned->Effect[i]) continue;
            switch (learned->Effect[i]) {
                case SPELL_EFFECT_CREATE_ITEM:
                    if (!learned->EffectItemType[i] || !sObjectMgr.GetItemPrototype(learned->EffectItemType[i]) ||
                        (facts.output && facts.output!=learned->EffectItemType[i])) unsupported=true;
                    facts.output=learned->EffectItemType[i];operation=true;break;
                case SPELL_EFFECT_ENCHANT_ITEM:
                case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                case SPELL_EFFECT_DISENCHANT: operation=true;break;
                default: if (!facts.learnedTier) unsupported=true;break;
            }
        }
        facts.supported=facts.supported || (operation && !unsupported);
        if (facts.output) {
            const auto required=sPlayerbotOrganicEconomy.RecipeMaterialQuantity(actor.GetGUIDLow(),facts.output);
            const auto owned=actor.GetItemCount(facts.output,true);
            facts.missingIntermediate=required>owned ? required-owned : 0;
        }
        return facts;
    }
}
