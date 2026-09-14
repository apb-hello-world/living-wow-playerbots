#include "botpch.h"
#include "LivingNativeSkinning.h"
#include "LivingGathering.h"
#include "LootObjectStack.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/values/SharedValueContext.h"

namespace LivingActivity {
namespace {
bool SkinningDrop(uint32_t source,uint32_t entry) {
    const auto* info=sObjectMgr.GetCreatureTemplate(source);
    if(!info || info->GetRequiredLootSkill()!=SKILL_SKINNING || !info->SkinningLootId)return false;
    const auto* loot=ai::DropMapValue::GetLootTemplate(ObjectGuid(HIGHGUID_UNIT,source,uint32_t(1)),LOOT_SKINNING);
    if(!loot)return false;
    for(const auto& item:loot->Entries)if(item.itemid==entry && item.chance>0)return true;
    for(const auto& group:loot->Groups) {
        for(const auto& item:group.ExplicitlyChanced)if(item.itemid==entry && item.chance>0)return true;
        for(const auto& item:group.EqualChanced)if(item.itemid==entry)return true;
    }
    return false;
}
bool UsableSkinningCorpse(Player& actor,Creature* corpse,uint32_t entry,uint32_t& required) {
    required=0;
    if(!corpse || !actor.GetPlayerbotAI() || !corpse->IsInWorld() || corpse->IsAlive() || corpse->IsPet() ||
        sServerFacade.GetDeathState(corpse)!=CORPSE || actor.GetMap()!=corpse->GetMap() ||
        corpse->GetLootStatus()!=CREATURE_LOOT_STATUS_LOOTED ||
        !corpse->HasFlag(UNIT_FIELD_FLAGS,UNIT_FLAG_SKINNABLE) ||
        corpse->IsGroupLootRecipient() || (corpse->HasLootRecipient() && corpse->GetLootRecipientGuid()!=actor.GetObjectGuid()) ||
        (corpse->m_loot && corpse->m_loot->GetLootType()==LOOT_SKINNING) || !SkinningDrop(corpse->GetEntry(),entry) ||
        !actor.HasSpell(8613))return false;
    // Match the pinned core's CheckCast requirement, including the <100 skill
    // branch; its ordinary random orange failure remains native, not bypassed.
    const uint32_t skill=actor.GetSkillValue(SKILL_SKINNING),level=corpse->GetLevel();
    required=skill<100?(level>10?(level-10)*10:1):std::max(1u,uint32_t(level)*5);
    if(CheckGatheringSkill(skill,actor.GetSkillMax(SKILL_SKINNING),required,false,
        GatheringIntent::RequestedMaterials)!=GatheringSkillResult::Eligible)return false;
    ai::LootObject target(&actor,corpse->GetObjectGuid());
    return !target.IsEmpty() && target.skillId==SKILL_SKINNING && target.IsLootPossible(&actor);
}
}
bool LocalNativeSkinningQuote(Player& actor,uint64_t source,uint32_t entry,NativeGatherQuote& q,std::string& why) {
    q={};const ObjectGuid guid(source);auto* ai=actor.GetPlayerbotAI();uint32_t required=0;
    auto* corpse=ai && guid.IsCreature()?ai->GetCreature(guid):nullptr;
    if(!UsableSkinningCorpse(actor,corpse,entry,required) || !actor.IsWithinDistInMap(corpse,INTERACTION_DISTANCE)) {
        why="skinning_owned_looted_corpse_or_tools_unavailable";return false;
    }
    q={actor.GetGUIDLow(),entry,SKILL_SKINNING,8613,required,actor.GetSkillValuePure(SKILL_SKINNING),
        actor.GetSkillMaxPure(SKILL_SKINNING),actor.GetSkillValue(SKILL_SKINNING),actor.GetMoney(),actor.GetItemCount(entry,false),source};
    if(!ValidNativeGatherQuote(q)){why="skinning_native_quote_invalid";return false;}
    why.clear();return true;
}
void NativeSkinningSources(Player& actor,uint32_t entry,std::vector<int32_t>& out) {
    out.clear();auto* ai=actor.GetPlayerbotAI();if(!ai || !actor.HasSpell(8613))return;
    unsigned scanned=0;
    for(const auto guid:ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest corpses")->Get()) {
        if(++scanned>256)break;
        auto* corpse=ai->GetCreature(guid);uint32_t required=0;
        if(UsableSkinningCorpse(actor,corpse,entry,required) && !ai->ShouldAvoidDeathArea(ai::WorldPosition(corpse)))
            out.push_back(int32_t(corpse->GetEntry()));
    }
    std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
}
Creature* NativeSkinningNode(Player& actor,uint32_t entry,const std::vector<int32_t>& sources) {
    auto* ai=actor.GetPlayerbotAI();if(!ai)return nullptr;Creature* best=nullptr;float distance=FLT_MAX;unsigned scanned=0;
    for(const auto guid:ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest corpses")->Get()) {
        if(++scanned>256)break;
        auto* corpse=ai->GetCreature(guid);uint32_t required=0;
        if(!corpse || !std::binary_search(sources.begin(),sources.end(),int32_t(corpse->GetEntry())) ||
            !UsableSkinningCorpse(actor,corpse,entry,required) || ai->ShouldAvoidDeathArea(ai::WorldPosition(corpse)))continue;
        const auto d=actor.GetDistance(corpse);
        if(d<distance || (d==distance && best && guid<best->GetObjectGuid())){best=corpse;distance=d;}
    }
    return best;
}
bool HoldsNativeSkinningLoot(PlayerbotAI& ai,uint64_t source,uint32_t entry) {
    const ObjectGuid guid(source);auto* corpse=guid.IsCreature()?ai.GetCreature(guid):nullptr;
    // Ordinary kill loot is not intercepted. Only the separately generated
    // skinning loot belongs to this exact requested-material cast/collection.
    return corpse && corpse->m_loot && corpse->m_loot->GetLootType()==LOOT_SKINNING && SkinningDrop(corpse->GetEntry(),entry);
}
}
