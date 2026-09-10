
#include "playerbot/playerbot.h"
#include "LootRollAction.h"
#include "playerbot/strategy/values/ItemUsageValue.h"
#include "playerbot/strategy/values/LootValues.h"
#include "playerbot/PlayerbotBuildProfile.h"
#include "playerbot/LivingActivityCoordinator.h"
#include "playerbot/LivingActivityScope.h"
#include "playerbot/LivingActivityGameplay.h"

using namespace ai;

namespace
{
    GroupLootRoll* GetGroupLootRoll(Player* bot, ObjectGuid lootGuid, uint32 slot)
    {
        Loot* loot = sLootMgr.GetLoot(bot, lootGuid);
        return loot ? loot->GetRollForSlot(slot) : NULL;
    }

    bool OtherBotMainNeeds(Player* bot, ItemQualifier& itemQualifier)
    {
        if (!bot || !bot->GetGroup()) return false;
        Group::MemberSlotList const& slots = bot->GetGroup()->GetMemberSlots();
        for (Group::MemberSlotList::const_iterator i = slots.begin(); i != slots.end(); ++i)
            if (Player* member = sObjectAccessor.FindPlayer(i->guid))
                if (member != bot && member->GetPlayerbotAI())
                {
                    ItemUsage other = member->GetPlayerbotAI()->GetAiObjectContext()->
                        GetValue<ItemUsage>("item usage", itemQualifier.GetQualifier())->Get();
                    if (other == ItemUsage::ITEM_USAGE_EQUIP || other == ItemUsage::ITEM_USAGE_FORCE_NEED)
                        return true;
                }
        return false;
    }

    RollVote ApplyMixedPartyLootPolicy(Player* bot, ItemQualifier& itemQualifier, ItemUsage usage,
        RollVote vote, GroupLootRoll* roll, bool& offspecNeed)
    {
        offspecNeed = false;
        if (!bot || !roll || !sPlayerbotPartyCombatCoordinator.IsActiveMixedParty(bot))
            return vote;

        if (sPlayerbotPartyCombatCoordinator.HumanNeededLoot(bot, roll))
            return vote == ROLL_NEED ? ROLL_GREED : vote;

        const bool mainNeed = usage == ItemUsage::ITEM_USAGE_EQUIP || usage == ItemUsage::ITEM_USAGE_FORCE_NEED;
        if (mainNeed)
            return ROLL_NEED;

        if (OtherBotMainNeeds(bot, itemQualifier))
            return vote == ROLL_NEED ? ROLL_GREED : vote;

        ItemPrototype const* proto = itemQualifier.GetProto();
        if (proto && sPlayerbotBuildProfiles.OffspecLootEnabled() &&
            sPlayerbotBuildProfiles.IsOffspecUpgrade(bot, proto) &&
            sPlayerbotBuildProfiles.CanCarryOffspecItem(bot, proto))
        {
            offspecNeed = true;
            return ROLL_NEED;
        }

        if (sPlayerbotPartyCombatCoordinator.BotCanNeedForUsage(usage)) return ROLL_NEED;

        return vote == ROLL_NEED ? ROLL_GREED : vote;
    }

    void AnnounceMixedPartyNeed(PlayerbotAI* ai, ItemQualifier& itemQualifier, ItemUsage usage, bool offspecNeed)
    {
        if (!ai || !sPlayerbotPartyCombatCoordinator.ShouldAnnounceLootNeed())
            return;

        std::string reason = offspecNeed ? "That would be a strong upgrade for my off-spec set." :
            ItemUsageValue::ReasonForNeed(usage, itemQualifier, 1, ai->GetBot());
        ai->SayToParty((offspecNeed ? "" : "I'll need on " + ChatHelper::formatItem(itemQualifier) + " ") + reason, true,
            PlayerbotAI::ChatMessageClass::social);
    }
}

LivingActivity::NativePermit RollAction::GetNativeActivityPermit(Event&) {
    using namespace LivingActivity;
    auto permit = sLivingActivityCoordinator.NativeActionContext(*ai, Lane::Roll,
        Mask(Effect::Inventory) | Mask(Effect::Social), uint32_t(Safety::Combat));
    if (!permit.world.actor || !bot->GetGroup()) return {};
    const LootRollMap rolls = AI_VALUE(LootRollMap, "active rolls");
    for (const auto& entry : rolls) {
        auto* roll = GetGroupLootRoll(bot, entry.first, entry.second);
        if (ReadyForNativeLootVote(*bot, roll, ROLL_NOT_EMITED_YET,
            roll && sPlayerbotPartyCombatCoordinator.ShouldDeferLootRoll(bot, roll))) {
            permit.validated = true; return permit;
        }
    }
    return {};
}

