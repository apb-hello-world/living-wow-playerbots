#include "LivingUsefulRecipe.h"
#include <cassert>
#include <limits>
using namespace LivingActivity;
int main() {
    RecipeLearningFacts recipe;
    recipe.item=6053;recipe.recipe=7255;recipe.skill=171;recipe.skillValue=100;
    recipe.skillMaximum=150;recipe.requiredSkill=100;recipe.greyAt=140;
    recipe.nativeUsable=recipe.identityAllowed=recipe.supported=true;
    assert(EvaluateUsefulRecipe(recipe).useful);
    assert(EvaluateUsefulRecipe(recipe).skillWindow==40);
    for (auto skill : LivingProfessions::Skills) {
        auto copy=recipe;copy.skill=skill;assert(EvaluateUsefulRecipe(copy).useful);
    }
    for (uint32_t skill : {185u,129u,356u}) {
        auto copy=recipe;copy.skill=skill;assert(EvaluateUsefulRecipe(copy).useful);
    }
    auto invalid=recipe;invalid.skill=43;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.known=true;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.skillValue=0;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.skillValue=151;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.skillValue=99;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.identityAllowed=false;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.nativeUsable=false;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.supported=false;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.greyAt=100;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.greyAt=0;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.skillValue=150;assert(!EvaluateUsefulRecipe(invalid).useful);
    // Grey recipes can serve an exact currently missing intermediate, not a
    // hypothetical craft or mere profession affiliation.
    invalid.greyAt=0;invalid.output=2454;invalid.missingIntermediate=1;
    assert(EvaluateUsefulRecipe(invalid).useful && EvaluateUsefulRecipe(invalid).priority==2);
    invalid.missingIntermediate=0;assert(!EvaluateUsefulRecipe(invalid).useful);
    // Existing secondary-profession training books must not become junk when
    // the current tier is capped; books cannot create a new primary identity.
    invalid.skill=129;invalid.currentTier=2;invalid.learnedTier=3;
    assert(EvaluateUsefulRecipe(invalid).useful && EvaluateUsefulRecipe(invalid).priority==3);
    invalid.skillValue=0;assert(!EvaluateUsefulRecipe(invalid).useful);
    invalid=recipe;invalid.greyAt=std::numeric_limits<uint32_t>::max();
    assert(EvaluateUsefulRecipe(invalid).skillWindow==50);
    assert(PreferUsefulRecipe(invalid,recipe));
    invalid=recipe;invalid.recipe--;assert(PreferUsefulRecipe(invalid,recipe));
    assert(!PreferUsefulRecipe(recipe,recipe));
    invalid=recipe;invalid.known=true;assert(!PreferUsefulRecipe(invalid,recipe));
    assert(PreferUsefulRecipe(recipe,invalid));
    const auto useful=EvaluateUsefulRecipe(recipe);
    assert(!*RecipePurchaseBlocker(useful,false,false,1));
    assert(*RecipePurchaseBlocker(useful,true,false,1));
    assert(*RecipePurchaseBlocker(useful,false,true,1));
    assert(*RecipePurchaseBlocker(useful,true,true,1));
    assert(*RecipePurchaseBlocker(useful,false,false,2));
    assert(*RecipePurchaseBlocker(useful,false,false,0));
    assert(*RecipePurchaseBlocker(EvaluateUsefulRecipe(invalid),false,false,1));
}
