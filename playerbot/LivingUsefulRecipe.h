#pragma once
#include "LivingProfessionPlan.h"
#include <cstdint>
#include <string>
class Player;
struct ItemPrototype;

namespace LivingActivity {
    // Native value snapshot shared by shopping and owned-book selection. This
    // is eligibility, never a learned-spell receipt or permission to execute.
    struct RecipeLearningFacts {
        uint32_t item=0,recipe=0,skill=0,skillValue=0,skillMaximum=0;
        uint32_t requiredSkill=0,greyAt=0,currentTier=0,learnedTier=0;
        uint32_t output=0,missingIntermediate=0;
        bool known=false,nativeUsable=false,identityAllowed=false,supported=false;
    };
    struct UsefulRecipe {
        bool useful=false;
        uint32_t priority=0,skillWindow=0;
        const char* reason="recipe_native_facts_unavailable";
    };
    inline UsefulRecipe EvaluateUsefulRecipe(const RecipeLearningFacts& facts) {
        auto reject=[](const char* reason) {return UsefulRecipe{false,0,0,reason};};
        if (!facts.item || !facts.recipe || !facts.skill) return reject("recipe_identity_unavailable");
        if (!LivingProfessions::Primary(facts.skill) && facts.skill!=185 && facts.skill!=129 && facts.skill!=356)
            return reject("recipe_profession_unsupported");
        if (facts.known) return reject("recipe_already_known");
        if (!facts.skillValue || !facts.skillMaximum || facts.skillValue>facts.skillMaximum)
            return reject("recipe_profession_not_learned");
        if (!facts.identityAllowed) return reject("recipe_profession_identity_protected");
        if (!facts.nativeUsable || facts.skillValue<facts.requiredSkill)
            return reject("recipe_native_prerequisite_unmet");
        if (!facts.supported) return reject("recipe_learning_effect_unsupported");
        if (facts.learnedTier>facts.currentTier)
            return {true,3,0,"recipe_advances_existing_profession_tier"};
        if (facts.output && facts.missingIntermediate)
            return {true,2,0,"recipe_required_intermediate"};
        if (facts.skillValue<facts.skillMaximum && facts.greyAt>facts.skillValue)
            return {true,1,std::min(facts.greyAt,facts.skillMaximum)-facts.skillValue,"recipe_profession_skill_advancement"};
        return reject("recipe_no_demonstrated_use");
    }
    inline bool PreferUsefulRecipe(const RecipeLearningFacts& candidate,const RecipeLearningFacts& current) {
        const auto a=EvaluateUsefulRecipe(candidate),b=EvaluateUsefulRecipe(current);
        if (!a.useful) return false;
        if (!b.useful) return true;
        if (a.priority!=b.priority) return a.priority>b.priority;
        if (a.skillWindow!=b.skillWindow) return a.skillWindow>b.skillWindow;
        if (candidate.recipe!=current.recipe) return candidate.recipe<current.recipe;
        return candidate.item<current.item;
    }
    // No database query, grants, purchase, cast, or new timer. Call on native
    // gameplay paths; concrete guild/equipment demands need their own validated
    // producer and cannot be inferred from an item's name or a model response.
    RecipeLearningFacts InspectUsefulRecipe(Player& actor,const ItemPrototype* item);
    bool RecipeBookAlreadyOwnedOrIncoming(Player& actor,uint32_t entry);
    inline const char* RecipePurchaseBlocker(const UsefulRecipe& purpose,bool ownedOrIncoming,bool bidPending,uint32_t quantity) {
        if (!purpose.useful) return purpose.reason;
        if (ownedOrIncoming) return "recipe_copy_owned_or_awaiting_collection";
        if (bidPending) return "recipe_bid_already_outstanding";
        if (quantity!=1) return "recipe_requires_only_one_copy";
        return "";
    }
}