bool LootStartRollAction::Execute(Event& event)
{
    WorldPacket p(event.getPacket()); //WorldPacket packet for CMSG_LOOT_ROLL, (8+4+1)
    ObjectGuid creatureGuid;
    uint32 itemSlot;
    uint32 itemId;
    uint32 randomSuffix;
    int32 randomPropertyId;
#ifdef MANGOSBOT_TWO
    uint32 mapId;
    uint32 count;
#endif 
    uint32 timeout;

    p.rpos(0); //reset packet pointer
    p >> creatureGuid; //creature guid what we're looting
#ifdef MANGOSBOT_TWO
    p >> mapId; /// 3.3.3 mapid
#endif 
    p >> itemSlot; // the itemEntryId for the item that shall be rolled for
    p >> itemId; // the itemEntryId for the item that shall be rolled for
    p >> randomSuffix; // randomSuffix
    p >> randomPropertyId; // item random property ID
#ifdef MANGOSBOT_TWO
    p >> count; // items in stack
#endif 
    p >> timeout;  // the countdown time to choose "need" or "greed"

    LootRollMap lootRolls = AI_VALUE(LootRollMap, "active rolls");

    if (lootRolls.find(creatureGuid) != lootRolls.end())
        return false;

    Loot* loot = sLootMgr.GetLoot(bot, creatureGuid);
    if (!loot)
        return false;

    for(uint8 i=0;i< MAX_NR_LOOT_ITEMS;i++)
        if(loot->GetRollForSlot(i))
            lootRolls.insert({ creatureGuid, i });
        
    ActiveRolls::CleanUp(bot,lootRolls);

    SET_AI_VALUE(LootRollMap, "active rolls", lootRolls);

    return false;
}

bool RollAction::Execute(Event& event)
{      
    Player* requester = event.getOwner() ? event.getOwner() : GetMaster();
    std::string text = event.getParam();

    if (text.empty())
    {
        ai->TellPlayerNoFacing(requester, "Please give a roll type or item. See " + ChatHelper::formatValue("help", "action:roll", "roll help") + " for more information.");
        return false;
    }

    ItemIds ids = ChatHelper::parseItems(text);

    std::string type = "auto";
    if (ids.empty())
        type = text;
    else
        type = text.substr(0, text.find(" "));

    if (type == "emote")
    {
        std::vector<std::string> args = ChatHelper::splitString(text, " ");

        if (args.size() == 2)
            args = { args[0], "1", args[1] };
        if (args.size() == 1)
            args = { args[0], "1", "100" };

        for (char& d : args[1]) //Check if itemId contains only numbers
            if (!isdigit(d))
                return false;

        for (char& d : args[2]) //Check if itemId contains only numbers
            if (!isdigit(d))
                return false;

        WorldPacket data(MSG_RANDOM_ROLL);
        data << stoi(args[1]);
        data << stoi(args[2]);
        bot->GetSession()->HandleRandomRollOpcode(data);

        return true;
    }

    bool rollFeedback = AI_VALUE2(bool, "manual bool", "roll feedback");

    if (type == "feedback")
    {
        rollFeedback = !rollFeedback;

        if (!rollFeedback)
            ai->TellPlayerNoFacing(requester, "Roll feedback disabled.");
        else
            ai->TellPlayerNoFacing(requester, "Roll feedback enalbed.");

        SET_AI_VALUE2(bool, "manual bool", "roll feedback", rollFeedback);

        return true;
    }

    if (!bot->GetGroup())
        return false;

    if (AI_VALUE(LootRollMap, "active rolls").empty())
        return false;

    if (AI_VALUE(uint8, "bag space") >= 100)
        return false;

    if (type != "need" && type != "greed" && type != "pass" && type != "auto")
    {
        ai->TellPlayerNoFacing(requester, "Please give a correct roll type. need, greed, pass or auto. See " + ChatHelper::formatValue("help", "action:roll", "roll help") + " for more information.");
        return false;
    }

    RollVote vote = ROLL_NOT_VALID;

    if (type.find("need") == 0)
        vote = ROLL_NEED;
    else if (type.find("greed") == 0)
        vote = ROLL_GREED;
    else if (type.find("pass") == 0)
        vote = ROLL_PASS;

    uint32 rolledItems = 0;

    LootRollMap lootRolls = AI_VALUE(LootRollMap, "active rolls");

    for (auto roll : lootRolls)
    {
        ItemQualifier itemQualifier = GetRollItem(roll.first, roll.second);

        if (!itemQualifier.GetId())
            continue;

        if (!ids.empty() && ids.find(itemQualifier.GetId()) == ids.end())
            continue;

        RollVote doVote = vote;
        if (doVote == ROLL_NOT_VALID) //Auto
            doVote = CalculateRollVote(itemQualifier);

        rolledItems += RollOnItemInSlot(doVote, roll.first, roll.second);     
    }

    return rolledItems;
}

