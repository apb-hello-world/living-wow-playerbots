#include "botpch.h"
#include "PlayerbotSocialActionBroker.h"
#include "PartyReleaseReadiness.h"
#include "PlayerbotPartyInvitationMgr.h"
#include "strategy/actions/MovementActions.h"

#include "PlayerbotActionBroker.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotChatDirector.h"
#include "PlayerbotLLMInterface.h"
#include "PlayerbotInventoryPressure.h"
#include "PlayerbotRendezvousManager.h"
#include "LootObjectStack.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/values/TravelValues.h"

#include <algorithm>
#include <regex>
#include <limits>
#include <sstream>
#include <thread>

PlayerbotSocialActionBroker& PlayerbotSocialActionBroker::instance()
{
    static PlayerbotSocialActionBroker broker;
    return broker;
}

bool PlayerbotSocialActionBroker::Supports(const std::string& type) const
{
    return sPlayerbotAIConfig.chatDirectorSocialActions && (type == "create_group_and_invite" || type == "invite_to_existing_group" ||
        type == "request_leader_invite" || type == "accept_group_invite" ||
        type == "pass_leadership" || type == "leave_group" ||
        type == "leave_ai_party_for_player" || type == "approve_party_departure" || type == "decline_party_departure" ||
        type == "solicit_petition_signatures" || type == "volunteer_for_guild_charter" ||
        type == "transfer_guild_leadership" ||
        type == "invite_to_guild" || type == "promote_guild_member" ||
        type == "demote_guild_member" || type == "remove_guild_member" ||
        type == "leave_guild" ||
        type == "perform_emote" || type == "wait_here" || type == "use_hearthstone" ||
        type == "share_quest" || type == "accept_party_quest_plan" || type == "meet_player" ||
        type == "vendor_bags" || type == "gather_node" || type == "decline_gather_node" ||
        type == "open_chest" || type == "decline_chest" ||
        type == "reserve_gathering_nodes" || type == "release_gathering_nodes" ||
        type == "ask_gathering_nodes" || type == "grant_party_free_time" ||
        type == "resume_party_assist");
}

static std::string GatheringPolicyKey(uint32 groupId, uint32 playerGuid, uint32 skillId)
{
    return std::to_string(groupId) + ':' + std::to_string(playerGuid) + ':' + std::to_string(skillId);
}

static std::string SharedObjectPartyKey(uint32 groupId, uint32 playerGuid)
{
    return std::to_string(groupId) + ':' + std::to_string(playerGuid);
}

static std::string SharedObjectKey(uint32 groupId, uint32 playerGuid, uint64 objectGuid)
{
    return SharedObjectPartyKey(groupId, playerGuid) + ':' + std::to_string(objectGuid);
}

static const char* GatheringSkillName(uint32 skillId)
{
    return skillId == SKILL_MINING ? "mining" : "herbalism";
}

static bool GroupHasRealHuman(Group* group)
{
    if (!group) return false;
    for (const auto& slot : group->GetMemberSlots())
        if (!sRandomPlayerbotMgr.IsRandomBot(slot.guid.GetCounter())) return true;
    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->getSource();
        if (member && member->IsInWorld() && member->isRealPlayer())
            return true;
    }
    return false;
}

static void SendSocialWhisper(Player* sender, Player* receiver, const std::string& message)
{
    if (!sender || !receiver)
        return;
    if (PlayerbotAI* ai = sender->GetPlayerbotAI())
        ai->Whisper(message, receiver->GetName(), false, PlayerbotAI::ChatMessageClass::social);
    else
        sender->Whisper(message, LANG_UNIVERSAL, receiver->GetObjectGuid());
}

static bool ValidatePetitionVolunteer(Player* bot, Player* owner, uint32 petitionGuid,
    uint32& signatureCount, uint32& required)
{
    signatureCount = 0;
    required = sWorld.getConfig(CONFIG_UINT32_MIN_PETITION_SIGNS);
    if (!bot || !owner || bot == owner || !bot->GetSession() || !bot->IsInWorld() || !owner->IsInWorld() ||
        bot->GetTeam() != owner->GetTeam() || bot->GetMapId() != owner->GetMapId() ||
        !bot->IsAlive() || bot->IsInCombat() || bot->GetGuildId() || bot->GetGuildIdInvited() ||
        bot->InBattleGround() || bot->IsTaxiFlying() || bot->GetTransport() ||
        owner->GetGuildId() || owner->GetGuildIdInvited())
        return false;
    Item* petition = owner->GetItemByEntry(5863);
    if (!petition || petition->GetObjectGuid().GetCounter() != petitionGuid)
        return false;
    auto petitionRow = CharacterDatabase.PQuery(
        "SELECT ownerguid FROM petition WHERE petitionguid = '%u' AND ownerguid = '%u'",
        petitionGuid, owner->GetGUIDLow());
    if (!petitionRow)
        return false;
    auto priorSignature = CharacterDatabase.PQuery(
        "SELECT playerguid FROM petition_sign WHERE player_account = '%u' AND petitionguid = '%u'",
        bot->GetSession()->GetAccountId(), petitionGuid);
    if (priorSignature)
        return false;
    auto signatures = CharacterDatabase.PQuery(
        "SELECT playerguid FROM petition_sign WHERE petitionguid = '%u'", petitionGuid);
    signatureCount = signatures ? signatures->GetRowCount() : 0;
    return signatureCount < required;
}

static bool HasPetitionSignature(Player* bot, uint32 petitionGuid)
{
    if (!bot || !bot->GetSession())
        return false;
    auto signature = CharacterDatabase.PQuery(
        "SELECT playerguid FROM petition_sign WHERE player_account = '%u' AND petitionguid = '%u'",
        bot->GetSession()->GetAccountId(), petitionGuid);
    return signature != nullptr;
}

bool PlayerbotSocialActionBroker::CanReleasePendingInvite(Player* bot) const
{
    Group* invite = bot ? bot->GetGroupInvite() : nullptr;
    if (!bot || bot->GetGroup() || !invite || invite->IsBattleGroup())
        return false;
    Player* leader = sObjectAccessor.FindPlayer(invite->GetLeaderGuid());
    if (!leader || leader->isRealPlayer() || !sRandomPlayerbotMgr.IsRandomBot(leader))
        return false;
    // Check persistent member slots too: an offline human still owns a seat.
    for (const auto& member : invite->GetMemberSlots())
        if (!sRandomPlayerbotMgr.IsRandomBot(member.guid.GetCounter()))
            return false;
    return true;
}

static bool LeaveAiOnlyParty(Player* bot, uint32 expectedGroupId)
{
    Group* group = bot ? bot->GetGroup() : nullptr;
    if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported() || !group || group->GetId() != expectedGroupId || GroupHasRealHuman(group) ||
        bot->IsInCombat() || bot->GetMap()->IsDungeon() || bot->InBattleGround() ||
        bot->IsTaxiFlying() || bot->GetTransport() || bot->IsBeingTeleported())
        return false;

    WorldPacket packet;
    packet << uint32(PARTY_OP_LEAVE) << bot->GetName() << uint32(0);
    bot->GetSession()->HandleGroupDisbandOpcode(packet);
    if (bot->GetGroup())
        return false;
    bot->GetPlayerbotAI()->SetMaster(nullptr);
    bot->GetPlayerbotAI()->RequestStrategyReset(true);
    bot->GetPlayerbotAI()->Reset();
    return true;
}

uint32 PlayerbotSocialActionBroker::ReservedForPlayer(uint32 botGuid)
{
    auto found = groupReservations.find(botGuid);
    if (found == groupReservations.end())
        return 0;

    const auto now = std::chrono::steady_clock::now();
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, found->second.playerGuid));
    if (now >= found->second.expires || !bot || !player || !player->IsInWorld() || bot->GetGroup())
    {
        groupReservations.erase(found);
        return 0;
    }
    return found->second.playerGuid;
}

void PlayerbotSocialActionBroker::ReserveForPlayer(uint32 botGuid, uint32 playerGuid)
{
    if (!botGuid || !playerGuid)
        return;
    GroupReservation reservation;
    reservation.playerGuid = playerGuid;
    reservation.expires = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    groupReservations[botGuid] = reservation;
}

void PlayerbotSocialActionBroker::CompleteGroupReservation(uint32 botGuid, uint32 playerGuid)
{
    auto found = groupReservations.find(botGuid);
    if (found != groupReservations.end() && found->second.playerGuid == playerGuid)
        groupReservations.erase(found);
}

bool PlayerbotSocialActionBroker::ValidateCommon(Player* bot, Player* player, bool requireBotAlive) const
{
    return bot && player && bot->GetPlayerbotAI() && bot->IsInWorld() && player->IsInWorld() &&
        (!requireBotAlive || bot->IsAlive()) && player->IsAlive() && bot->GetTeam() == player->GetTeam() &&
        !bot->InBattleGround() && !player->InBattleGround();
}

bool PlayerbotSocialActionBroker::HasActiveVendorTrip(uint32 botGuid) const
{
    for (const auto& pair : actions)
        if (pair.second.botGuid == botGuid && pair.second.type == "vendor_bags" &&
            (pair.second.state == "vendor_admission_wait" || pair.second.state == "vendor_travel" || pair.second.state == "vendor_relocating" ||
             pair.second.state == "return_pending" ||
             pair.second.state == "returning"))
            return true;
    return false;
}

