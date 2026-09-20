#include "playerbot/playerbot.h"
#include "LivingActivityLoot.h"
#include "LivingActivityCoordinator.h"
#include "LivingNativeGathering.h"
#include "ServerFacade.h"
#include "LootObjectStack.h"
#include "strategy/actions/AddLootAction.h"
#include "strategy/actions/LootAction.h"
#include "strategy/actions/MovementActions.h"
#include <algorithm>

namespace LivingActivity {
NativePermit NativeLootBookkeepingPermit(PlayerbotAI& ai) {
    auto* actor=ai.GetBot();const auto view=ai.ActivityPermissions().Inspect();
    if(!actor || !actor->IsInWorld() || !view || (!view->lease.actor && !view->commitmentEffects))return {};
    auto permit=sLivingActivityCoordinator.NativeActionContext(ai,Lane::State,0,127);
    permit.validated=permit.world.actor!=0;return permit;
}
NativePermit NativeCorpseLootPermit(PlayerbotAI& ai,uint64_t source,uint32_t effects,bool opened) {
    const uint32_t allowed=Mask(Effect::Movement)|Mask(Effect::Inventory)|Mask(Effect::Money);
    auto* actor=ai.GetBot();const ObjectGuid guid(source);
    if(!actor || !actor->IsInWorld() || !actor->IsAlive() || !guid.IsCreature() ||
        !effects || (effects&~allowed))return {};
    auto* corpse=ai.GetCreature(guid);
    if(!corpse || !corpse->IsInWorld() || corpse->IsAlive() || corpse->GetMap()!=actor->GetMap() ||
        sServerFacade.GetDeathState(corpse)!=CORPSE || !corpse->m_loot ||
        corpse->HasFlag(UNIT_FIELD_FLAGS,UNIT_FLAG_SKINNABLE) ||
        !corpse->HasFlag(UNIT_DYNAMIC_FLAGS,UNIT_DYNFLAG_LOOTABLE) ||
        !corpse->m_loot->CanLoot(actor) || corpse->m_loot->IsLootedFor(actor) ||
        actor->GetDistance(corpse)>std::min(sPlayerbotAIConfig.lootDistance,sPlayerbotAIConfig.sightDistance) ||
        HoldsManagedGatherLoot(ai,source))return {};
    if(opened && (actor->GetLootGuid()!=guid || sLootMgr.GetLoot(actor)!=corpse->m_loot ||
        actor->GetDistance(corpse)>INTERACTION_DISTANCE))return {};
    auto permit=sLivingActivityCoordinator.NativeActionContext(ai,Lane::Loot,effects,0);
    permit.validated=permit.world.actor!=0;return permit;
}
}
namespace ai {
LivingActivity::NativePermit AddLootAction::GetNativeActivityPermit(Event&) {
    return LivingActivity::NativeLootBookkeepingPermit(*ai);
}
LivingActivity::NativePermit AddAllLootAction::GetNativeActivityPermit(Event&) {
    return LivingActivity::NativeLootBookkeepingPermit(*ai);
}
LivingActivity::NativePermit LootAction::GetNativeActivityPermit(Event&) {
    const auto next=context->GetValue<LootObjectStack*>("available loot")->Get()->GetLoot(sPlayerbotAIConfig.lootDistance);
    const auto previous=context->GetValue<LootObject>("loot target")->Get();
    const auto mask=LivingActivity::Mask(LivingActivity::Effect::Movement);
    if(previous.guid && previous.guid!=next.guid &&
        !LivingActivity::NativeCorpseLootPermit(*ai,previous.guid.GetRawValue(),mask).validated)return {};
    return LivingActivity::NativeCorpseLootPermit(*ai,next.guid.GetRawValue(),mask);
}
LivingActivity::Effects OpenLootAction::GetActivityEffects() const {
    const auto mask=LivingActivity::Mask(LivingActivity::Effect::Movement);
    const auto source=context->GetValue<LootObject>("loot target")->Get().guid.GetRawValue();
    return {LivingActivity::NativeCorpseLootPermit(*ai,source,mask).validated?mask:
        mask|LivingActivity::Mask(LivingActivity::Effect::Inventory)|LivingActivity::Mask(LivingActivity::Effect::Spell),
        LivingActivity::Lane::Managed,true};
}
LivingActivity::NativePermit OpenLootAction::GetNativeActivityPermit(Event&) {
    return LivingActivity::NativeCorpseLootPermit(*ai,context->GetValue<LootObject>("loot target")->Get().guid.GetRawValue(),
        LivingActivity::Mask(LivingActivity::Effect::Movement));
}
LivingActivity::NativePermit MoveToLootAction::GetNativeActivityPermit(Event&) {
    return LivingActivity::NativeCorpseLootPermit(*ai,context->GetValue<LootObject>("loot target")->Get().guid.GetRawValue(),
        LivingActivity::Mask(LivingActivity::Effect::Movement));
}
LivingActivity::NativePermit StoreLootAction::GetNativeActivityPermit(Event& event) {
    WorldPacket packet(event.getPacket());if(packet.size()<8)return {};
    packet.rpos(0);ObjectGuid source;packet>>source;
    return LivingActivity::NativeCorpseLootPermit(*ai,source.GetRawValue(),GetActivityEffects().mask,true);
}
}