ItemQualifier RollAction::GetRollItem(ObjectGuid lootGuid, uint32 slot)
{
    Loot* loot = sLootMgr.GetLoot(bot, lootGuid);
    if (!loot)
        return ItemQualifier();

    LootItem* item = loot->GetLootItemInSlot(slot);

    if (!item)
        return ItemQualifier();

    return ItemQualifier(item);
}

RollVote RollAction::CalculateRollVote(ItemQualifier& itemQualifier)
{
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemQualifier.GetQualifier());

    RollVote needVote = ROLL_PASS;
    switch (usage)
    {
    case ItemUsage::ITEM_USAGE_EQUIP:
    case ItemUsage::ITEM_USAGE_GUILD_TASK:
    case ItemUsage::ITEM_USAGE_FORCE_NEED:
        needVote = ROLL_NEED;
        break;
    case ItemUsage::ITEM_USAGE_SKILL:
    case ItemUsage::ITEM_USAGE_USE:
    case ItemUsage::ITEM_USAGE_AH:
    case ItemUsage::ITEM_USAGE_BROKEN_AH:
    case ItemUsage::ITEM_USAGE_VENDOR:
    case ItemUsage::ITEM_USAGE_FORCE_GREED:
        needVote = ROLL_GREED;
        break;
    case ItemUsage::ITEM_USAGE_DISENCHANT:
#ifndef MANGOSBOT_TWO
        needVote = ROLL_GREED;
#else
        needVote = ROLL_DISENCHANT;
#endif
        break;
    }

    // special case for bad equip
    if (usage == ItemUsage::ITEM_USAGE_BAD_EQUIP)
    {
        bool shouldEquipBadItems = sPlayerbotAIConfig.rollBadItemsWithPlayer || !ai->HasRealPlayerMaster();
        if (shouldEquipBadItems)
            needVote = ROLL_NEED;
        else
            needVote = ROLL_GREED;
    }

    bool canLoot = StoreLootAction::IsLootAllowed(itemQualifier, bot->GetPlayerbotAI());

    if (AI_VALUE2(bool, "manual bool", "roll feedback"))
    {
        std::string reason = "because it can not be looted.";
        std::string vote = "Passing";
        if(canLoot)
            reason = ItemUsageValue::ReasonForNeed(usage, itemQualifier, 1, bot);

        if (needVote == ROLL_GREED)
            vote = "Rolling greed";
        else if (needVote == ROLL_NEED)
            vote = "Rolling need";
        else if (needVote == ROLL_DISENCHANT)
            vote = "Rolling disenchant";

         ai->TellPlayerNoFacing(ai->GetMaster(), vote + " on " + ChatHelper::formatItem(itemQualifier) + " " + reason);
    }

    return canLoot ? needVote : ROLL_PASS;
}

bool RollAction::RollOnItemInSlot(RollVote vote, ObjectGuid lootGuid, uint32 slot)
{
    Loot* loot = sLootMgr.GetLoot(bot, lootGuid);
    if (!loot)
        return false;

    LootItem* item = loot->GetLootItemInSlot(slot);
    if (!item) return false;
    ItemPrototype const* proto = sItemStorage.LookupEntry<ItemPrototype>(item->itemId);
    if (!proto)
        return false;

    GroupLootRoll* lootRoll = loot->GetRollForSlot(slot);
    if (!LivingActivity::ReadyForNativeLootVote(*bot, lootRoll, ROLL_NOT_EMITED_YET,
        lootRoll && sPlayerbotPartyCombatCoordinator.ShouldDeferLootRoll(bot, lootRoll)))
        return false;

    using namespace LivingActivity;
    auto permit = sLivingActivityCoordinator.NativeActionContext(*ai, Lane::Roll,
        Mask(Effect::Inventory) | Mask(Effect::Social), uint32_t(Safety::Combat));
    permit.validated = permit.world.actor && bot->GetGroup(); // Exact native roll and pending voter checked above.
    ExecutionScope nativeRoll(permit);
    if (!sLivingActivityCoordinator.PermitEffects(*ai, GetActivityEffects(), "native loot vote")) return false;
    bool didRoll = lootRoll->PlayerVote(bot, vote);

    if (didRoll)
    {
        LootRollMap lootRolls = AI_VALUE(LootRollMap, "active rolls");

        ActiveRolls::CleanUp(bot, lootRolls, lootGuid, slot);

        SET_AI_VALUE(LootRollMap, "active rolls", lootRolls);
    }

    return didRoll;
}