bool PlayerbotSocialActionBroker::StartVendorTrip(Player* bot, Player* player, const std::string& actionId,
    const std::string& eventId, const std::string& proposalId, bool announce)
{
    auto existing=actions.find(actionId);
    const bool resuming=existing!=actions.end() && existing->second.state=="vendor_admission_wait" &&
        bot && player && existing->second.botGuid==bot->GetGUIDLow() && existing->second.playerGuid==player->GetGUIDLow();
    if (!ValidateCommon(bot, player) || !bot->GetGroup() || bot->GetGroup() != player->GetGroup() ||
        bot->IsInCombat() || (HasActiveVendorTrip(bot->GetGUIDLow()) && !resuming))
        return false;
    uint8 bagUsage = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
    if (bagUsage < 80)
        return false;
    LivingActivity::ActivityLease lease;
    const auto acquisition = sPlayerbotRendezvousManager.AcquirePartyActivityLease(bot->GetGUIDLow(),
        PlayerbotRendezvousManager::PartyActivityOwner::player_command,
        PlayerbotRendezvousManager::PartyActivityPhase::traveling, 300, "vendor_bags", actionId, lease);
    if (!acquisition.Permitted()) {
        if(acquisition.Waiting()) {
            Action waiting=resuming?existing->second:Action{};
            waiting.actionId=actionId;waiting.eventId=eventId;waiting.proposalId=proposalId;
            waiting.type="vendor_bags";waiting.botGuid=bot->GetGUIDLow();waiting.playerGuid=player->GetGUIDLow();
            waiting.groupId=bot->GetGroup()->GetId();
            waiting.capabilityRef="vendor:"+std::to_string(waiting.botGuid)+':'+std::to_string(waiting.playerGuid);
            waiting.state="vendor_admission_wait";waiting.announceDeparture=announce;
            const bool changed=!resuming || waiting.failureReason!=acquisition.blocker;
            waiting.failureReason=acquisition.blocker;
            if(!resuming) waiting.stateSince=std::chrono::steady_clock::now();
            waiting.lastActionAttempt=std::chrono::steady_clock::now();
            actions[actionId]=waiting;
            if(changed) Report(waiting);
            return true; // Accepted and queued, NOT a departure or completion.
        }
        return false;
    }

    LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
    std::string maintenanceType;
    if (pressure.vendorStacks)
        maintenanceType = "vendor";
    else if (pressure.HasBankableStorage())
        maintenanceType = "bank";
    else
    {
        std::string reason = pressure.StorableStacks() && pressure.bankUsage >= 100 ?
            "bank_full" : "no_quick_disposition";
        sPlayerbotInventoryPressure.Defer(bot, pressure, reason);
        if (announce)
            bot->GetPlayerbotAI()->SayToParty(
                "My bags are packed, but everything left is protected for quests, crafting, banking, or the auction house. I'll sort it out after the group.", true,
                PlayerbotAI::ChatMessageClass::social);
        sPlayerbotRendezvousManager.ResumePartyAssist(bot, player,
            "vendor_trip_not_started");
        sPlayerbotRendezvousManager.ReleasePartyActivityLease(lease,
            PlayerbotRendezvousManager::PartyActivityPhase::deferred, reason);
        return false;
    }

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();
    TravelTarget* currentTarget = context->GetValue<TravelTarget*>("travel target")->Get();
    // The stock reset action is useful only when no travel target is active,
    // which makes it unable to interrupt a follower's current quest target.
    // This scoped broker owns the replacement and clears it directly.
    sTravelMgr.SetNullTravelTarget(currentTarget);
    context->ClearValues("no active travel destinations");
    if (!SetMaintenanceTarget(bot, maintenanceType))
    {
        sPlayerbotInventoryPressure.Defer(bot, pressure, "no_same_map_maintenance_destination");
        sLog.outString("Living WoW vendor maintenance bot=%u name=%s result=target_rejected kind=%s bag=%u",
            bot->GetGUIDLow(), bot->GetName(), maintenanceType.c_str(), (uint32)bagUsage);
        sPlayerbotRendezvousManager.ResumePartyAssist(bot, player,
            "vendor_trip_target_unavailable");
        sPlayerbotRendezvousManager.ReleasePartyActivityLease(lease,
            PlayerbotRendezvousManager::PartyActivityPhase::failed,
            "no_same_map_maintenance_destination");
        return false;
    }

    Action action;
    action.lease = lease;
    action.actionId = actionId;
    action.eventId = eventId;
    action.proposalId = proposalId;
    action.type = "vendor_bags";
    action.capabilityRef = "vendor:" + std::to_string(bot->GetGUIDLow()) + ':' +
        std::to_string(player->GetGUIDLow());
    action.botGuid = bot->GetGUIDLow();
    action.playerGuid = player->GetGUIDLow();
    action.groupId = bot->GetGroup()->GetId();
    action.initialBagUsage = bagUsage;
    action.bestBagUsage = bagUsage;
    // Four free backpack slots is a useful maintenance result for an active
    // party, rather than declaring success after selling a single stack.
    action.targetBagUsage = 75;
    action.serviceReadyAt = std::chrono::steady_clock::now() + std::chrono::seconds(10 + action.botGuid % 51);
    action.maintenanceType = maintenanceType;
    action.state = "vendor_travel";
    action.stateSince = std::chrono::steady_clock::now();
    action.expires = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    // The scoped trip owns non-combat movement until the bot has sold its
    // inventory. Otherwise ordinary party follow continually pulls the bot
    // back to the human while TravelStrategy tries to reach the vendor.
    // A scoped maintenance trip must always restore ordinary party following,
    // even when the bot joined with a stale/non-follow strategy or the trip
    // could not free a slot.
    action.restoreFollow = true;
    if (ai->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT))
        ai->ChangeStrategy("nc -follow", BotState::BOT_STATE_NON_COMBAT);
    ai->StopMoving();
    actions[action.actionId] = action;
    vendorCooldowns[action.botGuid] = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    Report(actions[action.actionId]);
    sLog.outString("Living WoW vendor maintenance bot=%u name=%s result=requested bag=%u player=%u",
        bot->GetGUIDLow(), bot->GetName(), (uint32)bagUsage, player->GetGUIDLow());
    if (announce)
        bot->GetPlayerbotAI()->SayToParty(maintenanceType == "bank" ?
            "My bags are full of things I need to keep. I'll put them in the bank and catch back up." :
            "My bags are full. I need to make a quick vendor run; I'll catch back up.", true,
            PlayerbotAI::ChatMessageClass::social);
    return true;
}

static bool InviteSocialPlayer(Player* inviter, Player* player)
{
    if (!inviter || !player || !inviter->GetSession() || player->GetGroup() || player->GetGroupInvite())
        return false;
    Group* group = inviter->GetGroup();
    if (group && (!group->IsLeader(inviter->GetObjectGuid()) || group->IsFull()))
        return false;
    WorldPacket packet;
    packet << player->GetName();
    packet << uint32(0);
    inviter->GetSession()->HandleGroupInviteOpcode(packet);
    return player->GetGroupInvite() != nullptr;
}

bool PlayerbotSocialActionBroker::CanUseSharedObject(Player* bot, Player* player, ObjectGuid guid)
{
    if (!bot || !player || !guid.IsGameObject() || !bot->GetPlayerbotAI() || !bot->GetGroup() ||
        bot->GetGroup() != player->GetGroup() || !player->isRealPlayer())
        return true;

    PlayerbotRendezvousManager::PartyActivityOwner movementOwner =
        sPlayerbotRendezvousManager.GetPartyActivityOwner(bot->GetGUIDLow());
    if (movementOwner != PlayerbotRendezvousManager::PartyActivityOwner::none &&
        movementOwner != PlayerbotRendezvousManager::PartyActivityOwner::party_follow)
        return false;

    LootObject loot(bot, guid);
    GameObject* node = bot->GetPlayerbotAI()->GetGameObject(guid);
    if (!node)
        return true;

    bool gatheringNode = loot.skillId == SKILL_MINING || loot.skillId == SKILL_HERBALISM;
    bool ordinaryChest = node->GetGoType() == GAMEOBJECT_TYPE_CHEST &&
        !sObjectMgr.IsGameObjectForQuests(guid.GetEntry()) && !gatheringNode;
    if (!gatheringNode && !ordinaryChest)
        return true;
    if (ordinaryChest && !loot.IsLootPossible(bot))
        return true;

    uint32 required = gatheringNode ? std::max<uint32>(1, loot.reqSkillValue) : 0;
    if (gatheringNode)
    {
        bool playerCanGather = player->HasSkill((SkillType)loot.skillId) &&
            uint32(player->GetSkillValue(loot.skillId)) >= required;
        if (loot.skillId == SKILL_MINING && !player->HasItemCount(2901, 1))
            playerCanGather = false;
        // Do not ask the human to reserve a node they cannot actually use.
        if (!playerCanGather)
            return true;

        auto policy = gatheringPolicies.find(GatheringPolicyKey(
            bot->GetGroup()->GetId(), player->GetGUIDLow(), loot.skillId));
        if (policy != gatheringPolicies.end())
        {
            if (policy->second.mode == "human_reserved")
                return false;
            if (policy->second.mode == "bots_open")
                return true;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const uint32 groupId = bot->GetGroup()->GetId();
    const std::string partyKey = SharedObjectPartyKey(groupId, player->GetGUIDLow());
    const std::string objectKey = SharedObjectKey(groupId, player->GetGUIDLow(), guid.GetRawValue());
    auto found = sharedObjectOffers.find(bot->GetGUIDLow());
    if (found != sharedObjectOffers.end() && found->second.objectGuid == guid.GetRawValue() &&
        found->second.playerGuid == player->GetGUIDLow() && found->second.expires > now)
        return found->second.state == "approved";

    // A bot may hold only one unresolved offer. Previously, walking among two
    // nearby containers replaced this entry on every AI pass, invalidating the
    // player's reply and producing a fresh announcement each time.
    if (found != sharedObjectOffers.end() && found->second.expires > now &&
        found->second.state == "pending")
        return false;

    auto objectCooldown = sharedObjectCooldowns.find(objectKey);
    if (objectCooldown != sharedObjectCooldowns.end() && objectCooldown->second > now)
        return false;

    // The party resolves one world-object question at a time, even when a town
    // contains several crates or barrels. This also gives short replies such as
    // "yes" exactly one stable offer to confirm.
    for (const auto& pair : sharedObjectOffers)
        if (pair.second.playerGuid == player->GetGUIDLow() && pair.second.groupId == groupId &&
            (pair.second.state == "pending" || pair.second.state == "approved") &&
            pair.second.expires > now)
            return false;


    auto partyCooldown = sharedObjectPartyCooldowns.find(partyKey);
    if (partyCooldown != sharedObjectPartyCooldowns.end() && partyCooldown->second > now)
        return false;

    SharedObjectOffer offer;
    offer.botGuid = bot->GetGUIDLow();
    offer.playerGuid = player->GetGUIDLow();
    offer.groupId = groupId;
    offer.objectGuid = guid.GetRawValue();
    offer.objectEntry = guid.GetEntry();
    offer.skillId = loot.skillId;
    offer.requiredSkill = required;
    offer.nodeName = node->GetName();
    if (offer.nodeName.empty() && node->GetGOInfo())
        offer.nodeName = node->GetGOInfo()->name;
    if (offer.nodeName.empty())
        offer.nodeName = ordinaryChest ? "a chest" : "a resource node";
    offer.objectKind = ordinaryChest ? "chest" : "gathering_node";
    offer.state = "pending";
    offer.expires = now + std::chrono::seconds(90);
    sharedObjectOffers[offer.botGuid] = offer;
    sharedObjectCooldowns[objectKey] = now + std::chrono::minutes(10);
    sharedObjectPartyCooldowns[partyKey] = now + std::chrono::seconds(60);

    std::ostringstream text;
    if (ordinaryChest)
        text << "There's " << offer.nodeName << " here. Do you want me to open it?";
    else
        text << "I see " << offer.nodeName << ". Can I gather it?";
    bot->GetPlayerbotAI()->SayToParty(text.str(), true,
        PlayerbotAI::ChatMessageClass::social);
    sLog.outString("Living WoW shared object permission bot=%u player=%u object=%u kind=%s skill=%u required=%u result=offered",
        offer.botGuid, offer.playerGuid, offer.objectEntry, offer.objectKind.c_str(), offer.skillId, offer.requiredSkill);
    return false;
}

bool PlayerbotSocialActionBroker::SetMaintenanceTarget(Player* bot, const std::string& maintenanceType) const
{
    if (!bot || !bot->GetPlayerbotAI())
        return false;
    TravelDestinationPurpose purpose = maintenanceType == "bank" ?
        TravelDestinationPurpose::Bank : TravelDestinationPurpose::Vendor;
    PlayerTravelInfo info(bot);
    DestinationList destinations = sTravelMgr.GetDestinations(
        info, (uint32)purpose, {}, true, 50000.0f, false);
    WorldPosition center(bot);
    TravelDestination* bestDestination = nullptr;
    WorldPosition* bestPosition = nullptr;
    float bestDistance = std::numeric_limits<float>::max();
    // Dungeon maps do not contain ordinary vendors or bankers. For a scoped
    // party maintenance break, select the closest reachable world service and
    // return through the preserved mixed-party session afterward. Outdoor
    // maintenance remains same-map only.
    bool allowCrossMap = bot->GetMap() && bot->GetMap()->IsDungeon();
    for (TravelDestination* destination : destinations)
    {
        if (!destination)
            continue;
        std::list<uint8> chances = { 100 };
        WorldPosition* position = destination->GetNextPoint(center, chances, true);
        if (!position || (!allowCrossMap && position->getMapId() != bot->GetMapId()))
            continue;
        float distance = center.distance(*position);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestDestination = destination;
            bestPosition = position;
        }
    }
    if (!bestDestination || !bestPosition)
        return false;
    TravelTarget* target = bot->GetPlayerbotAI()->GetAiObjectContext()->
        GetValue<TravelTarget*>("travel target")->Get();
    sTravelMgr.SetNullTravelTarget(target);
    target->SetTarget(bestDestination, bestPosition);
    target->SetForced(true);
    target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("no active travel destinations");
    sLog.outString("Living WoW vendor maintenance bot=%u result=target_selected kind=%s map=%u area=%s distance=%.1f",
        bot->GetGUIDLow(), maintenanceType.c_str(), bestPosition->getMapId(),
        bestPosition->getAreaName().c_str(), bestDistance);
    return true;
}

bool PlayerbotSocialActionBroker::ContinueAtBank(Action& action, Player* bot)
{
    if (!bot || action.maintenanceType == "bank")
        return false;
    LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
    if (!pressure.HasBankableStorage())
        return false;
    if (!SetMaintenanceTarget(bot, "bank"))
        return false;
    action.maintenanceType = "bank";
    action.sellAttempts = 0;
    action.lastSellAttempt = std::chrono::steady_clock::time_point();
    action.stateSince = std::chrono::steady_clock::now();
    Report(action);
    return true;
}

void PlayerbotSocialActionBroker::QueuePartyReturn(Action& action, Player* bot, Player* player,
    const std::string& reason, bool success)
{
    if (!bot || !bot->GetPlayerbotAI())
    {
        action.state = "failed";
        action.failureReason = "vendor character became unavailable before party return";
        action.completedAt = std::chrono::steady_clock::now();
        sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
            PlayerbotRendezvousManager::PartyActivityPhase::failed,
            "party_return_bot_unavailable");
        Report(action);
        return;
    }
    bot->GetPlayerbotAI()->ChangeStrategy("nc -travel once", BotState::BOT_STATE_NON_COMBAT);
    TravelTarget* completedTarget = bot->GetPlayerbotAI()->GetAiObjectContext()->
        GetValue<TravelTarget*>("travel target")->Get();
    sTravelMgr.SetNullTravelTarget(completedTarget);
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("no active travel destinations");
    vendorPressureNotified.erase(bot->GetGUIDLow());
    // The ordinary rendezvous manager intentionally rejects instances. A
    // vendor trip that began inside a dungeon is narrower: the bot is still in
    // the same party, and the validated maintenance action owns its return.
    // Relocate to a hidden, path-valid staging point on the party's instance,
    // then let the returning state run the last few seconds naturally.
    bool dungeonParty = player && player->GetMap() && player->GetMap()->IsDungeon();
    bool dungeonReturn = false;
    float returnX = 0.0f, returnY = 0.0f, returnZ = 0.0f;
    if (dungeonParty &&
        !bot->IsInCombat() && !bot->IsBeingTeleported() &&
        sPlayerbotRendezvousManager.FindSafeStagingPoint(bot, player,
            returnX, returnY, returnZ) &&
        sPlayerbotRendezvousManager.CanRelocateUnobserved(bot, player->GetMap(),
            returnX, returnY, returnZ) &&
        sPlayerbotRendezvousManager.ClaimRelocationSlot())
    {
        dungeonReturn = bot->TeleportTo(player->GetMapId(), returnX,
            returnY, returnZ, player->GetOrientation());
    }
    if (dungeonParty && !dungeonReturn)
    {
        bool firstWait = action.state != "return_pending";
        action.maintenanceSucceeded = success;
        action.state = "return_pending";
        if (firstWait)
        {
            action.expires = std::chrono::steady_clock::now() + std::chrono::seconds(90);
            sPlayerbotRendezvousManager.UpdatePartyActivityLease(action.lease,
                PlayerbotRendezvousManager::PartyActivityPhase::returning, 90,
                "dungeon_return_waiting_for_relocation_slot");
            Report(action);
        }
        return;
    }
    if (dungeonReturn || (player && sPlayerbotRendezvousManager.ResumePartyAssist(bot, player, reason)))
    {
        sPlayerbotRendezvousManager.UpdatePartyActivityLease(action.lease,
            PlayerbotRendezvousManager::PartyActivityPhase::returning, 90, reason);
        action.state = "returning";
        action.expires = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    }
    else
    {
        if (action.restoreFollow)
            bot->GetPlayerbotAI()->ChangeStrategy("nc +follow", BotState::BOT_STATE_NON_COMBAT);
        action.restoreFollow = false;
        action.state = success ? "completed" : "failed";
        action.completedAt = std::chrono::steady_clock::now();
        if (action.failureReason.empty())
            action.failureReason = "party return could not be queued";
        sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
            success ? PlayerbotRendezvousManager::PartyActivityPhase::completed :
                PlayerbotRendezvousManager::PartyActivityPhase::failed,
            reason);
    }
    Report(action);
}

