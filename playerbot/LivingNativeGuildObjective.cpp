#include "botpch.h"
#include "LivingNativeGuildObjective.h"
#include "LivingNativeGuildEvent.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityScope.h"
#include "LivingActivityNativeContext.h"
#include "TravelMgr.h"
#include "strategy/actions/AttackAction.h"
#include "strategy/values/FreeMoveValues.h"
#include "strategy/values/AttackersValue.h"
#include "strategy/values/PossibleAttackTargetsValue.h"

namespace LivingActivity {
namespace {
// Reuse the existing movement and attack mechanics, including pathfinding,
// party pull policy, pet handling and each direct mutation authority check.
class GuildQuestAction final : public ai::AttackAction {
public:
    explicit GuildQuestAction(PlayerbotAI* ai):AttackAction(ai,"guild quest objective") {}
    bool Pull(Unit* target){return isPossible() && Attack(nullptr,target);}
};
class GuildQuestApproach final : public ai::MovementAction {
public:
    explicit GuildQuestApproach(PlayerbotAI* ai):MovementAction(ai,"guild quest source") {}
    bool Approach(const ai::WorldPosition& point){return isPossible() && MoveTo2(point);}
};
}
std::string AdvanceNativeGuildQuestObjective(Player& actor,const Task& task,const ActionContext& action,ai::TravelTarget& route) {
    GuildEventCommitment event;std::string why;
    if(!sLivingActivityCoordinator.OnWorldThread() || !ExecutionScope::Matches(task,action) ||
        !DecodeGuildEventCommitment(task.checkpoint.data,event) || event.kind!="quest" ||
        task.checkpoint.step!="guild_event_objective" || !ValidateNativeGuildEventTask(actor,task,why))
        return why.empty()?"guild_event_objective_authority_required":why;
    auto* ai=actor.GetPlayerbotAI();
    if(!ai || !actor.IsInWorld() || ReadNativeSafety(actor,MovementFlags(MOVEFLAG_FALLING|MOVEFLAG_FALLINGFAR)) ||
        actor.GetTradeData() || actor.IsNonMeleeSpellCasted(false))return "guild_event_objective_safety_pause";
    if(actor.GetQuestStatus(event.target)!=QUEST_STATUS_INCOMPLETE || actor.GetQuestRewardStatus(event.target))
        return "guild_event_objective_no_longer_needed";
    auto* destination=dynamic_cast<ai::QuestObjectiveTravelDestination*>(route.GetDestination());
    if(!destination || destination->GetQuestId()!=event.target || !route.GetPosition() ||
        route.GetStatus()!=ai::TravelStatus::TRAVEL_STATUS_WORK || !route.IsDestinationActive())
        return "guild_event_exact_objective_route_required";
    if(destination->GetEntry()<=0)return "guild_event_objective_interaction_adapter_required";
    if(!sLivingActivityCoordinator.PermitEffects(*ai,{Mask(Effect::Movement)|Mask(Effect::TravelTarget),Lane::Managed,true},"guild quest objective"))
        return "guild_event_objective_authority_changed";
    auto* context=ai->GetAiObjectContext();
    if(!context->GetValue<bool>("can move around")->Get())return "guild_event_group_preparation_wait";
    // Native grind selection retains level/elite, death-area, tap, crowd-control
    // and party-distance policy. Its result must ALSO serve this exact objective.
    auto* target=context->GetValue<Unit*>("grind target")->Get();
    if(target && target->GetTypeId()==TYPEID_UNIT && target->IsAlive() && target->IsInWorld() &&
        target->GetMap()==actor.GetMap() && target->GetEntry()==uint32(destination->GetEntry()) &&
        ai::CanFreeMoveValue::CanFreeTarget(ai,ai::GuidPosition(target)) &&
        ai::AttackersValue::IsValid(target,&actor,nullptr,false,false) &&
        ai::PossibleAttackTargetsValue::IsPossibleTarget(target,&actor,sPlayerbotAIConfig.sightDistance,false)) {
        // The current acknowledged objective authorizes this exact, natively
        // eligible pull. Combat is not an inventory transaction. Confine the
        // native permit to this synchronous call; do not relax managed Spell
        // journalling or grant autonomous grind a standing combat exception.
        auto permit=sLivingActivityCoordinator.NativeActionContext(*ai,Lane::Combat,AttackEffectMask(),uint32_t(Safety::Combat));
        if(permit.world.actor!=task.actor)return "guild_event_objective_authority_changed";
        permit.validated=true;ExecutionScope pull(permit);
        if(!sLivingActivityCoordinator.PermitEffects(*ai,{AttackEffectMask(),Lane::Combat,true},"guild quest pull"))
            return "guild_event_objective_authority_changed";
        GuildQuestAction native(ai);
        return native.Pull(target)?"guild_event_native_objective_pull_started":"guild_event_native_objective_pull_rejected";
    }
    // WORK means inside the broad destination region, not at an actual live
    // source. Native travel stops here; continue to the selected spawn point.
    const auto& point=*route.GetPosition();
    if(point.getMapId()!=actor.GetMapId())return "guild_event_objective_map_revalidation";
    if(ai::WorldPosition(&actor).distance(point)>8.0f) {
        GuildQuestApproach native(ai);
        return native.Approach(point)?"guild_event_approaching_objective_source":"guild_event_objective_source_route_blocked";
    }
    return "guild_event_waiting_for_objective_source";
}
}