bool LootRollAction::Execute(Event& event)
{
    Player* bot = QueryItemUsageAction::ai->GetBot();

    WorldPacket p(event.getPacket()); //WorldPacket packet for CMSG_LOOT_ROLL, (8+4+1)
    ObjectGuid guid;
    uint32 slot;
    uint8 rollType;
    p.rpos(0); //reset packet pointer
    p >> guid; //guid of the item rolled
    p >> slot; //number of players invited to roll
    p >> rollType; //need,greed or pass on roll

    ItemQualifier itemQualifier = GetRollItem(guid, slot);

    if (!itemQualifier.GetId())
        return false;

    GroupLootRoll* lootRoll = GetGroupLootRoll(bot, guid, slot);
    if (!lootRoll || sPlayerbotPartyCombatCoordinator.ShouldDeferLootRoll(bot, lootRoll))
        return false;

    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemQualifier.GetQualifier());
    bool offspecNeed = false;
    RollVote vote = ApplyMixedPartyLootPolicy(bot, itemQualifier, usage, CalculateRollVote(itemQualifier), lootRoll, offspecNeed);

    bool rolled = RollOnItemInSlot(vote, guid, slot);
    if (rolled && vote == ROLL_NEED)
        AnnounceMixedPartyNeed(ai, itemQualifier, usage, offspecNeed);
    return rolled;
}

bool AutoLootRollAction::Execute(Event& event)
{
    LootRollMap lootRolls = AI_VALUE(LootRollMap, "active rolls");

    std::vector<LootRollMap::value_type> eligibleRolls;
    for (LootRollMap::const_iterator i = lootRolls.begin(); i != lootRolls.end(); ++i)
    {
        GroupLootRoll* lootRoll = GetGroupLootRoll(bot, i->first, i->second);
        if (lootRoll && !sPlayerbotPartyCombatCoordinator.ShouldDeferLootRoll(bot, lootRoll))
            eligibleRolls.push_back(*i);
    }
    if (eligibleRolls.empty())
        return false;

    LootRollMap::value_type const& currentRoll = eligibleRolls[urand(0, eligibleRolls.size() - 1)];

    ItemQualifier itemQualifier = GetRollItem(currentRoll.first, currentRoll.second);

    if (!itemQualifier.GetId())
        return false;

    GroupLootRoll* lootRoll = GetGroupLootRoll(bot, currentRoll.first, currentRoll.second);
    ItemUsage usage = AI_VALUE2(ItemUsage, "item usage", itemQualifier.GetQualifier());
    bool offspecNeed = false;
    RollVote vote = ApplyMixedPartyLootPolicy(bot, itemQualifier, usage, CalculateRollVote(itemQualifier), lootRoll, offspecNeed);

    bool rolled = RollOnItemInSlot(vote, currentRoll.first, currentRoll.second);
    if (rolled && vote == ROLL_NEED)
        AnnounceMixedPartyNeed(ai, itemQualifier, usage, offspecNeed);
    return rolled;
}

bool AutoLootRollAction::isPossible()
{
    // A full bot still has to submit PASS. CalculateRollVote and the mixed-party
    // policy already prevent it from needing an item it cannot store.
    if (!bot->GetGroup())
        return false;

    LootRollMap lootRolls = AI_VALUE(LootRollMap, "active rolls");
    for (LootRollMap::const_iterator i = lootRolls.begin(); i != lootRolls.end(); ++i)
    {
        GroupLootRoll* lootRoll = GetGroupLootRoll(bot, i->first, i->second);
        if (lootRoll && !sPlayerbotPartyCombatCoordinator.ShouldDeferLootRoll(bot, lootRoll))
            return true;
    }
    return false;
}