void PlayerbotSocialActionBroker::AddSharedObjectCapabilities(Player* bot, Player* player,
    ChatDirectorCandidate& candidate)
{
    if (!bot || !player || !bot->GetGroup() || bot->GetGroup() != player->GetGroup())
        return;

    for (uint32 skillId : { uint32(SKILL_MINING), uint32(SKILL_HERBALISM) })
    {
        if (!player->HasSkill((SkillType)skillId))
            continue;
        std::string skillName = GatheringSkillName(skillId);
        for (const std::string& mode : { std::string("reserve"), std::string("release"), std::string("ask") })
        {
            ChatDirectorCapability capability;
            capability.capabilityRef = "gather-policy:" + mode + ':' +
                std::to_string(bot->GetGUIDLow()) + ':' + std::to_string(player->GetGUIDLow()) + ':' +
                std::to_string(bot->GetGroup()->GetId()) + ':' + std::to_string(skillId);
            capability.type = mode == "reserve" ? "reserve_gathering_nodes" :
                mode == "release" ? "release_gathering_nodes" : "ask_gathering_nodes";
            capability.itemKind = skillName;
            capability.quantity = capability.minQuantity = capability.maxQuantity = 1;
            capability.groupId = bot->GetGroup()->GetId();
            capability.actorGuid = bot->GetGUIDLow();
            capability.description = mode == "reserve" ?
                "Reserve all " + skillName + " nodes the human can gather for the human for this party session, without asking at each node." :
                mode == "release" ?
                "Allow party bots to gather " + skillName + " nodes without asking the human until the policy changes." :
                "Restore asking the human for permission each time a party bot finds a " + skillName + " node the human can gather.";
            capability.deliveries.push_back("immediate");
            candidate.actionCapabilities.push_back(std::move(capability));
        }
    }

    auto found = sharedObjectOffers.find(bot->GetGUIDLow());
    if (found == sharedObjectOffers.end())
        return;
    SharedObjectOffer const& offer = found->second;
    if (offer.state != "pending" || offer.playerGuid != player->GetGUIDLow() ||
        offer.expires <= std::chrono::steady_clock::now() || !bot->GetGroup() ||
        bot->GetGroup() != player->GetGroup() || bot->GetGroup()->GetId() != offer.groupId)
        return;

    for (const std::string& decision : { std::string("allow"), std::string("decline") })
    {
        ChatDirectorCapability capability;
        std::string prefix = offer.objectKind == "chest" ? "chest" : "gather";
        capability.capabilityRef = prefix + ':' + decision + ':' + std::to_string(offer.botGuid) + ':' +
            std::to_string(offer.playerGuid) + ':' + std::to_string(offer.objectGuid);
        capability.type = offer.objectKind == "chest" ?
            (decision == "allow" ? "open_chest" : "decline_chest") :
            (decision == "allow" ? "gather_node" : "decline_gather_node");
        capability.itemKind = offer.objectKind;
        capability.quantity = capability.minQuantity = capability.maxQuantity = 1;
        capability.groupId = offer.groupId;
        capability.actorGuid = offer.botGuid;
        capability.description = decision == "allow" ?
            (offer.objectKind == "chest" ? "Open the exact nearby " : "Gather the exact nearby ") +
                offer.nodeName + " after the human granted permission." :
            "Leave the exact nearby " + offer.nodeName + " for the human player.";
        capability.deliveries.push_back("immediate");
        candidate.actionCapabilities.push_back(std::move(capability));
    }
}

