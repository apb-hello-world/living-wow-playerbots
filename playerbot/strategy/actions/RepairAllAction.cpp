
#include "playerbot/playerbot.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/PlayerbotRendezvousManager.h"
#include "playerbot/PlayerbotServiceTracking.h"
#include "RepairAllAction.h"
#include "playerbot/strategy/ItemVisitors.h"

#include "playerbot/ServerFacade.h"

using namespace ai;

bool RepairAllAction::Execute(Event& event)
{
    // This family has one executor during verified party free time, including
    // the admission/save gap before its first durable lease exists. Preserve
    // ordinary solo legacy repair until that family is migrated separately.
    if(sLivingActivityCoordinator.EffectEnforcementEnabled() &&
        sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands &&
        sPlayerbotRendezvousManager.IsPartyFreeTime(bot->GetGUIDLow()))return false;
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native service mutation")) return false;
    // Group leaders may remain here so followers can repair. That group trigger
    // does not mean the leader has damaged items of their own to repair.
    bool damaged = false;
    const auto repairMask = IterateItemsMask(uint8(IterateItemsMask::ITERATE_ITEMS_IN_EQUIP) | uint8(IterateItemsMask::ITERATE_ITEMS_IN_BAGS));
    for (Item* item : ai->InventoryParseItems("all", repairMask))
        if (item && item->GetUInt32Value(ITEM_FIELD_DURABILITY) < item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY))
        {
            damaged = true;
            break;
        }
    if (!damaged) return false;

    // Retain the legacy sound policy independently. Chat visibility is owned
    // by PlayerbotAI's scoped central boundary.
    bool suppressMaintenanceSound = sPlayerbotAIConfig.chatDirectorV2 &&
        sPlayerbotAIConfig.chatDirectorSuppressLegacyOperationalChat &&
        event.getSource() == "rpg action";
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::list<ObjectGuid> npcs = AI_VALUE(std::list<ObjectGuid>, "nearest npcs");
    for (std::list<ObjectGuid>::iterator i = npcs.begin(); i != npcs.end(); i++)
    {
        Creature *unit = bot->GetNPCIfCanInteractWith(*i, UNIT_NPC_FLAG_REPAIR);
        if (!unit)
            continue;

#ifdef MANGOS
        if(bot->hasUnitState(UNIT_STAT_DIED))
#endif
#ifdef CMANGOS
        if (bot->hasUnitState(UNIT_STAT_FEIGN_DEATH))
#endif
            bot->RemoveSpellsCausingAura(SPELL_AURA_FEIGN_DEATH);

        sServerFacade.SetFacingTo(bot, unit);
        float discountMod = bot->GetReputationPriceDiscount(unit);

        float durability = AI_VALUE(uint8, "durability inventory");

        const uint32 beforeDurability = PlayerbotServiceTracking::Durability(bot);
        uint32 botMoney = bot->GetMoney();
        if (ai->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(10000000);
        }

        //Repair weapons first.
        uint32 totalCost = bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_MAINHAND, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_RANGED, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepair((INVENTORY_SLOT_BAG_0 << 8) | EQUIPMENT_SLOT_OFFHAND, true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        totalCost += bot->DurabilityRepairAll(true, discountMod
#ifndef MANGOSBOT_ZERO
            , false
#endif
        );

        if (ai->HasCheat(BotCheatMask::gold))
        {
            bot->SetMoney(botMoney);
        }

        //Totalcost is bugged in core. For now we use this work-around.
        totalCost = botMoney - bot->GetMoney();

        const bool verifiedRepair = PlayerbotServiceTracking::Result(bot, "repair", unit->GetEntry(), 0,
            "durability_points", beforeDurability, PlayerbotServiceTracking::Durability(bot));

        if (totalCost > 0)
        {
            std::ostringstream out;
            out << "Repair: " << chat->formatMoney(totalCost) << " (" << unit->GetName() << ")";
            ai->TellPlayerNoFacing(requester, out.str(),PlayerbotSecurityLevel::PLAYERBOT_SECURITY_ALLOW_ALL, false);
            if (!suppressMaintenanceSound && sPlayerbotAIConfig.globalSoundEffects)
                bot->PlayDistanceSound(7994);

            sPlayerbotAIConfig.logEvent(ai, "RepairAllAction", std::to_string(durability), std::to_string(totalCost));

            ai->DoSpecificAction("equip upgrades", event, true);
        }

        SET_AI_VALUE(uint32, "death count", 0);
        RESET_AI_VALUE(uint8, "durability inventory");

        return verifiedRepair;
    }

    PlayerbotServiceTracking::Result(bot, "repair", 0, 0, "durability_points", 0, 0, true, "no_interactable_repair_npc");
    ai->TellPlayerNoFacing(requester, "Cannot find any npc to repair at");
    return false;
}