bool PlayerbotSocialActionBroker::Create(const ChatDirectorActionProposal& proposal, const ChatDirectorEvent& event)
{
    if (!Supports(proposal.type) || !proposal.botGuid || proposal.targetGuid != event.speakerGuid)
        return false;
    auto candidate = event.candidates.find(proposal.botGuid);
    if (candidate == event.candidates.end())
        return false;
    bool supplied = false;
    for (const ChatDirectorCapability& capability : candidate->second.actionCapabilities)
        if (capability.capabilityRef == proposal.capabilityRef && capability.type == proposal.type)
            supplied = true;
    if (!supplied)
        return false;

    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(proposal.botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, proposal.targetGuid));
    if (!ValidateCommon(bot, player, proposal.type != "leave_ai_party_for_player"))
        return false;

    if (proposal.type == "vendor_bags" && HasActiveVendorTrip(proposal.botGuid))
        return false;

    Action action;
    action.actionId = "wow-social-" + event.eventId + "-" + proposal.proposalId;
    action.eventId = event.eventId;
    action.proposalId = proposal.proposalId;
    action.type = proposal.type;
    action.capabilityRef = proposal.capabilityRef;
    action.botGuid = proposal.botGuid;
    action.playerGuid = proposal.targetGuid;
    action.state = "preparing";
    action.expires = std::chrono::steady_clock::now() + std::chrono::seconds(
        proposal.type == "vendor_bags" ? 300 : proposal.type == "grant_party_free_time" ? 360 :
        proposal.type == "leave_ai_party_for_player" ? 180 : 90);

    std::smatch match;
    bool completed = false;
    if (proposal.type == "approve_party_departure" || proposal.type == "decline_party_departure")
    {
        completed = (event.channelType == "party" || event.channelType == "whisper") &&
            sPlayerbotPartyInvitationMgr.Vote(bot, player, proposal.capabilityRef,
            proposal.type == "approve_party_departure");
        if (!completed) action.failureReason = "departure_request_stale_or_voter_not_authorized";
    }
    else if (proposal.type == "create_group_and_invite" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(group:create:([0-9]+):([0-9]+))")))
    {
        completed = !bot->GetGroup() && !player->GetGroup() && InviteSocialPlayer(bot, player);
    }
    else if ((proposal.type == "invite_to_existing_group" || proposal.type == "request_leader_invite") &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(group:(?:invite|request-leader):([0-9]+):([0-9]+):([0-9]+))")))
    {
        uint32 groupId = (uint32)std::stoul(match[1].str());
        uint32 leaderGuid = (uint32)std::stoul(match[2].str());
        Group* group = bot->GetGroup();
        Player* leader = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderGuid));
        action.groupId = groupId;
        completed = group && group->GetId() == groupId && !group->IsFull() &&
            group->GetLeaderGuid().GetCounter() == leaderGuid && leader && leader->GetPlayerbotAI() &&
            InviteSocialPlayer(leader, player);
    }
    else if (proposal.type == "accept_group_invite" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(group:accept:([0-9]+))")))
    {
        Group* invite = bot->GetGroupInvite();
        uint32 leaderGuid = (uint32)std::stoul(match[1].str());
        if (invite && leaderGuid == player->GetGUIDLow() &&
            (invite->IsLeader(player->GetObjectGuid()) ||
                (player->GetGroup() == invite && invite->IsAssistant(player->GetObjectGuid()))))
        {
            Player* inviter = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, leaderGuid));
            WorldPacket packet;
            bot->GetSession()->HandleGroupAcceptOpcode(packet);
            completed = bot->GetGroup() && bot->GetGroup() == player->GetGroup();
            if (completed && inviter && inviter->isRealPlayer())
                sPlayerbotRendezvousManager.RegisterPartyAssist(bot, inviter);
        }
    }
    else if (proposal.type == "pass_leadership" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(group:pass:([0-9]+):([0-9]+))")))
    {
        uint32 groupId = (uint32)std::stoul(match[1].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        if (group && group->GetId() == groupId && group->IsLeader(bot->GetObjectGuid()) &&
            group->IsMember(player->GetObjectGuid()))
        {
            WorldPacket packet;
            packet << player->GetObjectGuid();
            bot->GetSession()->HandleGroupSetLeaderOpcode(packet);
            completed = group->IsLeader(player->GetObjectGuid());
        }
    }
    else if (proposal.type == "leave_group" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(group:leave:([0-9]+))")))
    {
        uint32 groupId = (uint32)std::stoul(match[1].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        if (group && group->GetId() == groupId && !bot->IsInCombat() && !bot->GetMap()->IsDungeon())
        {
            WorldPacket packet;
            bot->GetSession()->HandleGroupDisbandOpcode(packet);
            completed = bot->GetGroup() == nullptr;
        }
    }
    else if (proposal.type == "leave_ai_party_for_player" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(group:leave-ai-invite:([0-9]+):([0-9]+):([0-9]+):([0-9]+))")))
    {
        Group* invite = bot->GetGroupInvite();
        if (CanReleasePendingInvite(bot) &&
            invite->GetLeaderGuid().GetCounter() == (uint32)std::stoul(match[1].str()) &&
            invite->GetId() == (uint32)std::stoul(match[2].str()) &&
            bot->GetGUIDLow() == (uint32)std::stoul(match[3].str()) &&
            player->GetGUIDLow() == (uint32)std::stoul(match[4].str()))
        {
            if (!invite->IsCreated() && invite->IsLeader(bot->GetObjectGuid()))
            {
                // Cancel our own unfinished party, including outgoing invites.
                // It has no persisted members and is not registered in ObjectMgr.
                invite->RemoveAllInvites();
                delete invite;
            }
            else
                bot->UninviteFromGroup();
            // Native cleanup may destroy the group: never dereference it again.
            completed = !bot->GetGroup() && !bot->GetGroupInvite();
            if (completed)
            {
                ReserveForPlayer(bot->GetGUIDLow(), player->GetGUIDLow());
                SendSocialWhisper(bot, player,
                    "I cleared the pending bot-party invite. I'm free now; try inviting me again.");
            }
        }
        if (!completed) action.failureReason = "pending invitation changed or is not NPC-only";
    }
    else if (proposal.type == "leave_ai_party_for_player" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(group:leave-ai-for-player:([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 groupId = (uint32)std::stoul(match[1].str());
        action.groupId = groupId;
        Group* group = bot->GetGroup();
        if (group && group->GetId() == groupId && !GroupHasRealHuman(group) &&
            !bot->GetMap()->IsDungeon() && !bot->InBattleGround())
        {
            const auto blocker = living_party_release::Classify(bot->IsInCombat(),
                bot->GetTransport() != nullptr, bot->IsTaxiFlying(), bot->IsBeingTeleported());
            for (const auto& pair : actions)
                if (pair.second.botGuid == bot->GetGUIDLow() && pair.second.state == "waiting_to_leave_ai_party")
                {
                    SendSocialWhisper(bot, player, pair.second.playerGuid == player->GetGUIDLow() ?
                        living_party_release::Waiting(blocker) : "I'm already arranging to join another party.");
                    action.state = "rejected";
                    action.failureReason = "release_already_pending";
                    Report(action);
                    return false; // Do not extend or duplicate the original request.
                }
            if (blocker != living_party_release::Blocker::ready)
            {
                action.state = "waiting_to_leave_ai_party";
                action.failureReason = living_party_release::Code(blocker);
                SendSocialWhisper(bot, player, living_party_release::Waiting(blocker));
                actions[action.actionId] = action;
                Report(actions[action.actionId]);
                return true;
            }
            completed = LeaveAiOnlyParty(bot, groupId);
            if (completed)
            {
                ReserveForPlayer(bot->GetGUIDLow(), player->GetGUIDLow());
                SendSocialWhisper(bot, player, bot->IsAlive() ?
                    "I'm free now. You can invite me." :
                    "I'm free now, but I'm dead and recovering. You can invite me.");
            }
        }
        else
            action.failureReason = GroupHasRealHuman(group) ? "a human is now in the party" :
                "the AI-only party is no longer safe to leave";
    }
    else if (proposal.type == "share_quest" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(quest:share:([0-9]+):([0-9]+))")))
    {
        uint32 questId = (uint32)std::stoul(match[1].str());
        uint32 groupId = (uint32)std::stoul(match[2].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        action.questId = questId;
        if (group && group->GetId() == groupId && group == player->GetGroup() && bot->CanShareQuest(questId))
        {
            WorldPacket packet;
            packet << questId;
            bot->GetSession()->HandlePushQuestToParty(packet);
            completed = true;
        }
    }
    else if (proposal.type == "accept_party_quest_plan" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(quest:plan:([0-9]+):([0-9]+))")))
    {
        uint32 questId = (uint32)std::stoul(match[1].str());
        uint32 groupId = (uint32)std::stoul(match[2].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        action.questId = questId;
        completed = group && group->GetId() == groupId && group == player->GetGroup() &&
            bot->GetQuestStatus(questId) != QUEST_STATUS_NONE;
        if (completed)
        {
            preferredQuests[bot->GetGUIDLow()] = std::make_pair(questId, std::chrono::steady_clock::now() + std::chrono::minutes(20));
            bot->GetPlayerbotAI()->DoSpecificAction("reset travel target", Event("living party quest plan", std::to_string(questId), player), true);
        }
    }
    else if (proposal.type == "volunteer_for_guild_charter" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(guild:petition-volunteer:([0-9]+):([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 ownerGuid = (uint32)std::stoul(match[3].str());
        uint32 petitionGuid = (uint32)std::stoul(match[4].str());
        Player* owner = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, ownerGuid));
        uint32 signatures = 0, required = 0;
        Group* group = bot->GetGroup();
        if (group && GroupHasRealHuman(group))
            action.failureReason = "this volunteer is now helping a human-led party";
        else if (group && !LeaveAiOnlyParty(bot, group->GetId()))
            action.failureReason = "this volunteer could not safely leave its autonomous party";
        else if (!ValidatePetitionVolunteer(bot, owner, petitionGuid, signatures, required))
            action.failureReason = "the charter or volunteer eligibility changed before travel began";
        else if (signatures + PendingPetitionVolunteers(petitionGuid) >= required)
            action.failureReason = "the charter already has enough signed or traveling volunteers";
        else
        {
            PlayerbotRendezvousManager::RequestResult result = sPlayerbotRendezvousManager.Request(
                bot, owner, action.actionId, true);
            if (result == PlayerbotRendezvousManager::RequestResult::accepted ||
                result == PlayerbotRendezvousManager::RequestResult::ordinary_travel)
            {
                action.subjectGuid = ownerGuid;
                action.questId = petitionGuid;
                action.state = "traveling_for_charter";
                action.stateSince = std::chrono::steady_clock::now();
                action.expires = action.stateSince + std::chrono::minutes(3);
                actions[action.actionId] = action;
                SendSocialWhisper(bot, owner,
                    "I heard you're looking for charter signatures. I'm on my way.");
                SendSocialWhisper(owner, bot,
                    "Great. Meet me here and I'll show you the charter.");
                sLog.outString("Living WoW charter volunteer event=travel_started bot=%u owner=%u requester=%u petition=%u",
                    bot->GetGUIDLow(), ownerGuid, player->GetGUIDLow(), petitionGuid);
                Report(actions[action.actionId]);
                return true;
            }
            action.failureReason = result == PlayerbotRendezvousManager::RequestResult::unsafe ?
                "there is no observer-safe route to the charter owner" :
                "the volunteer cannot travel to the charter owner right now";
        }
    }
    else if (proposal.type == "solicit_petition_signatures" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(guild:petition-solicit:([0-9]+):([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 groupId = (uint32)std::stoul(match[3].str());
        uint32 petitionGuid = (uint32)std::stoul(match[4].str());
        Group* group = bot->GetGroup();
        Item* petition = bot->GetItemByEntry(5863);
        action.groupId = groupId;
        action.questId = petitionGuid;
        uint32 required = sWorld.getConfig(CONFIG_UINT32_MIN_PETITION_SIGNS);
        auto signatures = CharacterDatabase.PQuery(
            "SELECT playerguid FROM petition_sign WHERE petitionguid = '%u'", petitionGuid);
        uint32 signatureCount = signatures ? signatures->GetRowCount() : 0;
        if (group && group == player->GetGroup() && group->GetId() == groupId && petition &&
            petition->GetObjectGuid().GetCounter() == petitionGuid && !bot->GetGuildId() &&
            !bot->GetGuildIdInvited() && signatureCount < required)
        {
            uint32 offered = 0;
            uint32 slots = required - signatureCount;
            for (GroupReference* reference = group->GetFirstMember(); reference && offered < slots;
                reference = reference->next())
            {
                Player* member = reference->getSource();
                if (!member || member == bot || !member->IsInWorld() || !member->GetSession() ||
                    member->GetGuildId() || member->GetGuildIdInvited() ||
                    member->GetMapId() != bot->GetMapId() ||
                    sServerFacade.GetDistance2d(bot, member) > sPlayerbotAIConfig.spellDistance)
                    continue;
                auto signedAlready = CharacterDatabase.PQuery(
                    "SELECT playerguid FROM petition_sign WHERE player_account = '%u' AND petitionguid = '%u'",
                    member->GetSession()->GetAccountId(), petitionGuid);
                if (signedAlready)
                    continue;
                if (bot->GetPlayerbotAI()->DoSpecificAction("offer petition",
                    Event("living charter solicitation", member->GetObjectGuid(), player), true))
                    ++offered;
            }
            completed = offered > 0;
            if (completed)
                sLog.outString("Living WoW charter solicitation owner=%u requester=%u group=%u petition=%u offered=%u",
                    bot->GetGUIDLow(), player->GetGUIDLow(), groupId, petitionGuid, offered);
            else
                action.failureReason = "no eligible unsigned party member is currently close enough";
        }
        else
            action.failureReason = "the charter, party, or signature state changed before solicitation";
    }
    else if (proposal.type == "transfer_guild_leadership" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(guild:transfer-leader:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 guildId = (uint32)std::stoul(match[3].str());
        Guild* guild = sGuildMgr.GetGuildById(guildId);
        if (!guild || bot->GetGuildId() != guildId)
            action.failureReason = "the named guild is no longer available to this character";
        else if (guild->GetLeaderGuid() != bot->GetObjectGuid())
            action.failureReason = "this character is no longer the guild leader";
        else if (player->GetGuildId() != guildId)
            action.failureReason = "the requested new leader is not currently a member of this guild";
        else if (player == bot)
            action.failureReason = "the requested character already leads this guild";
        else
        {
            completed = bot->GetPlayerbotAI()->DoSpecificAction("guild leader",
                Event("living guild leadership transfer", player->GetObjectGuid(), player), true);
            Guild* current = sGuildMgr.GetGuildById(guildId);
            completed = completed && current && current->GetLeaderGuid() == player->GetObjectGuid();
            if (completed)
                sLog.outString("Living WoW guild leadership transferred guild=%u from=%u to=%u requester=%u",
                    guildId, bot->GetGUIDLow(), player->GetGUIDLow(), player->GetGUIDLow());
            else
                action.failureReason = "normal guild rank rules did not permit that leadership transfer";
        }
    }
    else if ((proposal.type == "invite_to_guild" || proposal.type == "promote_guild_member" ||
              proposal.type == "demote_guild_member" || proposal.type == "remove_guild_member") &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(guild:(invite|promote|demote|remove):([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[2].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[3].str()) == player->GetGUIDLow())
    {
        std::string operation = match[1].str();
        uint32 guildId = (uint32)std::stoul(match[4].str());
        Guild* guild = sGuildMgr.GetGuildById(guildId);
        if (!guild || bot->GetGuildId() != guildId)
            action.failureReason = "the bound guild is no longer available to this character";
        else if (operation == "invite")
        {
            if (player->GetGuildId())
                action.failureReason = "the requested player is already in a guild";
            else if (player->GetGuildIdInvited())
                action.failureReason = "the requested player already has a pending guild invitation";
            else if (!guild->HasRankRight(bot->GetRank(), GR_RIGHT_INVITE))
                action.failureReason = "this character no longer has permission to invite guild members";
            else if (guild->GetMemberSize() >= 1000)
                action.failureReason = "the guild is full";
            else
            {
                completed = bot->GetPlayerbotAI()->DoSpecificAction("guild invite",
                    Event("living guild invitation", player->GetObjectGuid(), player), true);
                completed = completed && player->GetGuildIdInvited() == guildId;
                if (!completed)
                    action.failureReason = "normal guild invitation rules rejected the request";
            }
        }
        else
        {
            uint32 oldRank = player->GetRank();
            bool sameGuild = player->GetGuildId() == guildId;
            uint32 right = operation == "promote" ? GR_RIGHT_PROMOTE :
                operation == "demote" ? GR_RIGHT_DEMOTE : GR_RIGHT_REMOVE;
            bool rankAllowed = operation == "promote" ? oldRank > bot->GetRank() + 1 :
                operation == "demote" ? oldRank > bot->GetRank() && oldRank < guild->GetLowestRank() :
                oldRank > bot->GetRank();
            if (!sameGuild)
                action.failureReason = "the requested character is no longer a member of this guild";
            else if (!guild->HasRankRight(bot->GetRank(), right))
                action.failureReason = "this character no longer has the required guild rank right";
            else if (!rankAllowed)
                action.failureReason = "the guild rank hierarchy does not permit that change";
            else
            {
                std::string playerbotAction = operation == "promote" ? "guild promote" :
                    operation == "demote" ? "guild demote" : "guild remove";
                completed = bot->GetPlayerbotAI()->DoSpecificAction(playerbotAction,
                    Event("living guild governance", player->GetObjectGuid(), player), true);
                if (operation == "promote") completed = completed && player->GetGuildId() == guildId && player->GetRank() < oldRank;
                else if (operation == "demote") completed = completed && player->GetGuildId() == guildId && player->GetRank() > oldRank;
                else completed = completed && !player->GetGuildId();
                if (!completed)
                    action.failureReason = "normal guild rank rules rejected the requested change";
            }
        }
    }
    else if (proposal.type == "leave_guild" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(guild:leave:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 guildId = (uint32)std::stoul(match[3].str());
        Guild* guild = sGuildMgr.GetGuildById(guildId);
        bool contextualRequester = player->GetGuildId() == guildId ||
            (bot->GetGroup() && bot->GetGroup() == player->GetGroup());
        if (!guild || bot->GetGuildId() != guildId)
            action.failureReason = "this character is no longer a member of the bound guild";
        else if (guild->GetLeaderGuid() == bot->GetObjectGuid())
            action.failureReason = "a guild leader must transfer leadership or disband the guild before leaving";
        else if (!contextualRequester)
            action.failureReason = "the requester no longer has the relationship required for this request";
        else
        {
            completed = bot->GetPlayerbotAI()->DoSpecificAction("guild leave",
                Event("living guild departure", "", player), true);
            completed = completed && !bot->GetGuildId();
            if (!completed)
                action.failureReason = "normal guild security rules rejected the request to leave";
        }
    }
    else if (proposal.type == "perform_emote" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(social:emote:([0-9]+):([0-9]+):(wave|bow|cheer|salute|laugh|dance))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        std::string emote = match[3].str();
        completed = bot->GetMapId() == player->GetMapId() &&
            sServerFacade.GetDistance2d(bot, player) <= sPlayerbotAIConfig.farDistance &&
            bot->GetPlayerbotAI()->DoSpecificAction("emote",
                Event("living natural language emote", emote, player), true);
        if (!completed)
            action.failureReason = "the requested emote is not currently possible nearby";
    }
    else if (proposal.type == "wait_here" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(travel:wait:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 groupId = (uint32)std::stoul(match[3].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        if (group && group == player->GetGroup() && group->GetId() == groupId &&
            !bot->IsTaxiFlying() && !bot->GetTransport())
        {
            completed = !sServerFacade.isMoving(bot) || bot->GetPlayerbotAI()->DoSpecificAction(
                "stay", Event("living natural language wait", "", player), true);
        }
        if (!completed)
            action.failureReason = "the current party or movement state does not permit waiting here";
    }
    else if (proposal.type == "use_hearthstone" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(travel:hearth:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        uint32 groupId = (uint32)std::stoul(match[3].str());
        Group* group = bot->GetGroup();
        action.groupId = groupId;
        if (group && group == player->GetGroup() && group->GetId() == groupId &&
            !bot->IsInCombat() && !bot->InBattleGround() && !bot->IsTaxiFlying() &&
            bot->GetPlayerbotAI()->CanDoSpecificAction("hearthstone", true, true))
        {
            completed = bot->GetPlayerbotAI()->DoSpecificAction("hearthstone",
                Event("living natural language hearth", "", player), true);
        }
        if (!completed)
            action.failureReason = "the hearthstone is unavailable, on cooldown, or unsafe to use now";
    }
    else if (proposal.type == "vendor_bags" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(vendor:([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow() &&
        bot->GetGroup() && bot->GetGroup() == player->GetGroup() && !bot->IsInCombat())
    {
        if (StartVendorTrip(bot, player, action.actionId, action.eventId, action.proposalId, false))
            return true;
        action.failureReason = "no safe vendor trip is currently available";
    }
    else if (proposal.type == "grant_party_free_time" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(party:free-time:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow() &&
        bot->GetGroup() && bot->GetGroup() == player->GetGroup() &&
        bot->GetGroup()->GetId() == (uint32)std::stoul(match[3].str()))
    {
        action.groupId = bot->GetGroup()->GetId();
        if (HasActiveVendorTrip(bot->GetGUIDLow()))
        {
            action.state = "waiting_for_vendor_trip";
            actions[action.actionId] = action;
            Report(actions[action.actionId]);
            return true;
        }
        completed = sPlayerbotRendezvousManager.BeginPartyFreeTime(
            bot, player, "party_leader_granted_free_time");
    }
    else if (proposal.type == "resume_party_assist" &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(party:resume-assist:([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow() &&
        bot->GetGroup() && bot->GetGroup() == player->GetGroup() &&
        bot->GetGroup()->GetId() == (uint32)std::stoul(match[3].str()))
    {
        completed = sPlayerbotRendezvousManager.ResumePartyAssist(
            bot, player, "party_leader_recalled_free_time");
    }
    else if ((proposal.type == "reserve_gathering_nodes" ||
              proposal.type == "release_gathering_nodes" || proposal.type == "ask_gathering_nodes") &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"(gather-policy:(reserve|release|ask):([0-9]+):([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[2].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[3].str()) == player->GetGUIDLow())
    {
        uint32 groupId = (uint32)std::stoul(match[4].str());
        uint32 skillId = (uint32)std::stoul(match[5].str());
        Group* group = bot->GetGroup();
        completed = group && group == player->GetGroup() && group->GetId() == groupId &&
            (skillId == SKILL_MINING || skillId == SKILL_HERBALISM);
        if (completed)
        {
            GatheringPolicy policy;
            policy.playerGuid = player->GetGUIDLow();
            policy.groupId = groupId;
            policy.skillId = skillId;
            policy.mode = match[1].str() == "reserve" ? "human_reserved" :
                match[1].str() == "release" ? "bots_open" : "ask";
            std::string key = GatheringPolicyKey(groupId, policy.playerGuid, skillId);
            if (policy.mode == "ask")
                gatheringPolicies.erase(key);
            else
                gatheringPolicies[key] = policy;

            for (auto offer = sharedObjectOffers.begin(); offer != sharedObjectOffers.end(); )
                if (offer->second.groupId == groupId && offer->second.playerGuid == policy.playerGuid &&
                    offer->second.skillId == skillId)
                    offer = sharedObjectOffers.erase(offer);
                else
                    ++offer;
            sLog.outString("Living WoW gathering policy player=%u group=%u skill=%u mode=%s actor=%u",
                policy.playerGuid, groupId, skillId, policy.mode.c_str(), bot->GetGUIDLow());
        }
    }
    else if ((proposal.type == "gather_node" || proposal.type == "decline_gather_node" ||
              proposal.type == "open_chest" || proposal.type == "decline_chest") &&
        std::regex_match(proposal.capabilityRef, match,
            std::regex(R"((gather|chest):(allow|decline):([0-9]+):([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[3].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[4].str()) == player->GetGUIDLow())
    {
        uint64 objectGuid = std::stoull(match[5].str());
        auto offer = sharedObjectOffers.find(bot->GetGUIDLow());
        bool approving = proposal.type == "gather_node" || proposal.type == "open_chest";
        bool kindMatches = offer != sharedObjectOffers.end() &&
            ((offer->second.objectKind == "chest") == (match[1].str() == "chest"));
        completed = offer != sharedObjectOffers.end() && kindMatches && offer->second.state == "pending" &&
            offer->second.objectGuid == objectGuid && offer->second.playerGuid == player->GetGUIDLow() &&
            offer->second.expires > std::chrono::steady_clock::now() && bot->GetGroup() == player->GetGroup();
        if (completed && approving)
        {
            ObjectGuid guid(objectGuid);
            LootObject loot(bot, guid);
            completed = !bot->IsInCombat() && loot.IsLootPossible(bot);
            if (completed)
            {
                offer->second.state = "approved";
                offer->second.expires = std::chrono::steady_clock::now() + std::chrono::minutes(2);
                completed = bot->GetPlayerbotAI()->GetAiObjectContext()->
                    GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
                if (completed)
                {
                    // The offer may have been made while the bot was away on a
                    // city errand. Merely adding the old object to available
                    // loot let party follow win and produced a truthful queue
                    // acknowledgement with no visible movement. Reissue the
                    // exact-object loot movement immediately; normal loot
                    // validation still rejects a despawned or inaccessible GO.
                    bot->GetPlayerbotAI()->StopMoving();
                    if (bot->GetPlayerbotAI()->CanDoSpecificAction("move to loot", true, true))
                        bot->GetPlayerbotAI()->DoSpecificAction("move to loot",
                            Event("living shared object approval", guid, player), true);
                    sLog.outString("Living WoW shared object permission bot=%u player=%u object=%u kind=%s result=movement_reissued",
                        bot->GetGUIDLow(), player->GetGUIDLow(), offer->second.objectEntry,
                        offer->second.objectKind.c_str());
                }
            }
        }
        else if (completed)
        {
            offer->second.state = "declined";
            offer->second.expires = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        }
        if (completed)
            sLog.outString("Living WoW shared object permission bot=%u player=%u object=%u kind=%s result=%s",
                bot->GetGUIDLow(), player->GetGUIDLow(), offer->second.objectEntry,
                offer->second.objectKind.c_str(), approving ? "approved_queued" : "declined");
    }
    else if (proposal.type == "meet_player" &&
        std::regex_match(proposal.capabilityRef, match, std::regex(R"(meet:([0-9]+):([0-9]+))")) &&
        (uint32)std::stoul(match[1].str()) == bot->GetGUIDLow() &&
        (uint32)std::stoul(match[2].str()) == player->GetGUIDLow())
    {
        PlayerbotRendezvousManager::RequestResult result = sPlayerbotRendezvousManager.Request(
            bot, player, action.actionId, true);
        if (result == PlayerbotRendezvousManager::RequestResult::accepted ||
            result == PlayerbotRendezvousManager::RequestResult::ordinary_travel)
        {
            action.state = "meeting";
            actions[action.actionId] = action;
            Report(actions[action.actionId]);
            return true;
        }
        action.failureReason = result == PlayerbotRendezvousManager::RequestResult::unsafe ?
            "no observer-safe rendezvous route" : "bot cannot leave its current activity";
    }

    action.state = completed ? "completed" : "rejected";
    if (!completed && action.failureReason.empty())
        action.failureReason = "authoritative group or quest state no longer permits the action";
    if (!completed && proposal.type == "leave_ai_party_for_player")
        SendSocialWhisper(bot, player, "I couldn't safely change parties because my group or travel state changed. Please ask me again when I'm clear.");
    actions[action.actionId] = action;
    Report(actions[action.actionId]);
    return completed;
}

uint32 PlayerbotSocialActionBroker::PendingPetitionVolunteers(uint32 petitionGuid) const
{
    uint32 count = 0;
    for (const auto& pair : actions)
        if (pair.second.type == "volunteer_for_guild_charter" && pair.second.questId == petitionGuid &&
            (pair.second.state == "traveling_for_charter" || pair.second.state == "waiting_for_charter"))
            ++count;
    return count;
}

uint32 PlayerbotSocialActionBroker::PreferredQuest(uint32 botGuid) const
{
    auto found = preferredQuests.find(botGuid);
    if (found == preferredQuests.end() || found->second.second <= std::chrono::steady_clock::now())
        return 0;
    return found->second.first;
}

void PlayerbotSocialActionBroker::Update()
{
    bool dockExitAttempted = sPlayerbotPartyInvitationMgr.Update();
    const auto now = std::chrono::steady_clock::now();
    for (auto cooldown = sharedObjectCooldowns.begin(); cooldown != sharedObjectCooldowns.end(); )
        if (cooldown->second <= now)
            cooldown = sharedObjectCooldowns.erase(cooldown);
        else
            ++cooldown;
    for (auto cooldown = sharedObjectPartyCooldowns.begin(); cooldown != sharedObjectPartyCooldowns.end(); )
        if (cooldown->second <= now)
            cooldown = sharedObjectPartyCooldowns.erase(cooldown);
        else
            ++cooldown;
    for (auto policy = gatheringPolicies.begin(); policy != gatheringPolicies.end(); )
    {
        Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, policy->second.playerGuid));
        if (!player || !player->GetGroup() || player->GetGroup()->GetId() != policy->second.groupId)
            policy = gatheringPolicies.erase(policy);
        else
            ++policy;
    }
    for (auto offer = sharedObjectOffers.begin(); offer != sharedObjectOffers.end(); )
    {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(offer->second.botGuid);
        Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, offer->second.playerGuid));
        if (offer->second.expires <= now || !bot || !player || !bot->GetGroup() ||
            bot->GetGroup() != player->GetGroup() || bot->GetGroup()->GetId() != offer->second.groupId)
            offer = sharedObjectOffers.erase(offer);
        else
            ++offer;
    }
    if (!nextVendorScan.time_since_epoch().count() || now >= nextVendorScan)
    {
        nextVendorScan = now + std::chrono::seconds(5);
        // A human's follow-up invitation can otherwise wait behind ordinary
        // AI actions. Poll only explicit, short-lived release reservations.
        // Copy because native acceptance removes its reservation.
        const auto reservations = groupReservations;
        for (const auto& entry : reservations)
        {
            const uint32 playerGuid = ReservedForPlayer(entry.first);
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(entry.first);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, playerGuid));
            Group* invite = bot ? bot->GetGroupInvite() : nullptr;
            if (!playerGuid || !ValidateCommon(bot, player, false) || !invite || bot->GetGroup() ||
                !player->isRealPlayer() || !invite->IsLeader(player->GetObjectGuid()) ||
                bot->IsInCombat() || bot->GetTransport() || bot->IsTaxiFlying() ||
                bot->IsBeingTeleported() || bot->GetMap()->IsDungeon())
                continue;
            ai::Event invitation("reserved human invitation", "", player);
            bot->GetPlayerbotAI()->DoSpecificAction("accept invitation", invitation, true);
        }
        for (const auto& entry : sRandomPlayerbotMgr.GetPlayers())
        {
            Player* bot = entry.second;
            if (!bot || !bot->IsInWorld() || !bot->GetPlayerbotAI() || !bot->GetGroup() ||
                !bot->IsAlive() || bot->IsInCombat() || HasActiveVendorTrip(bot->GetGUIDLow()) ||
                sPlayerbotRendezvousManager.IsPartyFreeTime(bot->GetGUIDLow()))
                continue;
            uint8 bagUsage = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
            if (bagUsage < 90)
            {
                vendorPressureNotified.erase(bot->GetGUIDLow());
                continue;
            }
            auto cooldown = vendorCooldowns.find(bot->GetGUIDLow());
            if (cooldown != vendorCooldowns.end() && cooldown->second > now)
                continue;
            Player* player = nullptr;
            Group::MemberSlotList const& members = bot->GetGroup()->GetMemberSlots();
            for (Group::MemberSlotList::const_iterator member = members.begin(); member != members.end(); ++member)
            {
                Player* candidate = sObjectAccessor.FindPlayer(member->guid);
                if (candidate && !candidate->GetPlayerbotAI())
                {
                    player = candidate;
                    if (bot->GetGroup()->IsLeader(candidate->GetObjectGuid()))
                        break;
                }
            }
            if (!player)
                continue;
            if (bagUsage < 100)
            {
                if (vendorPressureNotified.insert(bot->GetGUIDLow()).second)
                {
                    std::ostringstream notice;
                    notice << "I'm at " << (uint32)bagUsage << "% bag space. Can I go vendor?";
                    bot->GetPlayerbotAI()->SayToParty(notice.str(), true,
                        PlayerbotAI::ChatMessageClass::social);
                    sLog.outString("Living WoW vendor maintenance bot=%u name=%s result=permission_requested bag=%u player=%u",
                        bot->GetGUIDLow(), bot->GetName(), (uint32)bagUsage, player->GetGUIDLow());
                }
                continue;
            }
            std::ostringstream id;
            id << "wow-social-vendor-auto-" << bot->GetGUIDLow() << '-' << time(nullptr);
            StartVendorTrip(bot, player, id.str(), "inventory-full", "proactive-vendor", true);
        }
    }
    for (auto& pair : actions)
    {
        Action& action = pair.second;
        if(action.state=="vendor_admission_wait")
        {
            if(now-action.lastActionAttempt<std::chrono::seconds(5)) continue;
            action.lastActionAttempt=now;
            Player* bot=sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player=sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER,action.playerGuid));
            // Offline or unsafe is a pause, not evidence of party departure.
            if(!bot || !player || !bot->IsInWorld() || !player->IsInWorld()) continue;
            if(!bot->GetGroup() || bot->GetGroup()!=player->GetGroup() || bot->GetGroup()->GetId()!=action.groupId) {
                action.state="cancelled";action.failureReason="party_changed_before_vendor_admission";
                action.completedAt=now;Report(action);continue;
            }
            if(!bot->IsAlive() || !player->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() ||
                bot->IsTaxiFlying() || bot->GetTransport() || bot->InBattleGround()) continue;
            if(bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint8>("bag space")->Get()<80) {
                action.state="cancelled";action.failureReason="vendor_capacity_need_no_longer_present";
                action.completedAt=now;Report(action);continue;
            }
            if(!StartVendorTrip(bot,player,action.actionId,action.eventId,action.proposalId,action.announceDeparture)) {
                action.state="failed";action.failureReason="vendor_preparation_unavailable_after_admission_wait";
                action.completedAt=now;Report(action);
            }
        }
        else if (action.state == "traveling_for_charter" || action.state == "waiting_for_charter")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* owner = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.subjectGuid));
            bool signedNow = HasPetitionSignature(bot, action.questId);
            if (signedNow)
            {
                action.state = "completed";
                action.completedAt = now;
                if (bot && owner)
                {
                    SendSocialWhisper(bot, owner, "Signed. Good luck with the guild!");
                    SendSocialWhisper(owner, bot, "Thanks for signing.");
                    sPlayerbotRendezvousManager.BeginDeparture(bot->GetGUIDLow(), owner->GetGUIDLow(),
                        "charter_signed");
                }
                sLog.outString("Living WoW charter volunteer event=signed bot=%u owner=%u requester=%u petition=%u",
                    action.botGuid, action.subjectGuid, action.playerGuid, action.questId);
                Report(action);
            }
            else if (!bot || !owner || !bot->IsInWorld() || !owner->IsInWorld() || now >= action.expires)
            {
                if (bot && owner)
                    sPlayerbotRendezvousManager.Cancel(bot->GetGUIDLow(), owner->GetGUIDLow(),
                        "charter_recruitment_expired");
                action.state = now >= action.expires ? "expired" : "failed";
                action.failureReason = now >= action.expires ?
                    "the charter meetup expired before the normal petition exchange completed" :
                    "the charter owner or volunteer became unavailable";
                Report(action);
            }
            else
            {
                uint32 signatures = 0, required = 0;
                if (!ValidatePetitionVolunteer(bot, owner, action.questId, signatures, required))
                {
                    action.state = "rejected";
                    action.failureReason = signatures >= required ?
                        "the charter received enough signatures before this volunteer arrived" :
                        "the charter or volunteer eligibility changed during the meetup";
                    sPlayerbotRendezvousManager.Cancel(bot->GetGUIDLow(), owner->GetGUIDLow(),
                        "charter_state_changed");
                    Report(action);
                }
                else if ((sPlayerbotRendezvousManager.State(bot->GetGUIDLow(), owner->GetGUIDLow()) == "arrived" ||
                          bot->IsWithinDistInMap(owner, INTERACTION_DISTANCE)) &&
                         (!action.lastActionAttempt.time_since_epoch().count() ||
                          std::chrono::duration_cast<std::chrono::seconds>(now - action.lastActionAttempt).count() >= 2))
                {
                    action.state = "waiting_for_charter";
                    action.lastActionAttempt = now;
                    bool offered = owner->GetPlayerbotAI() && owner->GetPlayerbotAI()->DoSpecificAction(
                        "offer petition", Event("living charter volunteer", bot->GetObjectGuid(), owner), true);
                    sLog.outString("Living WoW charter volunteer event=petition_offered bot=%u owner=%u requester=%u petition=%u result=%s",
                        bot->GetGUIDLow(), owner->GetGUIDLow(), action.playerGuid, action.questId,
                        offered ? "offered" : "retrying");
                }
            }
        }
        else if (action.state == "waiting_to_leave_ai_party")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            Group* group = bot ? bot->GetGroup() : nullptr;
            if (bot && player && group && group->GetId() == action.groupId && !GroupHasRealHuman(group) &&
                bot->IsInWorld() && !bot->IsBeingTeleported() && bot->GetTransport() && !dockExitAttempted &&
                now < action.expires && now >= action.lastActionAttempt + std::chrono::seconds(2))
            {
                action.lastActionAttempt = now;
                dockExitAttempted = true;
                if (ai::MovementAction::ExitTransportAtDock(bot->GetPlayerbotAI())) continue;
            }
            const auto blocker = bot ? living_party_release::Classify(bot->IsInCombat(),
                bot->GetTransport() != nullptr, bot->IsTaxiFlying(), bot->IsBeingTeleported()) :
                living_party_release::Blocker::ready;
            if (!bot || !player || !ValidateCommon(bot, player, false) || !group || group->GetId() != action.groupId ||
                GroupHasRealHuman(group))
            {
                action.state = "rejected";
                action.failureReason = group && GroupHasRealHuman(group) ? "a human joined the party" :
                    "party or character state changed before release";
                if (bot && player && bot->IsInWorld() && player->IsInWorld())
                    SendSocialWhisper(bot, player, "My party situation changed, so I cancelled the request to leave. Please check with me again.");
                Report(action);
            }
            else if (now >= action.expires)
            {
                action.state = "expired";
                action.failureReason = std::string("release_timeout:") + living_party_release::Code(blocker);
                SendSocialWhisper(bot, player, living_party_release::Expired(blocker));
                Report(action);
            }
            else if (blocker == living_party_release::Blocker::ready && LeaveAiOnlyParty(bot, action.groupId))
            {
                action.state = "completed";
                action.failureReason.clear();
                action.completedAt = now;
                ReserveForPlayer(bot->GetGUIDLow(), player->GetGUIDLow());
                SendSocialWhisper(bot, player, bot->IsAlive() ?
                    "I'm free now. You can invite me." :
                    "I'm free now, but I'm dead and recovering. You can invite me.");
                Report(action);
            }
            else if (blocker == living_party_release::Blocker::ready)
            {
                action.state = "rejected";
                action.failureReason = "release_safety_changed";
                SendSocialWhisper(bot, player, "I can't safely leave this party here. I'm still grouped; please ask me again outside the instance.");
                Report(action);
            }
            else if (action.failureReason != living_party_release::Code(blocker))
            {
                action.failureReason = living_party_release::Code(blocker);
                Report(action);
            }
        }
        else if (action.state == "waiting_for_vendor_trip")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            Group* group = bot ? bot->GetGroup() : nullptr;
            if (!bot || !player || !ValidateCommon(bot, player) || !group ||
                group != player->GetGroup() || group->GetId() != action.groupId ||
                !group->IsLeader(player->GetObjectGuid()))
            {
                action.state = "rejected";
                action.failureReason = "party or leader state changed before personal errands could begin";
                Report(action);
            }
            else if (!HasActiveVendorTrip(action.botGuid))
            {
                if (sPlayerbotRendezvousManager.IsPartyFreeTime(action.botGuid) ||
                    sPlayerbotRendezvousManager.BeginPartyFreeTime(
                        bot, player, "party_leader_granted_free_time_after_maintenance"))
                {
                    action.state = "completed";
                    action.completedAt = now;
                    Report(action);
                }
                else if (now >= action.expires)
                {
                    action.state = "expired";
                    action.failureReason = "personal errands could not begin safely after maintenance";
                    Report(action);
                }
                else if (!bot->IsInCombat())
                    sPlayerbotRendezvousManager.ResumePartyAssist(
                        bot, player, "party_free_time_waiting_for_safe_return");
            }
            else if (now >= action.expires)
            {
                action.state = "expired";
                action.failureReason = "the existing maintenance trip did not finish before personal errands expired";
                Report(action);
            }
        }
        else if (action.state == "vendor_relocating")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            long transitSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - action.stateSince).count();
            if (bot && player && bot->IsInWorld() && !bot->IsBeingTeleported() &&
                bot->GetGroup() == player->GetGroup())
            {
                action.state = "vendor_travel";
                action.stateSince = now;
                bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest npcs");
                Report(action);
            }
            else if (!bot || !player || transitSeconds >= 45)
            {
                if (bot && bot->GetPlayerbotAI())
                {
                    bot->GetPlayerbotAI()->ChangeStrategy("nc -travel once",
                        BotState::BOT_STATE_NON_COMBAT);
                    TravelTarget* staleTarget = bot->GetPlayerbotAI()->GetAiObjectContext()->
                        GetValue<TravelTarget*>("travel target")->Get();
                    sTravelMgr.SetNullTravelTarget(staleTarget);
                    bot->GetPlayerbotAI()->RequestStrategyReset(true);
                }
                action.state = "failed";
                action.failureReason = "vendor relocation worldport did not complete";
                action.completedAt = now;
                sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
                    PlayerbotRendezvousManager::PartyActivityPhase::failed,
                    "vendor_relocation_ack_timeout");
                Report(action);
            }
        }
        else if (action.state == "vendor_travel")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            if (!bot || !player || !ValidateCommon(bot, player) || bot->GetGroup() != player->GetGroup())
            {
                action.state = "failed";
                action.failureReason = "party or character state changed during vendor trip";
                if (bot && bot->GetPlayerbotAI())
                {
                    PlayerbotAI* ai = bot->GetPlayerbotAI();
                    ai->ChangeStrategy("nc -travel once", BotState::BOT_STATE_NON_COMBAT);
                    TravelTarget* staleTarget = ai->GetAiObjectContext()->
                        GetValue<TravelTarget*>("travel target")->Get();
                    sTravelMgr.SetNullTravelTarget(staleTarget);
                    ai->GetAiObjectContext()->ClearValues("no active travel destinations");
                    ai->StopMoving();
                    bool sameParty = player && bot->GetGroup() &&
                        bot->GetGroup() == player->GetGroup();
                    bool resumed = sameParty &&
                        sPlayerbotRendezvousManager.ResumePartyAssist(bot, player,
                            "vendor_trip_participant_unavailable");
                    if (!resumed)
                        ai->RequestStrategyReset(true);
                }
                action.restoreFollow = false;
                action.completedAt = now;
                sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
                    PlayerbotRendezvousManager::PartyActivityPhase::failed,
                    "vendor_trip_participant_unavailable");
                Report(action);
            }
            else
            {
                TravelTarget* target = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
                bool targetReady = target && target->GetPosition() &&
                    (target->GetStatus() == TravelStatus::TRAVEL_STATUS_TRAVEL ||
                     target->GetStatus() == TravelStatus::TRAVEL_STATUS_READY);
                if (targetReady)
                {
                    // Human-led bots do not normally load TravelStrategy. A
                    // scoped travel-once strategy makes this one validated
                    // vendor target executable and removes itself on arrival.
                    bot->GetPlayerbotAI()->ChangeStrategy("nc +travel once", BotState::BOT_STATE_NON_COMBAT);
                    long departureSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                        now - action.stateSince).count();
                    if (!action.outboundRelocated && !bot->IsInCombat() && departureSeconds >= 3)
                    {
                        WorldPosition* destination = target->GetPosition();
                        // Keep walking out of sight while relocation is denied.
                        // Stopping before the visibility/slot checks trapped a
                        // departing bot beside the human until the human moved.
                        bool sameMap = destination->getMapId() == bot->GetMapId();
                        bool relocated = false;
                        if (sameMap)
                        {
                            if (sPlayerbotRendezvousManager.CanRelocateUnobserved(bot,
                                bot->GetMap(), destination->getX(), destination->getY(),
                                destination->getZ()) &&
                                sPlayerbotRendezvousManager.ClaimRelocationSlot())
                            {
                                bot->GetPlayerbotAI()->StopMoving();
                                bot->NearTeleportTo(destination->getX(), destination->getY(),
                                    destination->getZ(), destination->getO());
                                relocated = true;
                            }
                        }
                        else if (bot->GetMap() && bot->GetMap()->IsDungeon())
                        {
                            Map* destinationMap = sMapMgr.FindMap(destination->getMapId(), 0);
                            if (sPlayerbotRendezvousManager.CanRelocateUnobserved(bot,
                                destinationMap, destination->getX(), destination->getY(),
                                destination->getZ()) &&
                                sPlayerbotRendezvousManager.ClaimRelocationSlot())
                            {
                                bot->GetPlayerbotAI()->StopMoving();
                                relocated = bot->TeleportTo(destination->getMapId(), destination->getX(),
                                    destination->getY(), destination->getZ(), destination->getO());
                            }
                        }
                        action.outboundRelocated = relocated;
                        if (relocated)
                        {
                            if (!sameMap)
                            {
                                action.state = "vendor_relocating";
                                action.stateSince = now;
                            }
                            bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest npcs");
                            sLog.outString("Living WoW vendor maintenance bot=%u name=%s result=relocated map=%u area=%s",
                                bot->GetGUIDLow(), bot->GetName(), destination->getMapId(),
                                destination->getAreaName().c_str());
                        }
                    }
                }

                if (action.state == "vendor_relocating")
                    continue;

                if (!bot->IsBeingTeleported() && target && (target->GetStatus() == TravelStatus::TRAVEL_STATUS_WORK ||
                    target->Distance(bot) <= INTERACTION_DISTANCE))
                {
                    bot->GetPlayerbotAI()->ChangeStrategy("nc -travel once", BotState::BOT_STATE_NON_COMBAT);
                    bot->GetPlayerbotAI()->StopMoving();
                    long sinceSell = action.lastSellAttempt.time_since_epoch().count() ?
                        std::chrono::duration_cast<std::chrono::seconds>(now - action.lastSellAttempt).count() : 2;
                    if (sinceSell >= 1)
                    {
                        action.lastSellAttempt = now;
                        ++action.sellAttempts;
                        // Relocation invalidates the cached nearby-NPC list.
                        // Refresh it before the vendor/banker interaction so
                        // preflight and execution see the same destination.
                        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest npcs");
                        if (action.maintenanceType == "vendor" && !action.repairAttempted)
                        {
                            action.repairAttempted = true;
                            bot->GetPlayerbotAI()->DoSpecificAction("repair",
                                Event("rpg action", "living-wow-maintenance", player), true);
                        }
                        bool sold = action.maintenanceType == "bank" ?
                            bot->GetPlayerbotAI()->DoSpecificAction("bank",
                                Event("rpg action", "living-wow-safe-storage", nullptr), true) :
                            bot->GetPlayerbotAI()->DoSpecificAction("sell",
                                Event("rpg action", "living-wow-safe-vendor", player), true);
                        // Bag-space is a cached Playerbots value. Invalidate it
                        // after each real sell attempt so completion observes
                        // the changed inventory instead of the pre-trip value.
                        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bag space");
                        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bank space");
                        sLog.outString("Living WoW vendor maintenance bot=%u name=%s result=%s attempt=%u distance=%.1f",
                            bot->GetGUIDLow(), bot->GetName(), sold ?
                                (action.maintenanceType == "bank" ? "bank_action" : "sell_action") :
                                (action.maintenanceType == "bank" ? "nothing_safe_to_bank" : "nothing_safe_to_sell"),
                            (uint32)action.sellAttempts, target->Distance(bot));
                    }
                }
                uint8 usage = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint8>("bag space")->Get();
                action.bestBagUsage = std::min(action.bestBagUsage, usage);
                LivingWowInventoryPressureSummary remaining = sPlayerbotInventoryPressure.Analyze(bot);
                bool serviceTimeElapsed = now >= action.serviceReadyAt;
                bool targetReached = usage <= action.targetBagUsage;
                bool noSafeMaintenance = !remaining.vendorStacks && !remaining.HasBankableStorage();
                if (serviceTimeElapsed && (targetReached ||
                    (noSafeMaintenance && usage < action.initialBagUsage)))
                {
                    if (targetReached)
                        bot->GetPlayerbotAI()->SayToParty(action.maintenanceType == "bank" ?
                            "I put the things I need to keep in the bank. Heading back now." :
                            "I cleared enough bag space to keep going. Heading back now.", true,
                            PlayerbotAI::ChatMessageClass::social);
                    else
                    {
                        std::ostringstream notice;
                        notice << "I freed what I safely could. My bags are still " << (uint32)usage
                            << "% full because the rest is protected, so I'm heading back.";
                        bot->GetPlayerbotAI()->SayToParty(notice.str(), true,
                            PlayerbotAI::ChatMessageClass::social);
                    }
                    QueuePartyReturn(action, bot, player,
                        targetReached ? "vendor_trip_complete" : "vendor_trip_partial",
                        targetReached);
                }
                else if (action.sellAttempts >= 5)
                {
                    if (!ContinueAtBank(action, bot))
                    {
                        if (!serviceTimeElapsed)
                            continue;
                        action.failureReason = "no additional safe maintenance items freed a bag slot";
                        sPlayerbotInventoryPressure.Defer(bot, remaining, "quick_maintenance_freed_no_slot");
                        bot->GetPlayerbotAI()->SayToParty(action.bestBagUsage < action.initialBagUsage ?
                            "I freed what I safely could. The rest is quest gear or other protected supplies, so I'm heading back." :
                            "I couldn't free a slot without losing quest items or other protected supplies. I'm heading back.", true,
                            PlayerbotAI::ChatMessageClass::social);
                        QueuePartyReturn(action, bot, player, "vendor_trip_no_space_freed",
                            action.bestBagUsage < action.initialBagUsage);
                    }
                }
                else if (now >= action.expires)
                {
                    action.failureReason = "vendor trip did not free bag space in time";
                    LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
                    sPlayerbotInventoryPressure.Defer(bot, pressure, "party_maintenance_timeout");
                    QueuePartyReturn(action, bot, player, "vendor_trip_timeout", false);
                }
            }
        }
        else if (action.state == "return_pending")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            bool invalid = !bot || !player || !bot->IsInWorld() || !player->IsInWorld() ||
                !bot->GetGroup() || bot->GetGroup() != player->GetGroup();
            if (invalid || now >= action.expires)
            {
                if (bot && bot->GetPlayerbotAI())
                {
                    bot->GetPlayerbotAI()->ChangeStrategy("nc -travel once",
                        BotState::BOT_STATE_NON_COMBAT);
                    TravelTarget* staleTarget = bot->GetPlayerbotAI()->GetAiObjectContext()->
                        GetValue<TravelTarget*>("travel target")->Get();
                    sTravelMgr.SetNullTravelTarget(staleTarget);
                    bot->GetPlayerbotAI()->RequestStrategyReset(true);
                }
                action.state = "failed";
                action.failureReason = invalid ? "party changed while return relocation was pending" :
                    "party return relocation timed out";
                action.completedAt = now;
                sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
                    PlayerbotRendezvousManager::PartyActivityPhase::failed,
                    invalid ? "party_return_participant_unavailable" : "party_return_timeout");
                Report(action);
            }
            else
                QueuePartyReturn(action, bot, player, "vendor_return_retry",
                    action.maintenanceSucceeded);
        }
        else if (action.state == "returning")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            if (bot && player && bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                if (action.restoreFollow)
                    bot->GetPlayerbotAI()->ChangeStrategy("nc +follow", BotState::BOT_STATE_NON_COMBAT);
                action.restoreFollow = false;
                action.state = "completed";
                action.completedAt = now;
                sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
                    PlayerbotRendezvousManager::PartyActivityPhase::completed,
                    "party_return_completed");
                Report(action);
            }
            else if (now >= action.expires)
            {
                if (bot && bot->GetPlayerbotAI() && action.restoreFollow)
                    bot->GetPlayerbotAI()->ChangeStrategy("nc +follow", BotState::BOT_STATE_NON_COMBAT);
                action.restoreFollow = false;
                action.state = "failed";
                action.failureReason = "party return timed out";
                action.completedAt = now;
                sPlayerbotRendezvousManager.ReleasePartyActivityLease(action.lease,
                    PlayerbotRendezvousManager::PartyActivityPhase::failed,
                    "party_return_timeout");
                Report(action);
            }
            else if (bot && player && bot->IsInWorld() && player->IsInWorld() &&
                !bot->IsBeingTeleported() && !bot->IsInCombat() &&
                bot->GetMapId() == player->GetMapId() &&
                bot->GetInstanceId() == player->GetInstanceId())
            {
                bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
            }
        }
        else if (action.state == "meeting")
        {
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
            Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
            if (bot && player && bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                action.state = "completed";
                action.completedAt = now;
                Report(action);
            }
            else if (now >= action.expires)
            {
                action.state = "expired";
                action.failureReason = "meeting offer expired";
                sPlayerbotRendezvousManager.BeginDeparture(action.botGuid, action.playerGuid, action.failureReason);
                Report(action);
            }
        }
        else if (action.state == "completed" && action.type == "meet_player" &&
            std::chrono::duration_cast<std::chrono::seconds>(now - action.completedAt).count() >= 30)
        {
            sPlayerbotRendezvousManager.BeginDeparture(action.botGuid, action.playerGuid, "meetup_completed");
            action.state = "departing";
            Report(action);
        }
        if (action.state == "preparing" && now >= action.expires)
        {
            action.state = "expired";
            action.failureReason = "social action expired";
            Report(action);
        }
    }
    for (auto it = preferredQuests.begin(); it != preferredQuests.end();)
        if (it->second.second <= now) it = preferredQuests.erase(it); else ++it;
}

void PlayerbotSocialActionBroker::Report(const Action& action) const
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(action.botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, action.playerGuid));
    std::string partySessionId = bot ? sPlayerbotRendezvousManager.GetPartySessionId(bot) :
        (player ? sPlayerbotRendezvousManager.GetPartySessionId(player) : "");
    // Preserve the old gateway contract; the optional detail distinguishes
    // accepted waiting from actual travel without pretending work started.
    const std::string reportedState=action.state=="vendor_admission_wait"?"preparing":action.state;
    std::ostringstream body;
    body << "{\"transaction_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(action.actionId)
         << "\",\"event_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(action.eventId)
         << "\",\"proposal_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(action.proposalId)
         << "\",\"party_session_id\":\"" << partySessionId
         << "\",\"bot_guid\":" << action.botGuid << ",\"bot_name\":\""
         << PlayerbotLLMInterface::SanitizeForJson(bot ? bot->GetName() : "") << "\",\"player_guid\":" << action.playerGuid
         << ",\"player_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(player ? player->GetName() : "")
         << "\",\"type\":\"" << action.type << "\",\"capability_ref\":\""
         << PlayerbotLLMInterface::SanitizeForJson(action.capabilityRef)
         << "\",\"item_name\":\"\",\"quantity\":0,\"price_copper\":0,\"delivery\":\"immediate\",\"state\":\""
         << reportedState << "\",\"activity_phase\":\"" << action.state
         << "\",\"rendezvous_state\":\"" << sPlayerbotRendezvousManager.State(action.botGuid, action.playerGuid)
         << "\",\"catchup_relocated\":" << (sPlayerbotRendezvousManager.WasRelocated(action.botGuid, action.playerGuid) ? "true" : "false")
         << ",\"failure_reason\":\"" << PlayerbotLLMInterface::SanitizeForJson(action.failureReason)
         << "\",\"group_id\":" << action.groupId << ",\"quest_id\":" << action.questId << ",\"expires_at\":\"world-clock\"}";
    std::string payload = body.str();
    std::thread([payload]() {
        std::vector<std::string> debug;
        PlayerbotLLMInterface::Generate(payload, 3, 2, debug, true, "/v2/action-status");
    }).detach();
}
