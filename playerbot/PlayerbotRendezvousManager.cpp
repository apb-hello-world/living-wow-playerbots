#include "botpch.h"
#include "PlayerbotRendezvousManager.h"
#include "LivingActivityCoordinator.h"
#include "PlayerbotGuildEventExecutor.h"
#include "PlayerbotPartyCatchup.h"
#include "LootObjectStack.h"
#include "strategy/actions/FollowActions.h"
#include "strategy/values/Formations.h"

#include "Entities/Transports.h"
#include "Mails/Mail.h"
#include "MotionGenerators/PathFinder.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotInventoryPressure.h"
#include "PlayerbotTraining.h"
#include "PlayerbotOrganicEconomy.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/ItemVisitors.h"
#include "strategy/values/TravelValues.h"
#include "strategy/values/LastMovementValue.h"

#include <cmath>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace
{
    constexpr float kRunSpeedYardsPerSecond = 7.0f;
    constexpr uint32 kErrandVendor = 1 << 0;
    constexpr uint32 kErrandRepair = 1 << 1;
    constexpr uint32 kErrandBank = 1 << 2;
    constexpr uint32 kErrandMail = 1 << 3;
    constexpr uint32 kErrandAuction = 1 << 4;
    constexpr uint32 kErrandProfession = 1 << 5;
    constexpr uint32 kErrandTraining = 1 << 6;
    constexpr uint32 kAllErrands = kErrandVendor | kErrandRepair | kErrandBank |
        kErrandMail | kErrandAuction | kErrandProfession | kErrandTraining;
    constexpr uint32 kPartyPersistenceSeconds = 7 * 24 * 60 * 60;
    constexpr size_t kPartyPersistenceDataLimit = 255;
    constexpr size_t kPartyPersistenceTaskIdLimit = 32;
    constexpr size_t kPartyPersistenceActivityLimit = 16;
    // v1 uses 18 separators, six ten-digit uint32 identity fields plus the
    // expiry, four bounded fixed-point coordinates, one relocated flag, four
    // three-digit errand masks, the two capped tokens, and a two-digit state.
    constexpr size_t kPartyPersistenceWorstCase = 1 + (6 * 10) + 10 +
        (3 * 10) + 8 + 1 + (4 * 3) + kPartyPersistenceTaskIdLimit +
        kPartyPersistenceActivityLimit + 2 + 18;
    static_assert(kPartyPersistenceWorstCase <= kPartyPersistenceDataLimit,
        "living party persistence must fit ai_playerbot_random_bots.data");
    const char* kPartyPersistenceEvent = "living_party_session_v1";

    enum class PersistedPartyState : uint32
    {
        invalid = 0,
        pending = 1,
        instance_handoff = 2,
        relocating = 3,
        approaching = 4,
        handoff = 5,
        active = 6,
        free_time = 7,
        hearth_sync = 8,
        departing = 9,
        returning = 10,
        dungeon_return = 11
    };

    const char* ErrandName(uint32 errand)
    {
        if (errand == kErrandTraining) return "training";
        if (errand == kErrandVendor) return "vendor";
        if (errand == kErrandRepair) return "repair";
        if (errand == kErrandBank) return "bank";
        if (errand == kErrandMail) return "mail";
        if (errand == kErrandAuction) return "auction";
        if (errand == kErrandProfession) return "profession";
        return "none";
    }

    std::string ActivityToken(const std::string& value)
    {
        std::string result;
        for (char c : value)
        {
            if (result.size() >= 48) break;
            if (std::isalnum((unsigned char)c)) result.push_back((char)std::tolower((unsigned char)c));
            else if (!result.empty() && result.back() != '_') result.push_back('_');
        }
        while (!result.empty() && result.back() == '_') result.pop_back();
        return result.empty() ? "none" : result;
    }

    std::string PersistenceToken(const std::string& value, size_t maximumLength)
    {
        std::string token = ActivityToken(value);
        if (token.size() > maximumLength) token.resize(maximumLength);
        return token;
    }

    PersistedPartyState PersistenceState(const std::string& state)
    {
        if (state == "pending") return PersistedPartyState::pending;
        if (state == "instance_handoff") return PersistedPartyState::instance_handoff;
        if (state == "relocating") return PersistedPartyState::relocating;
        if (state == "approaching") return PersistedPartyState::approaching;
        if (state == "handoff") return PersistedPartyState::handoff;
        if (state == "active") return PersistedPartyState::active;
        if (state == "free_time") return PersistedPartyState::free_time;
        if (state == "hearth_sync") return PersistedPartyState::hearth_sync;
        if (state == "departing") return PersistedPartyState::departing;
        if (state == "returning") return PersistedPartyState::returning;
        if (state == "dungeon_return") return PersistedPartyState::dungeon_return;
        return PersistedPartyState::invalid;
    }

    bool IsLivePartyState(const std::string& state)
    {
        return state == "pending" || state == "instance_handoff" ||
            state == "relocating" || state == "approaching" ||
            state == "handoff" || state == "active" ||
            state == "free_time" || state == "hearth_sync";
    }

    bool IsRealObserver(Player* player)
    {
        return player && player->IsInWorld() && player->isRealPlayer();
    }

    bool IsCastingHearthstone(Player* player)
    {
        Spell* spell = player ? player->GetCurrentSpell(CURRENT_GENERIC_SPELL) : nullptr;
        return spell && spell->m_spellInfo && spell->m_spellInfo->Id == 8690;
    }

    bool IsCapital(Player* player)
    {
        AreaTableEntry const* area = player ? GetAreaEntryByAreaID(sServerFacade.GetAreaId(player)) : nullptr;
        AreaTableEntry const* zone = player ? GetAreaEntryByAreaID(player->GetZoneId()) : nullptr;
        return (area && (area->flags & AREA_FLAG_CAPITAL)) || (zone && (zone->flags & AREA_FLAG_CAPITAL));
    }

    uint32 SettlementKey(Player* bot, Player* human)
    {
        if (!bot || !human || bot->GetMapId() != human->GetMapId() ||
            !bot->IsWithinDistInMap(human, 120.0f))
            return 0;
        if (IsCapital(human))
            return 0x80000000u | human->GetZoneId();

        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai)
            return 0;
        bool vendor = false, supportingService = false;
        uint32 serviceKinds = 0;
        std::list<ObjectGuid> npcs = ai->GetAiObjectContext()->
            GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get();
        for (ObjectGuid const& guid : npcs)
        {
            Unit* npc = ai->GetUnit(guid);
            if (!npc)
                continue;
            if (npc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_REPAIR))
            {
                vendor = true;
                ++serviceKinds;
            }
            if (npc->HasFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_TRAINER | UNIT_NPC_FLAG_TRAINER_CLASS |
                UNIT_NPC_FLAG_TRAINER_PROFESSION | UNIT_NPC_FLAG_INNKEEPER | UNIT_NPC_FLAG_BANKER |
                UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_FLIGHTMASTER))
            {
                supportingService = true;
                ++serviceKinds;
            }
        }
        return vendor && supportingService && serviceKinds >= 2 ? sServerFacade.GetAreaId(human) : 0;
    }

    uint32 FindExecutableProfessionSpell(Player* bot);

    uint32 PersonalErrandMask(Player* bot)
    {
        if (!bot || !bot->GetPlayerbotAI())
            return 0;
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        AiObjectContext* context = ai->GetAiObjectContext();
        LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
        uint32 errands = 0;
        if (!context->GetValue<std::vector<TrainerSpell const*>>(
            "trainable spells", std::to_string(TRAINER_TYPE_CLASS))->Get().empty())
            errands |= kErrandTraining;
        if (pressure.vendorStacks && context->GetValue<bool>("can sell")->Get())
            errands |= kErrandVendor;
        if (context->GetValue<uint8>("durability inventory")->Get() < 85 &&
            context->GetValue<bool>("can repair")->Get())
            errands |= kErrandRepair;
        if (pressure.StorableStacks() && (pressure.bagUsage >= 70 || pressure.StorableStacks() >= 3) &&
            context->GetValue<bool>("should bank deposit")->Get())
            errands |= kErrandBank;

        time_t now = time(nullptr);
        for (PlayerMails::iterator mail = bot->GetMailBegin(); mail != bot->GetMailEnd(); ++mail)
        {
            if ((*mail)->state != MAIL_STATE_DELETED && now >= (*mail)->deliver_time &&
                ((*mail)->has_items || (*mail)->money))
            {
                if (pressure.bagUsage < 100) errands |= kErrandMail;
                break;
            }
        }

        std::string goal = sPlayerbotOrganicEconomy.CurrentGoalType(bot->GetGUIDLow());
        if ((pressure.auctionStacks || goal == "list_surplus") &&
            sPlayerbotOrganicEconomy.IsAuctionPostingEnabled() &&
            context->GetValue<bool>("can ah sell")->Get())
            errands |= kErrandAuction;
        if (goal == "profession_skill_up" && FindExecutableProfessionSpell(bot))
            errands |= kErrandProfession;
        return errands;
    }

    uint32 FindExecutableProfessionSpell(Player* bot)
    {
        if (!bot || !bot->GetPlayerbotAI()) return 0;
        AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
        std::vector<uint32> spells = context->GetValue<std::vector<uint32>>("craft spells")->Get();
        for (uint32 spellId : spells)
        {
            const SpellEntry* spell = sServerFacade.LookupSpellInfo(spellId);
            if (spell && spell->EffectItemType[0] && !spell->RequiresSpellFocus &&
                context->GetValue<bool>("can craft spell", std::to_string(spellId))->Get() &&
                context->GetValue<bool>("should craft spell", std::to_string(spellId))->Get())
                return spellId;
        }
        return 0;
    }

    uint32 InventoryItemCount(Player* bot, uint32 itemEntry)
    {
        if (!bot || !bot->GetPlayerbotAI() || !itemEntry) return 0;
        return bot->GetPlayerbotAI()->GetAiObjectContext()->
            GetValue<uint32>("item count", std::to_string(itemEntry))->Get();
    }

    TravelDestinationPurpose ErrandPurpose(uint32 errand)
    {
        if (errand == kErrandVendor)
            return TravelDestinationPurpose::Vendor;
        if (errand == kErrandRepair)
            return TravelDestinationPurpose::Repair;
        if (errand == kErrandBank)
            return TravelDestinationPurpose::Bank;
        if (errand == kErrandMail)
            return TravelDestinationPurpose::Mail;
        if (errand == kErrandAuction)
            return TravelDestinationPurpose::AH;
        return TravelDestinationPurpose::Trainer;
    }

    bool FindSettlementErrandDestination(Player* bot, uint32 errand,
        TravelDestination*& selectedDestination, WorldPosition*& selectedPosition)
    {
        selectedDestination = nullptr;
        selectedPosition = nullptr;
        if (!bot || !bot->GetPlayerbotAI())
            return false;
        PlayerTravelInfo info(bot);
        std::vector<int32> entries;
        if (errand == kErrandTraining)
        {
            entries = bot->GetPlayerbotAI()->GetAiObjectContext()->
                GetValue<std::vector<int32>>("available trainers", std::to_string(TRAINER_TYPE_CLASS))->Get();
            // An empty filter means ALL destinations to TravelMgr.
            if (entries.empty()) return false;
        }
        DestinationList destinations = sTravelMgr.GetDestinations(
            info, (uint32)ErrandPurpose(errand), entries, true, 0.0f, true);
        WorldPosition center(bot);
        float bestDistance = std::numeric_limits<float>::max();
        for (TravelDestination* destination : destinations)
        {
            if (!destination)
                continue;
            // A service must be usable by this bot, even on a distant map.
            const int32 entry = destination->GetEntry();
            if (GuidPosition(entry > 0 ? HIGHGUID_UNIT : HIGHGUID_GAMEOBJECT,
                uint32(std::abs(entry))).IsHostileTo(bot)) continue;
            if (errand == kErrandTraining && !LivingWowHasClassTraining(bot, entry)) continue;
            // Evaluate actual spawns rather than choosing a random far square.
            // WorldPosition::distance includes known map-transfer links; FLT_MAX
            // means no connection. MovementAction resolves the detailed route.
            for (WorldPosition* position : destination->GetPoints())
            {
                if (!position || !position->isOverworld()) continue;
                float distance = center.distance(*position);
                if (!std::isfinite(distance) || distance >= bestDistance) continue;
                bestDistance = distance;
                selectedDestination = destination;
                selectedPosition = position;
            }
        }
        return selectedDestination && selectedPosition;
    }

    uint32 GroundedSettlementErrandMask(Player* bot)
    {
        uint32 requested = PersonalErrandMask(bot);
        uint32 grounded = 0;
        const uint32 errands[] = {kErrandVendor, kErrandRepair, kErrandBank,
            kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
        for (uint32 errand : errands)
        {
            if (!(requested & errand))
                continue;
            // No-focus profession recipes are exact local capabilities. They
            // need neither a trainer nor an invented station destination.
            if (errand == kErrandProfession)
            {
                if (sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands)
                    grounded |= errand;
                continue;
            }
            TravelDestination* destination = nullptr;
            WorldPosition* position = nullptr;
            if (FindSettlementErrandDestination(bot, errand, destination, position))
            {
                grounded |= errand;
                continue;
            }

        }
        return grounded;
    }

    std::string ErrandTelemetryNames(uint32 errands);

    bool SetAutomaticErrandTarget(Player* bot, uint32 errands)
    {
        if (!bot || !bot->GetPlayerbotAI() || !errands)
            return false;
        const uint32 priorities[] = {kErrandTraining, kErrandVendor, kErrandRepair, kErrandBank,
            kErrandMail, kErrandAuction, kErrandProfession};
        for (uint32 errand : priorities)
        {
            if (!(errands & errand))
                continue;
            TravelDestination* destination = nullptr;
            WorldPosition* position = nullptr;
            if (!FindSettlementErrandDestination(bot, errand, destination, position))
                continue;
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
            sTravelMgr.SetNullTravelTarget(target);
            target->SetTarget(destination, position);
            target->SetForced(true);
            target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
            ai->GetAiObjectContext()->ClearValues("no active travel destinations");
            ai->ChangeStrategy("nc +travel once", BotState::BOT_STATE_NON_COMBAT);
            ai->StopMoving();
            sLog.outString("Living WoW party errands event=target_selected bot=%u task=%s map=%u area=%s distance=%.1f",
                bot->GetGUIDLow(), ErrandTelemetryNames(errand).c_str(), position->getMapId(),
                position->getAreaName().c_str(), WorldPosition(bot).distance(*position));
            return true;
        }
        return false;
    }

    std::vector<std::string> PersonalErrands(uint32 errands)
    {
        std::vector<std::string> names;
        if (errands & kErrandTraining)
            names.push_back("learn new spells at my class trainer");
        if (errands & kErrandVendor)
            names.push_back("sell some junk");
        if (errands & kErrandRepair)
            names.push_back("repair my gear");
        if (errands & kErrandBank)
            names.push_back("put some materials in the bank");
        if (errands & kErrandMail)
            names.push_back("pick up my mail");
        if (errands & kErrandAuction)
            names.push_back("check the auction house");
        if (errands & kErrandProfession)
            names.push_back("work on my profession");
        return names;
    }

    std::string ErrandTelemetryNames(uint32 errands)
    {
        std::string names;
        auto append = [&names](const char* name)
        {
            if (!names.empty()) names += ',';
            names += name;
        };
        if (errands & kErrandTraining) append("training");
        if (errands & kErrandVendor) append("vendor");
        if (errands & kErrandRepair) append("repair");
        if (errands & kErrandBank) append("bank");
        if (errands & kErrandMail) append("mail");
        if (errands & kErrandAuction) append("auction");
        if (errands & kErrandProfession) append("profession");
        return names.empty() ? "none" : names;
    }

    std::string ErrandAnnouncement(std::vector<std::string> const& errands)
    {
        if (errands.empty())
            return "";
        std::ostringstream message;
        message << "I've got to ";
        size_t shown = std::min<size_t>(3, errands.size());
        for (size_t index = 0; index < shown; ++index)
        {
            if (index)
                message << (index + 1 == shown ? (shown == 2 ? " and " : ", and ") : ", ");
            message << errands[index];
        }
        if (errands.size() > shown)
            message << ", plus a couple other things";
        message << ". I'll catch back up when I'm done.";
        return message.str();
    }

    std::string ActivityJsonString(const std::string& value)
    {
        std::string escaped;
        escaped.reserve(value.size());
        for (char c : value)
        {
            if (c == '\\' || c == '"') escaped.push_back('\\');
            if (c == '\n' || c == '\r' || (unsigned char)c < 0x20) continue;
            escaped.push_back(c);
        }
        return escaped;
    }

    std::string ActivityEnumToken(const std::string& value, const char* fallback)
    {
        std::string token;
        token.reserve(std::min<size_t>(48, value.size()));
        bool underscore = false;
        for (unsigned char c : value)
        {
            if (token.size() >= 48)
                break;
            if (std::isalnum(c))
            {
                token.push_back((char)std::tolower(c));
                underscore = false;
            }
            else if (!token.empty() && !underscore)
            {
                token.push_back('_');
                underscore = true;
            }
        }
        while (!token.empty() && token.back() == '_')
            token.pop_back();
        if (token.empty() || !std::isalpha((unsigned char)token[0]))
            return fallback;
        return token;
    }
}

PlayerbotRendezvousManager& PlayerbotRendezvousManager::instance()
{
    static PlayerbotRendezvousManager manager;
    return manager;
}

const char* PlayerbotRendezvousManager::PartyActivityOwnerName(PartyActivityOwner owner)
{
    switch (owner)
    {
        case PartyActivityOwner::party_follow: return "party_follow";
        case PartyActivityOwner::combat: return "combat";
        case PartyActivityOwner::death_recovery: return "death_recovery";
        case PartyActivityOwner::transport: return "transport";
        case PartyActivityOwner::rendezvous: return "rendezvous";
        case PartyActivityOwner::party_errand: return "party_errand";
        case PartyActivityOwner::guild_event: return "guild_event";
        case PartyActivityOwner::guild_supply: return "guild_supply";
        case PartyActivityOwner::economy_service: return "economy_service";
        case PartyActivityOwner::player_command: return "player_command";
        default: return "none";
    }
}

const char* PlayerbotRendezvousManager::PartyActivityPhaseName(PartyActivityPhase phase)
{
    switch (phase)
    {
        case PartyActivityPhase::preparing: return "preparing";
        case PartyActivityPhase::departing: return "departing";
        case PartyActivityPhase::traveling: return "traveling";
        case PartyActivityPhase::performing: return "performing";
        case PartyActivityPhase::returning: return "returning";
        case PartyActivityPhase::verifying: return "verifying";
        case PartyActivityPhase::deferred: return "deferred";
        case PartyActivityPhase::blocked: return "blocked";
        case PartyActivityPhase::completed: return "completed";
        case PartyActivityPhase::failed: return "failed";
        default: return "idle";
    }
}

uint32 PlayerbotRendezvousManager::GetPartyRosterSignature(Player* participant) const
{
    if (!participant) return 0;
    Group* group = participant->GetGroup();
    if (!group) return participant->GetGUIDLow();

    std::vector<uint32> members;
    Group::MemberSlotList const& slots = group->GetMemberSlots();
    for (Group::MemberSlotList::const_iterator slot = slots.begin(); slot != slots.end(); ++slot)
        members.push_back(slot->guid.GetCounter());
    std::sort(members.begin(), members.end());

    uint32 signature = 2166136261u;
    auto mix = [&signature](uint32 value)
    {
        for (uint32 byte = 0; byte < 4; ++byte)
        {
            signature ^= (value >> (byte * 8)) & 0xff;
            signature *= 16777619u;
        }
    };
    mix(group->GetId());
    mix(group->GetLeaderGuid().GetCounter());
    mix(group->IsRaidGroup() ? 1u : 0u);
    for (uint32 guid : members) mix(guid);
    return signature;
}

uint64 PlayerbotRendezvousManager::GetPartySessionRevision(Player* participant) const
{
    if (!participant) return 0;
    Group* group = participant->GetGroup();
    if (!group) return participant->GetGUIDLow();

    uint32 signature = GetPartyRosterSignature(participant);
    if (!partyProcessEpoch) partyProcessEpoch = uint64(time(nullptr)) * 1000000ULL;
    GroupLifecycle& lifecycle = groupLifecycles[group->GetId()];
    if (!lifecycle.generation || lifecycle.signature != signature)
    {
        lifecycle.signature = signature;
        lifecycle.generation = partyProcessEpoch + ++partyGenerationSequence;
    }
    return lifecycle.generation;
}

std::string PlayerbotRendezvousManager::GetPartySessionId(Player* participant) const
{
    if (!participant) return "player:0";
    Group* group = participant->GetGroup();
    if (!group) return "player:" + std::to_string(participant->GetGUIDLow());
    std::ostringstream id;
    id << "party:" << group->GetId() << ':' << GetPartySessionRevision(participant);
    return id.str();
}

void PlayerbotRendezvousManager::PersistPartySession(const PartySession& session)
{
    if (!session.botGuid || !session.playerGuid || !session.groupId ||
        !session.partyRosterSignature)
        return;

    const PersistedPartyState state = PersistenceState(session.state);
    bool validOrigin = sMapStore.LookupEntry(session.originMapId) &&
        std::isfinite(session.originX) && std::isfinite(session.originY) &&
        std::isfinite(session.originZ) && std::isfinite(session.originO) &&
        std::fabs(session.originX) < 100000.0f &&
        std::fabs(session.originY) < 100000.0f &&
        std::fabs(session.originZ) < 100000.0f &&
        std::fabs(session.originO) < 1000.0f;
    if (state == PersistedPartyState::invalid || !validOrigin)
    {
        ClearPersistedPartySession(session.botGuid);
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
            "party_persistence_failed", state == PersistedPartyState::invalid ?
                "invalid_state" : "invalid_origin");
        return;
    }

    const uint32 now = uint32(time(nullptr));
    const uint32 expiresAt = now + kPartyPersistenceSeconds;
    std::ostringstream payload;
    payload << std::fixed << std::setprecision(3)
        << 1 << '|' << expiresAt << '|' << session.groupId << '|'
        << session.playerGuid << '|' << session.partyRosterSignature << '|'
        << session.originMapId << '|' << session.originInstanceId << '|'
        << session.originX << '|' << session.originY << '|' << session.originZ << '|'
        << session.originO << '|' << (session.relocated ? 1 : 0) << '|'
        << (session.automaticErrandScopeMask & kAllErrands) << '|'
        << (session.completedErrandMask & kAllErrands) << '|'
        << (session.deferredErrandMask & kAllErrands) << '|'
        << (session.currentErrand & kAllErrands) << '|'
        << PersistenceToken(session.currentErrandId, kPartyPersistenceTaskIdLimit) << '|'
        << PersistenceToken(session.previousActivity, kPartyPersistenceActivityLimit) << '|'
        << uint32(state);
    std::string data = payload.str();
    CharacterDatabase.escape_string(data);
    if (data.size() > kPartyPersistenceDataLimit)
    {
        ClearPersistedPartySession(session.botGuid);
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
            "party_persistence_failed", "payload_too_large");
        return;
    }

    // ai_playerbot_random_bots has no composite uniqueness constraint. Keep
    // the record outside owner=0 (whose clocks are advanced by RandomPlayerbot)
    // and replace it transactionally so a crash can never leave duplicates.
    CharacterDatabase.BeginTransaction();
    CharacterDatabase.PExecute(
        "DELETE FROM ai_playerbot_random_bots WHERE bot='%u' AND event='%s'",
        session.botGuid, kPartyPersistenceEvent);
    CharacterDatabase.PExecute(
        "INSERT INTO ai_playerbot_random_bots "
        "(owner,bot,`time`,validIn,event,`value`,`data`) "
        "VALUES ('%u','%u','%u','%u','%s','%u','%s')",
        session.playerGuid, session.botGuid, now,
        kPartyPersistenceSeconds, kPartyPersistenceEvent,
        expiresAt, data.c_str());
    CharacterDatabase.CommitTransaction();
}

void PlayerbotRendezvousManager::ClearPersistedPartySession(uint32 botGuid)
{
    if (!botGuid) return;
    CharacterDatabase.PExecute(
        "DELETE FROM ai_playerbot_random_bots WHERE bot='%u' AND event='%s'",
        botGuid, kPartyPersistenceEvent);
}

void PlayerbotRendezvousManager::PrunePersistedPartySessions()
{
    if (partyPersistencePruned) return;
    partyPersistencePruned = true;
    CharacterDatabase.PExecute(
        "DELETE FROM ai_playerbot_random_bots WHERE event='%s' "
        "AND (`value` IS NULL OR `value` < '%u')",
        kPartyPersistenceEvent, (uint32)time(nullptr));
}

bool PlayerbotRendezvousManager::RestorePersistedPartySession(
    Player* bot, Player* inviter, PartySession& session)
{
    if (!bot || !inviter || !bot->GetGroup()) return false;
    std::unique_ptr<QueryResult> result = CharacterDatabase.PQuery(
        "SELECT owner,`value`,`data` FROM ai_playerbot_random_bots "
        "WHERE bot='%u' AND event='%s' ORDER BY id DESC LIMIT 1",
        bot->GetGUIDLow(), kPartyPersistenceEvent);
    if (!result) return false;

    Field* fields = result->Fetch();
    const uint32 storedOwner = fields[0].GetUInt32();
    const uint32 storedExpiry = fields[1].GetUInt32();
    const std::string data = fields[2].GetString();
    std::vector<std::string> parts;
    std::stringstream input(data);
    std::string part;
    while (std::getline(input, part, '|')) parts.push_back(part);

    auto parseUnsigned = [](const std::string& value, uint64& parsed) -> bool
    {
        if (value.empty()) return false;
        char* end = nullptr;
        unsigned long long number = std::strtoull(value.c_str(), &end, 10);
        if (!end || *end != '\0') return false;
        parsed = number;
        return true;
    };
    auto parseFloat = [](const std::string& value, float& parsed) -> bool
    {
        if (value.empty()) return false;
        char* end = nullptr;
        double number = std::strtod(value.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(number)) return false;
        parsed = (float)number;
        return std::isfinite(parsed);
    };

    uint64 numeric[13] = {};
    bool valid = data.size() <= kPartyPersistenceDataLimit && parts.size() == 19;
    const size_t numericIndexes[] = {0,1,2,3,4,5,6,11,12,13,14,15};
    for (size_t index = 0; valid && index < 12; ++index)
        valid = parseUnsigned(parts[numericIndexes[index]], numeric[index]);
    valid = valid && parseUnsigned(parts[18], numeric[12]);
    for (size_t index = 0; valid && index < 13; ++index)
        valid = numeric[index] <= std::numeric_limits<uint32>::max();
    float originX = 0.0f, originY = 0.0f, originZ = 0.0f, originO = 0.0f;
    valid = valid && parseFloat(parts[7], originX) && parseFloat(parts[8], originY) &&
        parseFloat(parts[9], originZ) && parseFloat(parts[10], originO);

    const uint64 version = numeric[0], payloadExpiry = numeric[1];
    const uint32 groupId = (uint32)numeric[2], playerGuid = (uint32)numeric[3];
    const uint32 rosterSignature = (uint32)numeric[4];
    const uint32 originMapId = (uint32)numeric[5], originInstanceId = (uint32)numeric[6];
    const bool relocated = numeric[7] != 0;
    const uint32 scopeMask = (uint32)numeric[8], completedMask = (uint32)numeric[9];
    const uint32 deferredMask = (uint32)numeric[10], currentErrand = (uint32)numeric[11];
    const PersistedPartyState persistedState = (PersistedPartyState)(uint32)numeric[12];
    Group* group = bot->GetGroup();
    const uint32 currentRosterSignature = GetPartyRosterSignature(bot);
    std::string rejection = valid ? "stale_state" : "malformed_payload";
    if (valid && (version != 1 || persistedState <= PersistedPartyState::invalid ||
        persistedState > PersistedPartyState::dungeon_return ||
        numeric[7] > 1 || parts[16].size() > kPartyPersistenceTaskIdLimit ||
        parts[17].size() > kPartyPersistenceActivityLimit ||
        PersistenceToken(parts[16], kPartyPersistenceTaskIdLimit) != parts[16] ||
        PersistenceToken(parts[17], kPartyPersistenceActivityLimit) != parts[17]))
    {
        valid = false;
        rejection = "unsupported_payload";
    }
    if (valid && (payloadExpiry != storedExpiry || storedExpiry < uint64(time(nullptr))))
    {
        valid = false;
        rejection = "expired_state";
    }
    if (valid && (!storedOwner || storedOwner != playerGuid || !playerGuid ||
        groupId != group->GetId() ||
        !group->IsMember(ObjectGuid(HIGHGUID_PLAYER, playerGuid))))
    {
        valid = false;
        rejection = "party_or_human_mismatch";
    }
    if (valid && rosterSignature != currentRosterSignature)
    {
        valid = false;
        rejection = "roster_mismatch";
    }
    if (valid && ((scopeMask & ~kAllErrands) || (completedMask & ~scopeMask) ||
        (deferredMask & ~scopeMask) ||
        (currentErrand && ((currentErrand & (currentErrand - 1)) != 0 ||
            !(scopeMask & currentErrand)))))
    {
        valid = false;
        rejection = "invalid_errand_state";
    }
    if (valid && (!sMapStore.LookupEntry(originMapId) ||
        std::fabs(originX) >= 100000.0f || std::fabs(originY) >= 100000.0f ||
        std::fabs(originZ) >= 100000.0f || std::fabs(originO) >= 1000.0f))
    {
        valid = false;
        rejection = "invalid_origin";
    }
    if (!valid)
    {
        ClearPersistedPartySession(bot->GetGUIDLow());
        QueueActivityTelemetry(bot->GetGUIDLow(), inviter->GetGUIDLow(), group->GetId(),
            PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
            "persisted_party_session_rejected", rejection);
        return false;
    }

    session.playerGuid = inviter->GetGUIDLow();
    session.groupId = groupId;
    session.partyRosterSignature = currentRosterSignature;
    session.originMapId = originMapId;
    session.originInstanceId = originInstanceId;
    session.originX = originX;
    session.originY = originY;
    session.originZ = originZ;
    session.originO = originO;
    session.relocated = relocated;
    session.previousActivity = parts[17];
    session.automaticErrandScopeMask = scopeMask;
    session.completedErrandMask = completedMask;
    session.deferredErrandMask = deferredMask;

    const uint32 restartDeferred = scopeMask & ~(completedMask | deferredMask);
    const uint32 tasks[] = {kErrandVendor, kErrandRepair, kErrandBank,
        kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
    for (uint32 task : tasks)
    {
        if (!(scopeMask & task)) continue;
        PartySettlementErrand record;
        record.type = task;
        record.taskId = task == currentErrand && parts[16] != "none" ? parts[16] :
            "party_errand_" + std::to_string(session.botGuid) + "_recovered_" +
                std::to_string(task);
        if (completedMask & task)
        {
            record.phase = PartyActivityPhase::completed;
            record.outcomeCode = "completed_before_restart";
        }
        else
        {
            record.phase = PartyActivityPhase::deferred;
            record.outcomeCode = restartDeferred & task ? "realm_restart" :
                "deferred_before_restart";
        }
        session.errands[task] = record;
    }
    session.deferredErrandMask |= restartDeferred;
    session.currentErrand = 0;
    session.currentErrandId.clear();
    session.automaticErrandMask = 0;
    session.reason = restartDeferred ? "realm_restart_errands_deferred" :
        "realm_restart_party_recovered";
    return true;
}

std::string PlayerbotRendezvousManager::GetPartyActivityStateJson(uint32 botGuid) const
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    PartyActivityOwner owner = GetPartyActivityOwner(botGuid);
    PartyActivityPhase phase = GetPartyActivityPhase(botGuid);
    uint32 task = 0, deferred = 0;
    uint32 routeAttempts = 0, operationAttempts = 0;
    std::string blocker = "none";
    std::string sessionId = bot ? GetPartySessionId(bot) : "player:0";
    time_t leaseExpiresAt = 0;
    const auto now = std::chrono::steady_clock::now();

    auto party = partySessions.find(botGuid);
    if (party != partySessions.end())
    {
        task = party->second.currentErrand;
        deferred = party->second.deferredErrandMask;
        auto currentTask = party->second.errands.find(task);
        if (currentTask != party->second.errands.end())
        {
            routeAttempts = currentTask->second.routeAttempts;
            operationAttempts = currentTask->second.operationAttempts;
        }
        Player* human = sObjectAccessor.FindPlayer(
            ObjectGuid(HIGHGUID_PLAYER, party->second.playerGuid));
        if (human && bot && human->GetGroup() == bot->GetGroup())
            sessionId = GetPartySessionId(human);
        else if (!party->second.partySessionId.empty())
            sessionId = party->second.partySessionId;
        if (party->second.automaticErrandHardDeadline > now)
            leaseExpiresAt = time(nullptr) + std::chrono::duration_cast<std::chrono::seconds>(
                party->second.automaticErrandHardDeadline - now).count();
        if (phase == PartyActivityPhase::blocked || phase == PartyActivityPhase::deferred ||
            phase == PartyActivityPhase::failed)
            blocker = ActivityToken(party->second.reason);
        else if (deferred && !task)
            blocker = "tasks_deferred";
    }
    auto rendezvous = sessions.find(botGuid);
    if (rendezvous != sessions.end() && rendezvous->second.actionId.find("guild-event:") != 0)
    {
        Player* player = sObjectAccessor.FindPlayer(
            ObjectGuid(HIGHGUID_PLAYER, rendezvous->second.playerGuid));
        if (player && (!bot || !bot->GetGroup() || player->GetGroup() == bot->GetGroup()))
            sessionId = GetPartySessionId(player);
        else
            sessionId = "player:" + std::to_string(rendezvous->second.playerGuid);
        if (rendezvous->second.state == "arrived")
            leaseExpiresAt = time(nullptr) + std::max<long>(0,
                (long)sPlayerbotAIConfig.chatDirectorPartyReturnWaitSeconds -
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - rendezvous->second.stateSince).count());
    }
    auto external = externalLeases.find(botGuid);
    if (external != externalLeases.end() && external->second.expires > now)
        leaseExpiresAt = time(nullptr) + std::chrono::duration_cast<std::chrono::seconds>(
            external->second.expires - now).count();
    auto taskIdFor = [&party, this](uint32 errand) -> std::string
    {
        if (party == partySessions.end()) return "";
        auto found = party->second.errands.find(errand);
        return found == party->second.errands.end() ? "" : found->second.taskId;
    };

    if (!activitySequence) activitySequence = uint64(time(nullptr)) * 1000000ULL;
    uint64 stateRevision = ++activitySequence;

    std::ostringstream json;
    json << "{\"movement_owner\":\"" << PartyActivityOwnerName(owner)
         << "\",\"phase\":\"" << PartyActivityPhaseName(phase)
         << "\",\"task_type\":\"" << ErrandName(task)
         << "\",\"task_id\":\"";
    if (task) json << taskIdFor(task);
    json << "\",\"completed_tasks\":[";
    bool first = true;
    const uint32 errands[] = {kErrandVendor, kErrandRepair, kErrandBank,
        kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
    for (uint32 errand : errands)
    {
        if (party == partySessions.end()) continue;
        auto record = party->second.errands.find(errand);
        if (record != party->second.errands.end() &&
            record->second.phase == PartyActivityPhase::completed)
        {
            if (!first) json << ',';
            first = false;
            json << "{\"task_id\":\"" << taskIdFor(errand)
                 << "\",\"task_type\":\"" << ErrandName(errand)
                 << "\",\"outcome_code\":\"completed\"}";
        }
    }
    json << "],\"deferred_tasks\":[";
    first = true;
    for (uint32 errand : errands)
    {
        if (party == partySessions.end()) continue;
        auto record = party->second.errands.find(errand);
        if (record != party->second.errands.end() &&
            (record->second.phase == PartyActivityPhase::deferred ||
             record->second.phase == PartyActivityPhase::failed))
        {
            if (!first) json << ',';
            first = false;
            json << "{\"task_id\":\"" << taskIdFor(errand)
                 << "\",\"task_type\":\"" << ErrandName(errand)
                 << "\",\"blocker_code\":\"" << ActivityJsonString(record->second.outcomeCode)
                 << "\"}";
        }
    }
    json << "],\"blocker_code\":\"" << blocker
         << "\",\"party_session_id\":\"" << sessionId
         << "\",\"state_revision\":" << stateRevision
         << ",\"route_attempts\":" << routeAttempts
         << ",\"operation_attempts\":" << operationAttempts
         << ",\"lease_expires_at\":" << leaseExpiresAt << '}';
    return json.str();
}

PlayerbotRendezvousManager::PartyActivityOwner
PlayerbotRendezvousManager::GetPartyActivityOwner(uint32 botGuid) const
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    if (bot)
    {
        if (!bot->IsAlive()) return PartyActivityOwner::death_recovery;
        if (bot->IsInCombat()) return PartyActivityOwner::combat;
        if (bot->IsTaxiFlying() || bot->GetTransport()) return PartyActivityOwner::transport;
    }
    auto party = partySessions.find(botGuid);
    if (party != partySessions.end())
    {
        const std::string& state = party->second.state;
        // A fresh or recovered human-party arrival is the highest non-safety
        // movement commitment. It preempts stale autonomous errands and guild
        // assembly instead of losing a tug-of-war to their older routes.
        if (state == "pending" || state == "instance_handoff" || state == "relocating" ||
            state == "approaching" || state == "handoff" || state == "departing" ||
            state == "returning" || state == "dungeon_return")
            return PartyActivityOwner::rendezvous;
        if (state == "hearth_sync") return PartyActivityOwner::transport;
    }
    auto lease = externalLeases.find(botGuid);
    if (lease != externalLeases.end() && lease->second.expires > std::chrono::steady_clock::now() &&
        lease->second.owner == PartyActivityOwner::player_command)
        return lease->second.owner;
    auto rendezvous = sessions.find(botGuid);
    bool rendezvousActive = rendezvous != sessions.end() &&
        (rendezvous->second.state == "pending_relocation" || rendezvous->second.state == "relocating" ||
         rendezvous->second.state == "approaching" ||
         rendezvous->second.state == "arrived" || rendezvous->second.state == "assembled" ||
         rendezvous->second.state == "departing");
    bool guildRendezvous = rendezvousActive &&
        rendezvous->second.actionId.find("guild-event:") == 0;
    if (rendezvousActive && !guildRendezvous) return PartyActivityOwner::rendezvous;
    if (party == partySessions.end())
        return guildRendezvous || IsGuildEventAssemblyOrganizer(botGuid) || sGuildEventExecutor.OwnsMovement(botGuid) ?
            PartyActivityOwner::guild_event :
            (lease != externalLeases.end() && lease->second.expires > std::chrono::steady_clock::now() ?
                lease->second.owner : PartyActivityOwner::none);
    const std::string& state = party->second.state;
    if (state == "free_time") return PartyActivityOwner::party_errand;
    if (state == "active") return PartyActivityOwner::party_follow;
    if (guildRendezvous) return PartyActivityOwner::guild_event;
    if (IsGuildEventAssemblyOrganizer(botGuid)) return PartyActivityOwner::guild_event;
    return PartyActivityOwner::none;
}

PlayerbotRendezvousManager::PartyActivityPhase
PlayerbotRendezvousManager::GetPartyActivityPhase(uint32 botGuid) const
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    if (bot && (!bot->IsAlive() || bot->IsInCombat())) return PartyActivityPhase::blocked;
    PartyActivityOwner owner = GetPartyActivityOwner(botGuid);
    auto party = partySessions.find(botGuid);
    if (party != partySessions.end() && owner == PartyActivityOwner::rendezvous)
    {
        const std::string& state = party->second.state;
        if (state == "pending" || state == "instance_handoff" || state == "handoff")
            return PartyActivityPhase::preparing;
        if (state == "relocating" || state == "approaching") return PartyActivityPhase::traveling;
        if (state == "departing") return PartyActivityPhase::departing;
        if (state == "returning" || state == "dungeon_return")
            return PartyActivityPhase::returning;
    }
    if (party != partySessions.end() && owner == PartyActivityOwner::transport)
        return PartyActivityPhase::traveling;
    auto lease = externalLeases.find(botGuid);
    if (lease != externalLeases.end() && lease->second.expires > std::chrono::steady_clock::now() &&
        owner == lease->second.owner)
        return lease->second.phase;
    auto rendezvous = sessions.find(botGuid);
    if (rendezvous != sessions.end() &&
        (owner == PartyActivityOwner::rendezvous || owner == PartyActivityOwner::guild_event))
    {
        const std::string& state = rendezvous->second.state;
        if (state == "pending_relocation" || state == "relocating" || state == "approaching")
            return PartyActivityPhase::traveling;
        if (state == "arrived" || state == "assembled") return PartyActivityPhase::performing;
        if (state == "departing") return PartyActivityPhase::returning;
    }
    auto found = partySessions.find(botGuid);
    if (found == partySessions.end())
        return (IsGuildEventAssemblyParticipant(botGuid) || IsGuildEventAssemblyOrganizer(botGuid)) ?
            PartyActivityPhase::traveling : PartyActivityPhase::idle;
    const PartySession& session = found->second;
    if (session.state == "pending" || session.state == "instance_handoff" ||
        session.state == "handoff") return PartyActivityPhase::preparing;
    if (session.state == "relocating" || session.state == "approaching")
        return PartyActivityPhase::traveling;
    if (session.state == "departing") return PartyActivityPhase::departing;
    if (session.state == "returning" || session.state == "dungeon_return")
        return PartyActivityPhase::returning;
    if (session.state == "active") return PartyActivityPhase::idle;
    if (session.state == "hearth_sync") return PartyActivityPhase::traveling;
    if (session.state == "free_time")
    {
        auto task = session.errands.find(session.currentErrand);
        if (task != session.errands.end()) return task->second.phase;
        return PartyActivityPhase::preparing;
    }
    return PartyActivityPhase::idle;
}

bool PlayerbotRendezvousManager::OwnsPartyMovement(uint32 botGuid) const
{
    if (!sPlayerbotAIConfig.chatDirectorPartyActivityOwnership) return false;
    PartyActivityOwner owner = GetPartyActivityOwner(botGuid);
    return owner == PartyActivityOwner::party_follow || owner == PartyActivityOwner::rendezvous ||
        owner == PartyActivityOwner::party_errand ||
        owner == PartyActivityOwner::guild_event || owner == PartyActivityOwner::guild_supply ||
        owner == PartyActivityOwner::economy_service || owner == PartyActivityOwner::player_command;
}

bool PlayerbotRendezvousManager::BlocksAutonomousPartyWork(uint32 botGuid) const
{
    return OwnsPartyMovement(botGuid);
}

bool PlayerbotRendezvousManager::ClaimRelocationSlot()
{
    if (!relocationAvailableThisUpdate)
        return false;
    relocationAvailableThisUpdate = false;
    return true;
}

bool PlayerbotRendezvousManager::CanRelocateUnobserved(Player* bot, Map* destinationMap,
    float destinationX, float destinationY, float destinationZ) const
{
    return bot && destinationMap &&
        IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) &&
        IsPointUnobservedOnMap(destinationMap, bot, destinationX, destinationY, destinationZ);
}

bool PlayerbotRendezvousManager::FindSafeStagingPoint(Player* bot, Player* player,
    float& x, float& y, float& z) const
{
    return FindStagingPoint(bot, player, x, y, z);
}

bool PlayerbotRendezvousManager::AllowsOwnedMovement(uint32 botGuid, const std::string& actionName)
{
    if (GetPartyActivityOwner(botGuid) == PartyActivityOwner::economy_service)
        return sPlayerbotOrganicEconomy.AllowsServiceAction(botGuid, actionName);
    if (sGuildEventExecutor.OwnsMovement(botGuid) &&
        GetPartyActivityOwner(botGuid) == PartyActivityOwner::guild_event)
        return sGuildEventExecutor.AllowsMovement(botGuid,actionName);
    if (!sPlayerbotAIConfig.chatDirectorPartyActivityOwnership || !OwnsPartyMovement(botGuid))
        return true;
    PartyActivityOwner owner = GetPartyActivityOwner(botGuid);
    if (actionName == "move to loot" && owner == PartyActivityOwner::party_errand)
        return false;
    if (actionName == "follow" && owner == PartyActivityOwner::party_follow)
        return true;
    if (actionName == "move to travel target" &&
        (owner == PartyActivityOwner::party_errand || owner == PartyActivityOwner::player_command))
        return true;
    // Combat, reaction, death recovery, looting, roll decisions, and local
    // interactions retain their own actions. Suppress only route-changing
    // autonomous movement that can replace the owner's movement generator.
    bool conflict = actionName == "follow" || actionName == "move to rpg target" ||
        actionName == "move to travel target" || actionName == "travel" ||
        actionName == "move random" || actionName == "progression move random" ||
        actionName == "go" || actionName == "follow chat shortcut";
    if (!conflict) return true;
    const auto now = std::chrono::steady_clock::now();
    const std::string key = std::to_string(botGuid) + ':' + actionName;
    auto found = movementConflictCooldowns.find(key);
    if (found == movementConflictCooldowns.end() || now >= found->second)
    {
        movementConflictCooldowns[key] = now + std::chrono::seconds(10);
        QueueActivityTelemetry(botGuid, 0, 0, owner, GetPartyActivityPhase(botGuid),
            "conflict_prevented", actionName);
    }
    return false;
}

LivingActivity::Acquisition PlayerbotRendezvousManager::AcquirePartyActivityLease(uint32 botGuid, PartyActivityOwner owner,
    PartyActivityPhase phase, uint32 ttlSeconds, const std::string& reason,
    const std::string& jobKey, LivingActivity::ActivityLease& handle)
{
    using LivingActivity::AcquisitionState;
    sLivingActivityCoordinator.ObserveLeaseBoundary(botGuid, LivingActivityCoordinator::LeaseBoundary::Acquire);
    if (!sPlayerbotAIConfig.chatDirectorPartyActivityOwnership)
        return {AcquisitionState::LegacyAllowed, "legacy_ownership_disabled"};
    LivingActivity::ActivityLease identity;
    if (!sLivingActivityCoordinator.CompatibilityContext(botGuid, PartyActivityOwnerName(owner),
        jobKey, identity)) return {AcquisitionState::Invalidated, "native_context_unavailable"};
    auto now = std::chrono::steady_clock::now();
    auto found = externalLeases.find(botGuid);
    if (found != externalLeases.end() && found->second.expires > now &&
        (found->second.owner != owner ||
         !LivingActivity::MayAcquireCompatibilityLease(found->second.handle, handle, identity, true)))
    {
        QueueActivityTelemetry(botGuid, 0, 0, found->second.owner, found->second.phase,
            "conflict_prevented", reason);
        return {AcquisitionState::Waiting, "another_committed_activity"};
    }
    // Category equality is not job identity. Even the same subsystem cannot
    // borrow another accepted job's lease or renew it with a delayed callback.
    auto party = partySessions.find(botGuid);
    if (owner == PartyActivityOwner::player_command && party != partySessions.end() &&
        party->second.state == "free_time")
    {
        PartySession& session = party->second;
        if (session.currentErrand)
        {
            session.deferredErrandMask |= session.currentErrand;
            PartySettlementErrand& record = session.errands[session.currentErrand];
            record.phase = PartyActivityPhase::deferred;
            record.outcomeCode = "player_command_preempted";
            QueueActivityTelemetry(botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::party_errand, PartyActivityPhase::deferred,
                "task_deferred", "player_command_preempted", session.currentErrand,
                &session.errandBefore, nullptr);
        }
        session.currentErrand = 0;
        session.currentErrandCapability = 0;
        session.currentErrandOutput = 0;
        session.currentErrandOutputCountBefore = 0;
        session.currentErrandLocal = false;
        session.currentErrandId.clear();
        session.automaticErrandMask = 0;
        session.deferredErrandMask |= session.automaticErrandScopeMask &
            ~session.completedErrandMask;
        session.errandOperationAccepted = false;
        session.errandRelocationPending = false;
        session.state = "active";
        session.reason = "player_command_preempted_errand";
        PersistPartySession(session);
    }
    PartyActivityOwner current = GetPartyActivityOwner(botGuid);
    if (current != PartyActivityOwner::none && current != PartyActivityOwner::party_follow &&
        current != owner)
    {
        QueueActivityTelemetry(botGuid, 0, 0, current, GetPartyActivityPhase(botGuid),
            "conflict_prevented", reason);
        return {AcquisitionState::Waiting, "higher_priority_activity"};
    }
    if (found != externalLeases.end() && LivingActivity::SameLease(found->second.handle, handle) &&
        found->second.handle.context == identity.context && found->second.expires > now)
        return UpdatePartyActivityLease(handle, phase, ttlSeconds, reason) ?
            LivingActivity::Acquisition{AcquisitionState::Granted, ""} :
            LivingActivity::Acquisition{AcquisitionState::Invalidated, "lease_context_changed"};
    if (externalLeaseGeneration == UINT64_MAX)
        return {AcquisitionState::Invalidated, "lease_generation_exhausted"};
    ExternalLease& lease = externalLeases[botGuid];
    identity.generation = ++externalLeaseGeneration;
    lease.handle = handle = identity;
    lease.owner = owner; lease.phase = phase; lease.reason = reason;
    lease.expires = now + std::chrono::seconds(std::max<uint32>(1, ttlSeconds));
    QueueActivityTelemetry(botGuid, 0, 0, owner, phase, "lease_acquired", reason);
    return {AcquisitionState::Granted, ""};
}

bool PlayerbotRendezvousManager::HasPartyActivityLease(const LivingActivity::ActivityLease& handle) const
{
    const auto found = externalLeases.find(handle.actor);
    return found != externalLeases.end() && LivingActivity::SameLease(found->second.handle, handle) &&
        found->second.expires > std::chrono::steady_clock::now();
}

bool PlayerbotRendezvousManager::UpdatePartyActivityLease(const LivingActivity::ActivityLease& handle,
    PartyActivityPhase phase, uint32 ttlSeconds, const std::string& reason)
{
    const uint32 botGuid = handle.actor;
    sLivingActivityCoordinator.ObserveLeaseBoundary(botGuid, LivingActivityCoordinator::LeaseBoundary::Renew);
    auto found = externalLeases.find(botGuid);
    if (!sPlayerbotAIConfig.chatDirectorPartyActivityOwnership) return true;
    if (!HasPartyActivityLease(handle)) return false;
    LivingActivity::ActivityLease current;
    if (!sLivingActivityCoordinator.CompatibilityContext(botGuid, "lease_check", "native", current) ||
        !(current.context == handle.context)) return false;
    found->second.phase = phase; found->second.reason = reason;
    found->second.expires = std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max<uint32>(1, ttlSeconds));
    QueueActivityTelemetry(botGuid, 0, 0, found->second.owner, phase, "lease_updated", reason);
    return true;
}

void PlayerbotRendezvousManager::ReleasePartyActivityLease(const LivingActivity::ActivityLease& handle,
    PartyActivityPhase terminalPhase, const std::string& reason)
{
    const uint32 botGuid = handle.actor;
    sLivingActivityCoordinator.ObserveLeaseBoundary(botGuid, LivingActivityCoordinator::LeaseBoundary::Release);
    if (!sLivingActivityCoordinator.OnWorldThread()) return;
    auto found = externalLeases.find(botGuid);
    if (found == externalLeases.end() || !LivingActivity::SameLease(found->second.handle, handle)) return;
    const PartyActivityOwner owner = found->second.owner;
    auto party = partySessions.find(botGuid);
    if (owner == PartyActivityOwner::player_command && party != partySessions.end() &&
        party->second.state == "active" &&
        party->second.reason == "player_command_preempted_errand")
    {
        // A command can preempt free-time after the errand executor has
        // removed follow/master state. Any command terminal path, including a
        // failure before its own Action record exists, must re-enter the
        // authoritative party handoff instead of leaving an "active" bot with
        // no movement owner.
        PartySession& session = party->second;
        session.state = "pending";
        session.reason = "player_command_released_to_party";
        session.forceRelocation = true;
        session.approachIssued = false;
        session.nextApproachAttempt = std::chrono::steady_clock::time_point();
        session.stateSince = std::chrono::steady_clock::now();
        PersistPartySession(session);
    }
    QueueActivityTelemetry(botGuid, 0, 0, owner, terminalPhase, "lease_released", reason);
    externalLeases.erase(found);
}

bool PlayerbotRendezvousManager::RegisterPartyAssist(Player* bot, Player* inviter, bool recovered)
{
    if (!bot || !inviter || !bot->GetPlayerbotAI() || !bot->IsInWorld() || !inviter->IsInWorld() ||
        !inviter->isRealPlayer() || bot->GetTeam() != inviter->GetTeam() || !bot->GetGroup() ||
        bot->GetGroup() != inviter->GetGroup() || bot->InBattleGround() || inviter->InBattleGround() ||
        !bot->GetMap() || !inviter->GetMap())
        return false;

    // Existing mixed-party membership survives a realm restart, including in
    // an instance. Reconstruct same-instance ownership without ever using the
    // outdoor relocation path; mismatched instance/map state remains unsafe.
    bool botInDungeon = bot->GetMap()->IsDungeon();
    bool inviterInDungeon = inviter->GetMap()->IsDungeon();
    bool sameInstance = bot->GetMapId() == inviter->GetMapId() &&
        bot->GetInstanceId() == inviter->GetInstanceId();
    bool sameDungeonInstance = botInDungeon && inviterInDungeon && sameInstance;
    uint64 lifecycleRevision = GetPartySessionRevision(bot);
    auto restricted = freshRestrictedPartyRevisions.find(bot->GetGUIDLow());
    if (restricted != freshRestrictedPartyRevisions.end() &&
        restricted->second != lifecycleRevision)
    {
        freshRestrictedPartyRevisions.erase(restricted);
        restricted = freshRestrictedPartyRevisions.end();
    }
    if (!PartyInstanceBoundarySafe(bot, inviter))
    {
        if (restricted == freshRestrictedPartyRevisions.end())
        {
            freshRestrictedPartyRevisions[bot->GetGUIDLow()] = lifecycleRevision;
            QueueActivityTelemetry(bot->GetGUIDLow(), inviter->GetGUIDLow(),
                bot->GetGroup()->GetId(), PartyActivityOwner::rendezvous,
                PartyActivityPhase::blocked, "party_recovery_blocked",
                recovered ? "recovered_cross_instance_mismatch" :
                    "fresh_cross_instance_mismatch");
        }
        return false;
    }

    // A newly formed human party supersedes autonomous guild assembly. Do not
    // restore the old guild route here: the human invitation is now the
    // authoritative commitment and Update will install its movement safely.
    auto oldRendezvous = sessions.find(bot->GetGUIDLow());
    if (oldRendezvous != sessions.end() && oldRendezvous->second.actionId.find("guild-event:") == 0)
    {
        QueueActivityTelemetry(bot->GetGUIDLow(), oldRendezvous->second.playerGuid, 0,
            PartyActivityOwner::guild_event, PartyActivityPhase::deferred,
            "lease_preempted", "human_party_joined");
        bot->GetPlayerbotAI()->ChangeStrategy(
            "-stay,+follow,-wander", BotState::BOT_STATE_NON_COMBAT);
        sessions.erase(oldRendezvous);
    }

    // A bot can temporarily serve only one human-created party. Existing
    // autonomous bot groups are never registered here and are therefore never
    // dismantled by the return lifecycle.
    bool replacedPartySession = false;
    auto existingParty = partySessions.find(bot->GetGUIDLow());
    if (existingParty != partySessions.end())
    {
        uint32 currentRosterSignature = GetPartyRosterSignature(bot);
        if (existingParty->second.groupId == bot->GetGroup()->GetId() &&
            existingParty->second.partyRosterSignature == currentRosterSignature &&
            IsLivePartyState(existingParty->second.state))
            return true;
        QueueActivityTelemetry(bot->GetGUIDLow(), existingParty->second.playerGuid,
            existingParty->second.groupId, PartyActivityOwner::rendezvous,
            PartyActivityPhase::failed, "party_session_replaced", "roster_changed");
        auto oldLease = externalLeases.find(bot->GetGUIDLow());
        if (oldLease != externalLeases.end())
        {
            QueueActivityTelemetry(bot->GetGUIDLow(), existingParty->second.playerGuid,
                existingParty->second.groupId, oldLease->second.owner,
                PartyActivityPhase::deferred, "lease_released", "party_rejoined");
            externalLeases.erase(oldLease);
        }
        partySessions.erase(existingParty);
        replacedPartySession = true;
    }

    PartySession session;
    session.botGuid = bot->GetGUIDLow();
    session.playerGuid = inviter->GetGUIDLow();
    session.groupId = bot->GetGroup()->GetId();
    session.partyRosterSignature = GetPartyRosterSignature(bot);
    session.partySessionRevision = lifecycleRevision;
    session.partySessionId = GetPartySessionId(bot);
    bool restored = recovered && !replacedPartySession &&
        RestorePersistedPartySession(bot, inviter, session);
    if (!restored)
    {
        session.originMapId = bot->GetMapId();
        session.originInstanceId = bot->GetInstanceId();
        session.originX = bot->GetPositionX();
        session.originY = bot->GetPositionY();
        session.originZ = bot->GetPositionZ();
        session.originO = bot->GetOrientation();
        session.previousActivity = ActivityToken(
            bot->GetPlayerbotAI()->HandleRemoteCommand("action"));
    }
    session.state = sameDungeonInstance ? "instance_handoff" : "pending";
    session.stateSince = std::chrono::steady_clock::now();
    partySessions[session.botGuid] = session;
    PersistPartySession(partySessions[session.botGuid]);
    LogPartyEvent(partySessions[session.botGuid], "registered");
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::rendezvous, PartyActivityPhase::preparing,
        restored ? "party_session_reconstructed" : "party_session_registered",
        restored ? session.reason : (sameDungeonInstance ?
            "same_instance_party_session" : "fresh_party_session"));
    if (restored)
        for (const auto& task : partySessions[session.botGuid].errands)
            if (task.second.outcomeCode == "realm_restart")
                QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                    PartyActivityOwner::party_errand, PartyActivityPhase::deferred,
                    "task_deferred", "realm_restart", task.first);
    return true;
}

bool PlayerbotRendezvousManager::ResumePartyAssist(Player* bot, Player* player, const std::string& reason)
{
    if (!bot || !player || !bot->GetGroup() || bot->GetGroup() != player->GetGroup() ||
        !bot->IsInWorld() || !player->IsInWorld() || !bot->IsAlive() || !player->IsAlive() ||
        bot->IsTaxiFlying() || bot->GetTransport() ||
        bot->InBattleGround() || bot->GetMap()->IsDungeon() || player->GetMap()->IsDungeon())
        return false;

    auto found = partySessions.find(bot->GetGUIDLow());
    if (found == partySessions.end())
    {
        if (!RegisterPartyAssist(bot, player))
            return false;
        found = partySessions.find(bot->GetGUIDLow());
    }
    PartySession& session = found->second;
    if (session.groupId != bot->GetGroup()->GetId())
        return false;

    if (bot->IsInCombat())
    {
        if (session.state != "free_time")
            return false;
        session.freeTimeRecallRequested = true;
        session.reason = "free_time_recall_after_combat";
        PersistPartySession(session);
        LogPartyEvent(session, "free_time_recall_queued");
        return true;
    }

    if (session.state == "free_time")
    {
        if (session.currentErrand)
        {
            session.deferredErrandMask |= session.currentErrand;
            PartySettlementErrand& record = session.errands[session.currentErrand];
            record.phase = PartyActivityPhase::deferred;
            record.outcomeCode = "party_return_preempted";
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::party_errand, PartyActivityPhase::deferred,
                "task_deferred", "party_return_preempted", session.currentErrand,
                &session.errandBefore, nullptr);
        }
        session.currentErrand = 0;
        session.currentErrandCapability = 0;
        session.currentErrandOutput = 0;
        session.currentErrandOutputCountBefore = 0;
        session.currentErrandLocal = false;
        session.currentErrandId.clear();
        session.automaticErrandMask = 0;
        session.errandOperationAccepted = false;
        session.errandRelocationPending = false;
        ClearMovementState(bot, player, true);
    }

    session.playerGuid = player->GetGUIDLow();
    session.state = "pending";
    session.reason = reason;
    session.forceRelocation = true;
    session.approachIssued = false;
    session.freeTimeRecallRequested = false;
    session.freeTimeUntil = std::chrono::steady_clock::time_point();
    session.nextApproachAttempt = std::chrono::steady_clock::time_point();
    session.stateSince = std::chrono::steady_clock::now();
    PersistPartySession(session);
    LogPartyEvent(session, "party_return_queued");
    return true;
}

bool PlayerbotRendezvousManager::BeginPartyFreeTime(Player* bot, Player* player, const std::string& reason)
{
    if (!bot || !player || !bot->GetPlayerbotAI() || !bot->GetGroup() ||
        bot->GetGroup() != player->GetGroup() || !bot->GetGroup()->IsLeader(player->GetObjectGuid()) ||
        !bot->IsInWorld() || !player->IsInWorld() || !bot->IsAlive() || !player->IsAlive() ||
        bot->IsInCombat() || player->IsInCombat() || bot->IsTaxiFlying() || bot->GetTransport() ||
        bot->InBattleGround() || bot->GetMap()->IsDungeon() || player->GetMap()->IsDungeon() ||
        bot->GetMapId() != player->GetMapId() || !bot->IsWithinDistInMap(player, 120.0f))
        return false;
    PartyActivityOwner currentOwner = GetPartyActivityOwner(bot->GetGUIDLow());
    if (sPlayerbotAIConfig.chatDirectorPartyActivityOwnership &&
        currentOwner != PartyActivityOwner::none && currentOwner != PartyActivityOwner::party_follow)
    {
        QueueActivityTelemetry(bot->GetGUIDLow(), player->GetGUIDLow(), bot->GetGroup()->GetId(),
            currentOwner, GetPartyActivityPhase(bot->GetGUIDLow()),
            "conflict_prevented", "party_errand");
        return false;
    }

    auto found = partySessions.find(bot->GetGUIDLow());
    if (found == partySessions.end())
    {
        if (!RegisterPartyAssist(bot, player))
            return false;
        found = partySessions.find(bot->GetGUIDLow());
    }
    PartySession& session = found->second;
    if (session.groupId != bot->GetGroup()->GetId())
        return false;
    if (session.state == "free_time")
        return true;

    PlayerbotAI* ai = bot->GetPlayerbotAI();
    ClearMovementState(bot, nullptr, false);

    const auto now = std::chrono::steady_clock::now();
    session.playerGuid = player->GetGUIDLow();
    session.state = "free_time";
    session.reason = reason;
    session.freeTimeRecallRequested = false;
    session.freeTimePlayerZoneId = player->GetZoneId();
    session.freeTimePlayerAreaId = sServerFacade.GetAreaId(player);
    bool automaticSettlement = reason == "automatic_settlement_errands";
    bool verifiedBundle = sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands;
    session.freeTimeUntil = verifiedBundle ? std::chrono::steady_clock::time_point() :
        now + (automaticSettlement ? std::chrono::minutes(5) : std::chrono::minutes(30));
    session.automaticErrandScopeMask = verifiedBundle ? GroundedSettlementErrandMask(bot) :
        (automaticSettlement ? GroundedSettlementErrandMask(bot) : 0);
    session.automaticErrandMask = session.automaticErrandScopeMask;
    session.completedErrandMask = 0;
    session.deferredErrandMask = 0;
    session.currentErrand = 0;
    session.currentErrandCapability = 0;
    session.currentErrandOutput = 0;
    session.currentErrandOutputCountBefore = 0;
    session.currentErrandLocal = false;
    session.currentErrandId.clear();
    session.errands.clear();
    if (!errandSequence) errandSequence = uint64(time(nullptr)) * 1000000ULL;
    const uint32 bundleTasks[] = {kErrandVendor, kErrandRepair, kErrandBank,
        kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
    for (uint32 task : bundleTasks)
        if (session.automaticErrandScopeMask & task)
        {
            PartySettlementErrand record;
            record.type = task;
            record.taskId = "party-errand:" + std::to_string(session.botGuid) + ':' +
                std::to_string(++errandSequence);
            session.errands[task] = record;
        }
    session.errandRouteAttempts = 0;
    session.errandOperationAttempts = 0;
    session.errandOperationAccepted = false;
    session.errandRelocationPending = false;
    session.errandFallbackUsed = false;
    session.errandSummarySent = false;
    session.errandSummaryReadyAt = std::chrono::steady_clock::time_point();
    session.errandLastDistance = -1.0f;
    session.automaticErrandLastX = bot->GetPositionX();
    session.automaticErrandLastY = bot->GetPositionY();
    session.automaticErrandHardDeadline = verifiedBundle ?
        std::chrono::steady_clock::time_point() :
        (automaticSettlement ? now + std::chrono::minutes(15) : std::chrono::steady_clock::time_point());
    session.automaticErrandActiveDeadline = std::chrono::steady_clock::time_point();
    session.errandWorldportSince = std::chrono::steady_clock::time_point();
    session.currentErrandDeadline = std::chrono::steady_clock::time_point();
    session.currentErrandNoProgressDeadline = std::chrono::steady_clock::time_point();
    session.errandBlockedSince = std::chrono::steady_clock::time_point();
    session.nextErrandStep = now;
    session.nextAutomaticErrandCheck = automaticSettlement ?
        now + std::chrono::seconds(45) : std::chrono::steady_clock::time_point();
    session.nextAutomaticErrandProgressLog = automaticSettlement ?
        now + std::chrono::minutes(1) : std::chrono::steady_clock::time_point();
    session.stateSince = now;
    LogPartyEvent(session, "free_time_started");
    if (verifiedBundle)
    {
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::party_errand, PartyActivityPhase::preparing,
            "errand_bundle_started", reason);
        if (!StartNextVerifiedErrand(session, bot))
            session.freeTimeRecallRequested = true;
    }
    else if (automaticSettlement)
    {
        SetAutomaticErrandTarget(bot, session.automaticErrandMask);
        LogAutomaticErrandEvent(session, bot, "started", session.automaticErrandMask,
            session.automaticErrandMask);
    }
    PersistPartySession(session);
    return true;
}

bool PlayerbotRendezvousManager::IsPartyFreeTime(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    return found != partySessions.end() && found->second.state == "free_time";
}

bool PlayerbotRendezvousManager::HasVerifiedErrandRoute(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    return sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands && found != partySessions.end() &&
        found->second.state == "free_time" && found->second.currentErrand &&
        !found->second.currentErrandLocal && !found->second.freeTimeRecallRequested;
}

bool PlayerbotRendezvousManager::FindClassTrainingDestination(Player* bot,
    ai::TravelDestination*& destination, ai::WorldPosition*& position) const
{
    return FindSettlementErrandDestination(bot, kErrandTraining, destination, position);
}

bool PlayerbotRendezvousManager::YieldPartyFollowToLoot(Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI()) return false;
    auto found = partySessions.find(bot->GetGUIDLow());
    if (found == partySessions.end() || found->second.state != "active") return false;
    PartySession& session = found->second;
    AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
    LootObject loot = context->GetValue<LootObject>("loot target")->Get();
    Player* human = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, session.playerGuid));
    WorldObject* object = loot.GetWorldObject(bot);
    bool safe = human && human->IsInWorld() && human->IsAlive() && bot->IsAlive() &&
        !human->IsInCombat() && !bot->IsInCombat() && !bot->IsTaxiFlying() && !bot->GetTransport() &&
        human->GetMapId() == bot->GetMapId() && human->GetInstanceId() == bot->GetInstanceId() &&
        bot->IsWithinDistInMap(human, 60.0f) && object && human->IsWithinDistInMap(object, 60.0f) &&
        loot.IsLootPossible(bot);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (safe && session.lootWindow.Observe(loot.guid.GetRawValue(), bot->GetDistance(object), now))
        return true;
    session.lootWindow.Observe(0, 0, now);
    if (!loot.IsEmpty() && !bot->IsInCombat())
    {
        context->GetValue<LootObjectStack*>("available loot")->Get()->Remove(loot.guid);
        context->GetValue<LootObject>("loot target")->Set(LootObject());
        context->ClearValues("has available loot");
    }
    return false;
}

std::string PlayerbotRendezvousManager::PartyState(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    return found == partySessions.end() ? "none" : found->second.state;
}

std::string PlayerbotRendezvousManager::PartyReason(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    return found == partySessions.end() ? "" : found->second.reason;
}

uint32 PlayerbotRendezvousManager::PartyDeadRecoveryAttempts(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    return found == partySessions.end() ? 0 : found->second.deadRecoveryAttempts;
}

uint32 PlayerbotRendezvousManager::PartyDeadRecoverySeconds(uint32 botGuid) const
{
    auto found = partySessions.find(botGuid);
    if (found == partySessions.end() || found->second.deadRecoveryStarted.time_since_epoch().count() == 0)
        return 0;
    return (uint32)std::max<long long>(0, std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - found->second.deadRecoveryStarted).count());
}

PlayerbotRendezvousManager::Session* PlayerbotRendezvousManager::Find(uint32 botGuid, uint32 playerGuid)
{
    auto found = sessions.find(botGuid);
    return found != sessions.end() && found->second.playerGuid == playerGuid ? &found->second : nullptr;
}

const PlayerbotRendezvousManager::Session* PlayerbotRendezvousManager::Find(uint32 botGuid, uint32 playerGuid) const
{
    auto found = sessions.find(botGuid);
    return found != sessions.end() && found->second.playerGuid == playerGuid ? &found->second : nullptr;
}

bool PlayerbotRendezvousManager::IsPointUnobserved(Player* bot, float x, float y, float z) const
{
    return bot && IsPointUnobservedOnMap(bot->GetMap(), bot, x, y, z);
}

bool PlayerbotRendezvousManager::IsPointUnobservedOnMap(Map* map, Player* bot, float x, float y, float z) const
{
    if (!map || !bot) return false;
    float visibility = map->GetVisibilityDistance();
    for (const auto& reference : map->GetPlayers())
    {
        Player* observer = reference.getSource();
        if (!IsRealObserver(observer)) continue;
        if (observer->IsWithinDist3d(x, y, z, visibility) && observer->IsWithinLOS(x, y, z + bot->GetCollisionHeight(), false))
            return false;
    }
    return true;
}

bool PlayerbotRendezvousManager::ValidPath(Player* bot, float sx, float sy, float sz, Player* player) const
{
    if (!bot || !player || !player->GetMap()) return false;
    PathFinder path(player->GetMapId(), player->GetInstanceId());
    if (!path.calculate(Vector3(sx, sy, sz), Vector3(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ()), true))
        return false;
    PathType type = path.getPathType();
    return !(type & PATHFIND_NOPATH) && !(type & PATHFIND_SHORTCUT) && !(type & PATHFIND_INCOMPLETE);
}

bool PlayerbotRendezvousManager::FindStagingPoint(Player* bot, Player* player, float& x, float& y, float& z) const
{
    if (!bot || !player || !player->GetMap()) return false;
    uint32 targetSeconds = std::max<uint32>(5, std::min<uint32>(30, sPlayerbotAIConfig.chatDirectorRendezvousTargetSeconds));
    uint32 maximumSeconds = std::max<uint32>(targetSeconds,
        std::min<uint32>(60, sPlayerbotAIConfig.chatDirectorRendezvousMaximumSeconds));
    const float rings[] = { kRunSpeedYardsPerSecond * targetSeconds,
        kRunSpeedYardsPerSecond * ((targetSeconds + maximumSeconds) / 2.0f),
        kRunSpeedYardsPerSecond * maximumSeconds };
    for (float radius : rings)
    {
        for (uint32 step = 0; step < 16; ++step)
        {
            float angle = float(step) * float(M_PI) / 8.0f;
            float cx = player->GetPositionX() + std::cos(angle) * radius;
            float cy = player->GetPositionY() + std::sin(angle) * radius;
            float cz = player->GetMap()->GetHeight(cx, cy, player->GetPositionZ() + 25.0f);
            if (cz < -100000.0f || !IsPointUnobservedOnMap(player->GetMap(), bot, cx, cy, cz) ||
                !ValidPath(bot, cx, cy, cz, player))
                continue;
            x = cx; y = cy; z = cz + 0.1f;
            return true;
        }
    }
    return false;
}

bool PlayerbotRendezvousManager::FindPartyRecoveryPoint(Player* bot, Player* player,
    float& x, float& y, float& z) const
{
    if (!bot || !player || !player->GetMap() || bot->GetMapId() != player->GetMapId() ||
        bot->GetInstanceId() != player->GetInstanceId())
        return false;

    // A hidden staging point can occasionally pass the preflight path check
    // but still fail when MoveFollow builds its live path (dynamic obstacles,
    // steep terrain, or a moving target).  This bounded recovery point is
    // intentionally close enough to finish the party handoff immediately.
    // It is used only after the configured maximum arrival time has elapsed.
    const float radii[] = { 8.0f, 10.0f, 12.0f };
    const uint32 firstStep = bot->GetGUIDLow() % 16;
    for (float radius : radii)
    {
        for (uint32 offset = 0; offset < 16; ++offset)
        {
            const uint32 step = (firstStep + offset) % 16;
            const float angle = float(step) * float(M_PI) / 8.0f;
            float cx = player->GetPositionX();
            float cy = player->GetPositionY();
            float cz = player->GetPositionZ();
            player->GetNearPoint(bot, cx, cy, cz, bot->GetObjectBoundingRadius(), radius, angle);
            const float ground = player->GetMap()->GetHeight(cx, cy, player->GetPositionZ() + 10.0f);
            if (ground < -100000.0f) continue;
            cz = ground + 0.1f;
            if (!player->IsWithinLOS(cx, cy, cz + bot->GetCollisionHeight(), true) ||
                !ValidPath(bot, cx, cy, cz, player))
                continue;
            x = cx;
            y = cy;
            z = cz;
            return true;
        }
    }
    return false;
}

PlayerbotRendezvousManager::RequestResult PlayerbotRendezvousManager::Request(
    Player* bot, Player* player, const std::string& actionId, bool returnAfter)
{
    const bool guildEvent = actionId.find("guild-event:") == 0;
    auto finishRequest = [&](RequestResult result, const char* reason)
    {
        if (guildEvent)
        {
            const char* outcome = result == RequestResult::accepted ? "accepted" :
                result == RequestResult::ordinary_travel ? "ordinary_travel" :
                result == RequestResult::unsafe ? "unsafe" : "unavailable";
            sLog.outString(
                "Living WoW guild rendezvous request action=%s bot=%u organizer=%u outcome=%s reason=%s",
                actionId.c_str(), bot ? bot->GetGUIDLow() : 0,
                player ? player->GetGUIDLow() : 0, outcome, reason);
        }
        return result;
    };
    if (!bot || !player || !bot->IsInWorld() || !player->IsInWorld() ||
        bot->IsInCombat() || !bot->IsAlive() || !player->IsAlive() || bot->IsTaxiFlying())
        return finishRequest(RequestResult::unavailable, "participant_unavailable");
    if (guildEvent && !sGuildEventExecutor.CanRendezvous(
        bot->GetGUIDLow(), player->GetGUIDLow(), actionId.substr(12)))
    {
        QueueActivityTelemetry(bot->GetGUIDLow(), player->GetGUIDLow(), 0,
            GetPartyActivityOwner(bot->GetGUIDLow()), GetPartyActivityPhase(bot->GetGUIDLow()),
            "conflict_prevented", "guild_event");
        return finishRequest(RequestResult::unavailable, "guild_assembly_not_authorized");
    }
    auto existing = sessions.find(bot->GetGUIDLow());
    if (existing != sessions.end())
    {
        if (existing->second.playerGuid == player->GetGUIDLow())
        {
            // ApplyGuildPlans starts the rendezvous with its candidate ID and
            // the lifecycle subsequently knows the persisted event ID. Adopt
            // that authoritative ID without replacing the active session.
            if (guildEvent && existing->second.actionId.find("guild-event:") == 0)
                existing->second.actionId = actionId;
            return RequestResult::accepted;
        }
        return finishRequest(RequestResult::unavailable, "different_active_session");
    }

    const bool sameMap = bot->GetMapId() == player->GetMapId() &&
        bot->GetInstanceId() == player->GetInstanceId();
    // Cross-map rendezvous is reserved for one-way, world-authoritative
    // handoffs such as assembling a bot guild event. Transactions that must
    // return to the original activity keep their existing same-map contract.
    if (!sameMap && (returnAfter || bot->InBattleGround() || player->InBattleGround() ||
        bot->GetMap()->IsDungeon() || player->GetMap()->IsDungeon()))
        return finishRequest(RequestResult::unavailable, "restricted_cross_map");
    if (sameMap && bot->GetTransport())
        return finishRequest(RequestResult::unavailable, "transport_active");

    const auto now = std::chrono::steady_clock::now();
    bool queuedRelocation = false;
    Session session;
    session.botGuid = bot->GetGUIDLow();
    session.playerGuid = player->GetGUIDLow();
    session.mapId = bot->GetMapId();
    session.originX = bot->GetPositionX(); session.originY = bot->GetPositionY();
    session.originZ = bot->GetPositionZ(); session.originO = bot->GetOrientation();
    session.actionId = actionId;
    session.previousActivity = bot->GetPlayerbotAI()->HandleRemoteCommand("action");
    session.state = "approaching";
    session.returnAfter = returnAfter;
    session.started = session.stateSince = now;

    float distance = sameMap ? bot->GetDistance(player) : 100000.0f;
    // Guild events have a bounded assembly window. Reusing the ordinary
    // transaction threshold here allowed a member to begin a five-minute run
    // at the same instant the five-minute event timeout started. Use a short
    // ordinary approach for these one-way, server-organized rendezvous calls,
    // then let the existing visibility and safety checks decide whether a
    // catch-up relocation is allowed. Player trades and party errands retain
    // the configured threshold and return-trip behavior.
    uint32 triggerSeconds = guildEvent ? 10 : std::max<uint32>(10, std::min<uint32>(300,
        sPlayerbotAIConfig.chatDirectorRendezvousTriggerSeconds));
    bool needsCatchup = distance > kRunSpeedYardsPerSecond * triggerSeconds;
    if (needsCatchup)
    {
        if (!sPlayerbotAIConfig.chatDirectorRendezvousCatchup)
            return finishRequest(RequestResult::unavailable, "catchup_disabled");
        // Never make either end of the relocation disappear in front of a real
        // observer. Camera orientation is not authoritative server data, so LOS
        // and visibility from every nearby human are the conservative boundary.
        if (!IsPointUnobserved(bot, session.originX, session.originY, session.originZ))
            return finishRequest(RequestResult::unsafe, "origin_observed");
        float stageX = 0.0f, stageY = 0.0f, stageZ = 0.0f;
        if (!FindStagingPoint(bot, player, stageX, stageY, stageZ))
            return finishRequest(RequestResult::unsafe, "no_safe_staging_point");
        // Request can be called several times from one chat/world update.
        // Queue the validated relocation and let Update consume the one shared
        // relocation slot; no invitation, trade, or guild callback teleports
        // synchronously anymore.
        session.pendingMapId = player->GetMapId();
        session.pendingX = stageX;
        session.pendingY = stageY;
        session.pendingZ = stageZ;
        session.pendingO = player->GetOrientation();
        session.state = "pending_relocation";
        queuedRelocation = true;
    }

    sessions[session.botGuid] = session;
    // TeleportTo completes the map transfer asynchronously. Installing a
    // movement generator while m_currMap is detached violates a CMaNGOS map
    // invariant, so cross-map arrivals begin following from Update only after
    // IsBeingTeleported has cleared and both actors share the destination map.
    if (!queuedRelocation)
        bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
    LogEvent(sessions[session.botGuid], queuedRelocation ? "relocation_queued" : "ordinary_arrival");
    return finishRequest(queuedRelocation ? RequestResult::accepted : RequestResult::ordinary_travel,
        queuedRelocation ? "relocation_queued" : "ordinary_approach_started");
}

void PlayerbotRendezvousManager::BeginDeparture(uint32 botGuid, uint32 playerGuid, const std::string& reason)
{
    Session* session = Find(botGuid, playerGuid);
    if (!session) return;
    if (!session->returnAfter)
    {
        LogEvent(*session, "rendezvous_released");
        sessions.erase(botGuid);
        return;
    }
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, playerGuid));
    session->state = "departing";
    session->reason = reason;
    session->stateSince = std::chrono::steady_clock::now();
    if (bot && player && bot->GetMapId() == player->GetMapId())
    {
        float angle = player->GetAngle(bot);
        float x = bot->GetPositionX() + std::cos(angle) * 45.0f;
        float y = bot->GetPositionY() + std::sin(angle) * 45.0f;
        float z = bot->GetMap()->GetHeight(x, y, bot->GetPositionZ() + 10.0f);
        if (z > -100000.0f)
            bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN);
    }
    LogEvent(*session, "departure_started");
}

bool PlayerbotRendezvousManager::ReturnToActivity(Session& session, Player* bot)
{
    if (!bot || !bot->IsInWorld()) return false;
    if (session.relocated && bot->GetMapId() == session.mapId &&
        IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) &&
        IsPointUnobserved(bot, session.originX, session.originY, session.originZ))
    {
        float ground = bot->GetMap()->GetHeight(session.originX, session.originY, session.originZ + 10.0f);
        if (ground > -100000.0f)
        {
            if (!ClaimRelocationSlot())
                return false;
            bot->NearTeleportTo(session.originX, session.originY, ground + 0.1f, session.originO);
        }
    }
    // Re-evaluate the high-level travel/quest target against current world state;
    // the saved activity is diagnostic context, never an unvalidated command.
    bot->GetPlayerbotAI()->DoSpecificAction("reset travel target", Event("living rendezvous resume"), true);
    return true;
}

void PlayerbotRendezvousManager::Cancel(uint32 botGuid, uint32 playerGuid, const std::string& reason)
{
    Session* session = Find(botGuid, playerGuid);
    if (session && session->actionId.find("guild-event:") == 0)
    {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
        if (bot && bot->GetPlayerbotAI())
            bot->GetPlayerbotAI()->ChangeStrategy(
                "-stay,+follow,-wander", BotState::BOT_STATE_NON_COMBAT);
    }
    BeginDeparture(botGuid, playerGuid, reason);
}

bool PlayerbotRendezvousManager::CancelGuildEvent(uint32 botGuid, const std::string& eventId, const std::string& reason)
{
    auto found = sessions.find(botGuid);
    if (found == sessions.end() || found->second.actionId != "guild-event:" + eventId)
        return false;
    // A human-party arrival, newer event or unrelated errand must never be
    // cancelled by a delayed cleanup record from an older guild event.
    Cancel(botGuid, found->second.playerGuid, reason);
    return true;
}

void PlayerbotRendezvousManager::Update()
{
    const auto now = std::chrono::steady_clock::now();
    PrunePersistedPartySessions();
    relocationAvailableThisUpdate = true;
    for (auto lease = externalLeases.begin(); lease != externalLeases.end(); )
    {
        if (lease->second.expires > now) { ++lease; continue; }
        QueueActivityTelemetry(lease->first, 0, 0, lease->second.owner,
            PartyActivityPhase::failed, "lease_expired", "deadline_expired");
        lease = externalLeases.erase(lease);
    }
    if (nextPartyDiscovery.time_since_epoch().count() == 0 || now >= nextPartyDiscovery)
    {
        nextPartyDiscovery = now + std::chrono::seconds(2);
        for (uint32 botGuid : sRandomPlayerbotMgr.GetChatBotGuids())
        {
            if (partySessions.find(botGuid) != partySessions.end())
                continue;
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
            if (!bot || !bot->IsInWorld() || !bot->GetPlayerbotAI() || !bot->GetGroup())
                continue;
            Player* human = FindPartyHuman(bot);
            if (!human)
                continue;

            // Group membership persists across realm restarts, while the
            // rendezvous registry intentionally does not. Reconstruct every
            // mixed party through the typed lifecycle, including a bot-led
            // group with an online real member.
            // UpdatePartyAssists serializes every reconstructed arrival.
            if (RegisterPartyAssist(bot, human, true))
                LogPartyEvent(partySessions[botGuid], "recovered_persisted_party");
        }
    }
    UpdatePartyAssists();
    for (auto iterator = sessions.begin(); iterator != sessions.end(); )
    {
        Session& session = iterator->second;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(session.botGuid);
        Player* player = sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER, session.playerGuid));
        bool erase = false;
        if (!bot) erase = true;
        else if (session.state == "pending_relocation")
        {
            long waitingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                now - session.stateSince).count();
            bool ready = player && player->IsInWorld() && bot->IsInWorld() && bot->IsAlive() &&
                !bot->IsInCombat() && !bot->IsTaxiFlying() && !bot->IsBeingTeleported() &&
                player->GetMapId() == session.pendingMapId &&
                IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) &&
                IsPointUnobservedOnMap(player->GetMap(), bot, session.pendingX, session.pendingY,
                    session.pendingZ);
            if (!player || !player->IsInWorld() || waitingSeconds >= 45)
            {
                session.reason = !player || !player->IsInWorld() ?
                    "relocation_player_unavailable" : "relocation_queue_timeout";
                LogEvent(session, session.reason.c_str());
                erase = true;
            }
            else if (ready && ClaimRelocationSlot())
            {
                GenericTransport* transport = bot->GetTransport();
                bot->GetPlayerbotAI()->StopMoving();
                if (transport)
                    transport->RemovePassenger(bot);
                bool sameMap = bot->GetMapId() == session.pendingMapId &&
                    bot->GetInstanceId() == player->GetInstanceId();
                bool accepted = true;
                if (sameMap)
                    bot->NearTeleportTo(session.pendingX, session.pendingY, session.pendingZ,
                        bot->GetAngle(player));
                else
                    accepted = bot->TeleportTo(session.pendingMapId, session.pendingX,
                        session.pendingY, session.pendingZ, session.pendingO);
                if (!accepted)
                {
                    session.reason = "queued_relocation_rejected";
                    LogEvent(session, "queued_relocation_rejected");
                    erase = true;
                }
                else
                {
                    session.relocated = true;
                    session.state = sameMap ? "approaching" : "relocating";
                    session.stateSince = now;
                    if (sameMap)
                        bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                    LogEvent(session, sameMap ? "queued_relocation_completed" :
                        "queued_cross_map_relocation_started");
                }
            }
        }
        else if (session.state == "relocating")
        {
            if (!player || !player->IsInWorld())
            {
                LogEvent(session, "relocation_player_unavailable");
                erase = true;
            }
            else if (!bot->IsInWorld() || bot->IsBeingTeleported())
            {
                // Player::TeleportTo temporarily detaches a far-teleporting
                // bot from its map until PlayerbotAI handles the worldport ACK.
                // That is expected transit, not a vanished participant. The
                // old top-level !IsInWorld check erased the rendezvous here,
                // so the eventual arrival had no movement owner or hold.
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - session.stateSince).count() >= 45)
                {
                    session.reason = "worldport_ack_timeout";
                    LogEvent(session, "relocation_ack_timeout");
                    erase = true;
                }
            }
            else
            {
                if (bot->GetMapId() != player->GetMapId() ||
                    bot->GetInstanceId() != player->GetInstanceId())
                {
                    LogEvent(session, "relocation_map_mismatch");
                    erase = true;
                }
                else
                {
                    session.state = "approaching";
                    session.stateSince = now;
                    bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                    LogEvent(session, "relocation_attached");
                }
            }
        }
        else if (!bot->IsInWorld()) erase = true;
        else if (session.state == "approaching")
        {
            if (!player || !player->IsInWorld() || player->GetMapId() != bot->GetMapId())
            {
                session.state = "departing"; session.reason = "player_unavailable"; session.stateSince = now;
            }
            else if (bot->IsInCombat())
            {
                if (!session.combatPaused)
                {
                    // Do not synchronously clear the movement generator here.
                    // This update can run immediately after NearTeleportTo;
                    // Playerbots may still hold the active generator for its
                    // next AI tick, and deleting it here causes a use-after-free.
                    // Normal combat AI owns subsequent movement until combat ends.
                    session.combatPaused = true;
                    session.stateSince = now;
                    LogEvent(session, "combat_paused");
                }
            }
            else if (session.combatPaused)
            {
                session.combatPaused = false;
                session.stateSince = now;
                bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                LogEvent(session, "combat_resumed");
            }
            else if (bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                if (!session.returnAfter)
                {
                    if (session.actionId.find("guild-event:") == 0)
                    {
                        // Guild formation is a barrier, not a collection of
                        // independent arrivals. Keep early members beside the
                        // stationary organizer until the lifecycle confirms
                        // the complete online roster; otherwise autonomous AI
                        // can wander away while later members are arriving.
                        session.state = "assembled";
                        session.stateSince = now;
                        bot->GetPlayerbotAI()->ChangeStrategy(
                            "+stay,-follow,-wander", BotState::BOT_STATE_NON_COMBAT);
                        bot->GetPlayerbotAI()->StopMoving();
                        LogEvent(session, "assembly_held");
                    }
                    else
                    {
                        // Other one-way rendezvous operations hand movement
                        // back to their owning workflow immediately.
                        LogEvent(session, "arrival_handed_off");
                        erase = true;
                    }
                }
                else
                {
                    session.state = "arrived";
                    session.stateSince = now;
                    LogEvent(session, "arrived");
                }
            }
            else if (std::chrono::duration_cast<std::chrono::seconds>(now - session.stateSince).count() >= 2)
            {
                bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                session.stateSince = now;
            }
        }
        else if (session.state == "assembled")
        {
            if (!player || !player->IsInWorld() || !player->GetGroup() ||
                bot->GetGroup() != player->GetGroup())
            {
                bot->GetPlayerbotAI()->ChangeStrategy(
                    "-stay,+follow,-wander", BotState::BOT_STATE_NON_COMBAT);
                LogEvent(session, "assembly_cancelled");
                erase = true;
            }
            else if (!bot->IsInCombat() &&
                !bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                session.state = "approaching";
                session.stateSince = now;
                bot->GetPlayerbotAI()->ChangeStrategy(
                    "-stay,+follow,-wander", BotState::BOT_STATE_NON_COMBAT);
                bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                LogEvent(session, "assembly_rejoin");
            }
            else if (!bot->IsInCombat() && sServerFacade.isMoving(bot))
                bot->GetPlayerbotAI()->StopMoving();
        }
        else if (session.state == "arrived")
        {
            if (!player || !player->IsInWorld() || player->GetMapId() != bot->GetMapId())
            {
                session.state = "departing"; session.reason = "player_unavailable"; session.stateSince = now;
            }
            else if (std::chrono::duration_cast<std::chrono::seconds>(
                now - session.stateSince).count() >= std::max<uint32>(15,
                    sPlayerbotAIConfig.chatDirectorPartyReturnWaitSeconds))
            {
                BeginDeparture(session.botGuid, session.playerGuid, "rendezvous_wait_timeout");
            }
            else if (!bot->IsInCombat() && !bot->IsWithinDistInMap(player, INTERACTION_DISTANCE))
            {
                // The trade has not completed merely because the bot reached
                // the player. Recover from incidental movement instead of
                // silently abandoning the still-authorized transaction.
                session.state = "approaching";
                session.stateSince = now;
                bot->GetMotionMaster()->MoveFollow(player, 2.0f, 0.0f, true, false);
                LogEvent(session, "arrival_distance_recovered");
            }
            else if (!bot->IsInCombat())
                bot->GetPlayerbotAI()->StopMoving();
        }
        else if (session.state == "departing")
        {
            long elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session.stateSince).count();
            bool hidden = IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
            if (hidden || elapsed >= std::max<uint32>(15,
                std::min<uint32>(300, sPlayerbotAIConfig.chatDirectorRendezvousDepartureSeconds)))
            {
                if (ReturnToActivity(session, bot))
                {
                    LogEvent(session, hidden ? "activity_restored" : "natural_resume_no_visible_teleport");
                    erase = true;
                }
            }
        }
        if (erase) iterator = sessions.erase(iterator); else ++iterator;
    }
}

bool PlayerbotRendezvousManager::IsGuildEventAssemblyParticipant(uint32 botGuid) const
{
    auto found = sessions.find(botGuid);
    if (found == sessions.end() || found->second.actionId.find("guild-event:") != 0)
        return false;
    const std::string& state = found->second.state;
    return state == "pending_relocation" || state == "relocating" ||
        state == "approaching" || state == "assembled";
}

bool PlayerbotRendezvousManager::IsGuildEventAssemblyOrganizer(uint32 botGuid) const
{
    for (const auto& pair : sessions)
    {
        const Session& session = pair.second;
        if (session.playerGuid != botGuid || session.actionId.find("guild-event:") != 0)
            continue;
        if (session.state == "pending_relocation" || session.state == "relocating" ||
            session.state == "approaching" ||
            session.state == "assembled")
            return true;
    }
    return false;
}

Player* PlayerbotRendezvousManager::FindPartyHuman(Player* bot) const
{
    if (!bot || !bot->GetGroup()) return nullptr;
    Player* leader = sObjectAccessor.FindPlayer(bot->GetGroup()->GetLeaderGuid());
    if (leader && leader->IsInWorld() && leader->isRealPlayer())
        return leader;
    Player* selected = nullptr;
    for (GroupReference* reference = bot->GetGroup()->GetFirstMember(); reference; reference = reference->next())
    {
        Player* member = reference->getSource();
        if (member && member->IsInWorld() && member->isRealPlayer() &&
            (!selected || member->GetGUIDLow() < selected->GetGUIDLow()))
            selected = member;
    }
    return selected;
}

bool PlayerbotRendezvousManager::PartyHasHuman(Player* bot) const
{
    return FindPartyHuman(bot) != nullptr;
}

bool PlayerbotRendezvousManager::PartyInstanceBoundarySafe(Player* bot, Player* player) const
{
    if (!bot || !player || !bot->GetMap() || !player->GetMap()) return false;
    bool botInDungeon = bot->GetMap()->IsDungeon();
    bool playerInDungeon = player->GetMap()->IsDungeon();
    if (!botInDungeon && !playerInDungeon) return true;
    return botInDungeon && playerInDungeon && bot->GetMapId() == player->GetMapId() &&
        bot->GetInstanceId() == player->GetInstanceId();
}

bool PlayerbotRendezvousManager::PartySafeToRelease(Player* bot) const
{
    return bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() && !bot->GetTransport() &&
        !bot->IsTaxiFlying() && !bot->InBattleGround() && !bot->GetMap()->IsDungeon();
}

void PlayerbotRendezvousManager::ClearMovementState(Player* bot, Player* master, bool restoreFollow)
{
    if (!bot || !bot->GetPlayerbotAI()) return;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    AiObjectContext* context = ai->GetAiObjectContext();
    ai->StopMoving();
    sTravelMgr.SetNullTravelTarget(context->GetValue<TravelTarget*>("travel target")->Get());
    context->GetValue<GuidPosition>("rpg target")->Set(GuidPosition());
    context->ClearValues("possible rpg targets");
    context->ClearValues("nearest npcs");
    context->ClearValues("nearest npcs no los");
    context->ClearValues("no active travel destinations");
    ai->SetMaster(master);
    ai->RequestStrategyReset(true, restoreFollow ?
        "+follow,-stay,-wander,-travel,-travel once,-rpg,-rpg craft" :
        "-follow,-stay,-wander,-travel,-travel once,-rpg,-rpg craft");
}

void PlayerbotRendezvousManager::BeginPartyHandoff(PartySession& session, Player* bot,
    Player* player, const std::string& reason)
{
    ClearMovementState(bot, player, true);
    const auto now = std::chrono::steady_clock::now();
    session.state = "handoff";
    session.reason = reason;
    session.stateSince = now;
    session.approachIssued = false;
    session.lastHumanDistance = bot && player && bot->GetMapId() == player->GetMapId() ?
        bot->GetDistance(player) : 100000.0f;
    session.postArrivalErrandGraceUntil = now + std::chrono::seconds(
        std::max<uint32>(1, sPlayerbotAIConfig.chatDirectorPartyPostArrivalErrandGraceSeconds));
    PersistPartySession(session);
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::rendezvous, PartyActivityPhase::preparing,
        "arrival_handoff_started", reason);
    LogPartyEvent(session, "arrival_handoff_started");
}

PlayerbotRendezvousManager::ErrandObservation
PlayerbotRendezvousManager::ObserveErrandState(Player* bot) const
{
    ErrandObservation result;
    if (!bot || !bot->GetPlayerbotAI()) return result;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
    result.bagUsage = pressure.bagUsage;
    result.durability = ai->GetAiObjectContext()->GetValue<uint8>("durability inventory")->Get();
    result.vendorStacks = pressure.vendorStacks;
    result.bankStacks = pressure.bankStacks + pressure.craftStacks;
    result.auctionStacks = pressure.auctionStacks;
    time_t now = time(nullptr);
    for (PlayerMails::iterator mail = bot->GetMailBegin(); mail != bot->GetMailEnd(); ++mail)
        if ((*mail)->state != MAIL_STATE_DELETED && now >= (*mail)->deliver_time &&
            ((*mail)->has_items || (*mail)->money))
            ++result.mailPayloads;
    if (std::unique_ptr<QueryResult> auctions = CharacterDatabase.PQuery(
        "SELECT COUNT(*) FROM auction WHERE itemowner='%u'", bot->GetGUIDLow()))
        result.auctionCount = (*auctions)[0].GetUInt32();
    const uint32 professions[] = {164,165,171,182,186,197,202,333,393,755};
    for (uint32 skill : professions)
        result.professionSkill += bot->GetSkillValue(skill);
    ai::ListItemsVisitor inventory;
    ai->InventoryIterateItems(&inventory, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
    uint32 signature = 2166136261u;
    for (const auto& item : inventory.items)
    {
        signature = (signature ^ item.first) * 16777619u;
        signature = (signature ^ uint32(std::max<int>(0, item.second))) * 16777619u;
    }
    result.inventorySignature = signature;
    for (const auto& spell : bot->GetSpellMap())
        if (spell.second.state != PLAYERSPELL_REMOVED && !spell.second.disabled)
            result.knownSpells.insert(spell.first);
    return result;
}

bool PlayerbotRendezvousManager::StartNextVerifiedErrand(PartySession& session, Player* bot)
{
    uint32 remaining = session.automaticErrandScopeMask &
        ~(session.completedErrandMask | session.deferredErrandMask);
    const uint32 priorities[] = {kErrandTraining, kErrandVendor, kErrandRepair, kErrandBank,
        kErrandMail, kErrandAuction, kErrandProfession};
    session.currentErrand = 0;
    for (uint32 task : priorities)
        if (remaining & task) { session.currentErrand = task; break; }
    session.automaticErrandMask = remaining;
    if (!session.currentErrand) return false;
    PartySettlementErrand& taskRecord = session.errands[session.currentErrand];
    taskRecord.type = session.currentErrand;
    if (taskRecord.taskId.empty())
    {
        if (!errandSequence) errandSequence = uint64(time(nullptr)) * 1000000ULL;
        taskRecord.taskId = "party-errand:" + std::to_string(session.botGuid) + ':' +
            std::to_string(++errandSequence);
    }
    session.currentErrandId = taskRecord.taskId;
    taskRecord.phase = PartyActivityPhase::preparing;
    taskRecord.outcomeCode.clear();
    if (!(PersonalErrandMask(bot) & session.currentErrand))
    {
        session.errandBefore = ObserveErrandState(bot);
        session.deferredErrandMask |= session.currentErrand;
        taskRecord.before = taskRecord.after = session.errandBefore;
        taskRecord.phase = PartyActivityPhase::deferred;
        taskRecord.outcomeCode = "precondition_changed";
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::party_errand, PartyActivityPhase::deferred,
            "task_deferred", "precondition_changed", session.currentErrand,
            &session.errandBefore, nullptr);
        session.currentErrand = 0;
        session.currentErrandId.clear();
        return StartNextVerifiedErrand(session, bot);
    }
    session.currentErrandCapability = session.currentErrand == kErrandProfession ?
        FindExecutableProfessionSpell(bot) : 0;
    session.currentErrandOutput = 0;
    session.currentErrandOutputCountBefore = 0;
    if (session.currentErrand == kErrandProfession)
    {
        const SpellEntry* spell = sServerFacade.LookupSpellInfo(session.currentErrandCapability);
        if (!spell || !spell->EffectItemType[0])
        {
            FinishCurrentErrand(session, bot, false, "profession_capability_unavailable");
            return session.currentErrand != 0;
        }
        session.currentErrandOutput = spell->EffectItemType[0];
        session.currentErrandOutputCountBefore =
            InventoryItemCount(bot, session.currentErrandOutput);
    }
    session.errandFallbackUsed = false;
    session.errandCatchupUsed = false;
    session.errandTravelStarted = std::chrono::steady_clock::now();
    session.nextErrandCatchupAttempt = session.errandTravelStarted;
    session.errandBefore = ObserveErrandState(bot);
    taskRecord.before = session.errandBefore;
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::party_errand, PartyActivityPhase::preparing,
        "task_queued", "validated_capability", session.currentErrand,
        &session.errandBefore, nullptr);

    const auto now = std::chrono::steady_clock::now();
    session.currentErrandDeadline = now + std::chrono::seconds(
        std::max<uint32>(10, sPlayerbotAIConfig.chatDirectorPartyTaskActiveDeadlineSeconds));
    session.errandTravel.Reset(now, bot->GetMapId(), bot->GetPositionX(),
        bot->GetPositionY(), bot->GetPositionZ());
    session.errandRouteAttempts = 0;
    session.errandOperationAttempts = 0;
    session.errandOperationAccepted = false;
    session.nextErrandStep = now + std::chrono::milliseconds(750);
    if (session.currentErrand == kErrandProfession)
    {
        // ClearMovementState released close-follow before the bundle began.
        // This exact no-focus craft therefore executes locally under the
        // party-errand lease after a brief staging handoff, without pretending
        // a profession trainer is its service destination.
        session.currentErrandLocal = true;
        taskRecord.phase = PartyActivityPhase::departing;
        bot->GetPlayerbotAI()->StopMoving();
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::party_errand, PartyActivityPhase::departing,
            "task_staged", "local_no_focus_capability", session.currentErrand,
            &session.errandBefore, nullptr);
        return true;
    }
    session.currentErrandLocal = false;
    TravelDestination* destination = nullptr;
    WorldPosition* position = nullptr;
    if (!FindSettlementErrandDestination(bot, session.currentErrand, destination, position))
    {
        FinishCurrentErrand(session, bot, false, "no_service_route");
        return session.currentErrand != 0;
    }
    // The interaction timer starts on arrival. Long-distance travel is governed
    // by observed progress and bounded retries, never an overall errand radius.
    session.currentErrandDeadline = std::chrono::steady_clock::time_point();
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    sTravelMgr.SetNullTravelTarget(target);
    target->SetTarget(destination, position);
    target->SetForced(true);
    target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
    ai->GetAiObjectContext()->ClearValues("no active travel destinations");
    session.errandRouteAttempts = 1;
    taskRecord.routeAttempts = session.errandRouteAttempts;
    taskRecord.phase = PartyActivityPhase::traveling;
    session.errandRelocationPending = false;
    session.errandLastDistance = target->Distance(bot);
    sLog.outString("Living WoW errand event=service_route bot=%u task=%s entry=%d map=%u distance=%.0f",
        bot->GetGUIDLow(), ErrandName(session.currentErrand), destination->GetEntry(),
        position->getMapId(), session.errandLastDistance);
    session.nextErrandStep = now;
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::party_errand, taskRecord.phase,
        "task_traveling", "service_travel_route",
        session.currentErrand, &session.errandBefore, nullptr);
    return true;
}

bool PlayerbotRendezvousManager::TryErrandServiceCatchup(PartySession& session, Player* bot,
    TravelTarget* target, std::chrono::steady_clock::time_point now)
{
    // Preserve the visible departure and final approach. Only the long leg is
    // handled by catch-up; service execution and its postcondition are unchanged.
    if (!sPlayerbotAIConfig.chatDirectorRendezvousCatchup || session.errandCatchupUsed ||
        session.freeTimeRecallRequested || session.errandOperationAttempts || !target || !target->GetPosition() ||
        !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() ||
        bot->IsBeingTeleported() || bot->IsTaxiFlying() || bot->GetTransport() ||
        bot->IsInWater() || bot->IsFlying() ||
        (bot->m_movementInfo.GetMovementFlags() & (MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR)) ||
        bot->IsNonMeleeSpellCasted(false) || bot->GetTradeData() ||
        bot->InBattleGround() || bot->duel || now < session.nextErrandCatchupAttempt)
        return false;
    session.nextErrandCatchupAttempt = now + std::chrono::seconds(5);
    WorldPosition* service = target->GetPosition();
    if (!service->isOverworld() || !WorldPosition(bot).isOverworld()) return false;
    const uint32 triggerSeconds = std::max<uint32>(10, std::min<uint32>(300,
        sPlayerbotAIConfig.chatDirectorRendezvousTriggerSeconds));
    const uint32 maximumSeconds = std::max<uint32>(10, std::min<uint32>(60,
        sPlayerbotAIConfig.chatDirectorRendezvousMaximumSeconds));
    const long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - session.errandTravelStarted).count();
    const bool sameMap = bot->GetMapId() == service->getMapId();
    const float distance = target->Distance(bot);
    if (!LivingWowServiceCatchupNeeded(sameMap, distance, elapsed, triggerSeconds, maximumSeconds))
        return false;
    if (!IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()))
        return false;

    Map* destinationMap = service->getMap(0);
    if (!destinationMap) return false;
    const uint32 targetSeconds = std::max<uint32>(5, std::min<uint32>(30,
        sPlayerbotAIConfig.chatDirectorRendezvousTargetSeconds));
    const float desired = std::min(140.0f, kRunSpeedYardsPerSecond * targetSeconds);
    const float radii[] = {desired, desired * 0.75f, desired * 0.5f};
    for (float radius : radii)
    {
        for (uint32 step = 0; step < 8; ++step)
        {
            const float angle = float((step + session.botGuid) % 8) * float(M_PI) / 4.0f;
            WorldPosition candidate(service->getMapId(),
                service->getX() + std::cos(angle) * radius,
                service->getY() + std::sin(angle) * radius, service->getZ());
            service->loadMapAndVMaps(candidate, 0);
            PathFinder path(service->getMapId(), 0);
            if (!path.calculate(service->getVector3(), candidate.getVector3(), false)) continue;
            const PathType invalid = PathType(PATHFIND_NOPATH | PATHFIND_SHORTCUT |
                PATHFIND_INCOMPLETE | PATHFIND_NOT_USING_PATH | PATHFIND_SHORT);
            if (!(path.getPathType() & PATHFIND_NORMAL) || (path.getPathType() & invalid)) continue;
            std::vector<WorldPosition> points = service->fromPointsArray(path.getPath());
            if (points.size() < 2 || points.front().distance(*service) > INTERACTION_DISTANCE) continue;
            WorldPosition landing = points.back();
            const float finalDistance = landing.distance(*service);
            if (!landing.isValid() || finalDistance < 15.0f || finalDistance > 180.0f ||
                landing.distance(candidate) > 10.0f || service->getPathLength(points) > 210.0f) continue;
            // Validate the return direction too, including the precise trainer
            // floor. Never choose a raw terrain height or an incomplete shortcut.
            if (!path.calculate(landing.getVector3(), service->getVector3(), false) ||
                !(path.getPathType() & PATHFIND_NORMAL) || (path.getPathType() & invalid)) continue;
            points = service->fromPointsArray(path.getPath());
            if (points.size() < 2 || points.back().distance(*service) > INTERACTION_DISTANCE ||
                points.front().distance(landing) > 2.0f || service->getPathLength(points) > 210.0f) continue;
            if (!CanRelocateUnobserved(bot, destinationMap, landing.getX(), landing.getY(), landing.getZ()))
                continue;
            if (!ClaimRelocationSlot()) return false;
            PlayerbotAI* ai = bot->GetPlayerbotAI();
            ai->StopMoving();
            ai->GetAiObjectContext()->GetValue<LastMovement&>("last movement")->Get().clear();
            if (sameMap)
                bot->NearTeleportTo(landing.getX(), landing.getY(), landing.getZ() + 0.1f, bot->GetOrientation());
            else if (!bot->TeleportTo(service->getMapId(), landing.getX(), landing.getY(),
                landing.getZ() + 0.1f, bot->GetOrientation())) return false;
            session.errandCatchupUsed = true;
            session.errandWorldportSince = now;
            session.errandTravel.Reset(now, service->getMapId(), landing.getX(), landing.getY(), landing.getZ());
            session.errandLastDistance = finalDistance;
            session.nextErrandStep = now + std::chrono::seconds(1);
            target->SetForced(true);
            target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
            ai->GetAiObjectContext()->ClearValues("no active travel destinations");
            sLog.outString("Living WoW errand event=service_catchup bot=%u task=%s entry=%d map=%u remaining=%.0f",
                bot->GetGUIDLow(), ErrandName(session.currentErrand), target->GetEntry(),
                service->getMapId(), finalDistance);
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::party_errand, PartyActivityPhase::traveling,
                "service_catchup", "final_service_approach", session.currentErrand);
            return true;
        }
    }
    return false;
}

bool PlayerbotRendezvousManager::ExecuteVerifiedErrand(PartySession& session, Player* bot)
{
    if (!bot || !bot->GetPlayerbotAI()) return false;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    if (session.currentErrand == kErrandTraining)
    {
        TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
        if (!target || !target->GetEntry()) return false;
        std::list<ObjectGuid> npcs = ai->GetAiObjectContext()->
            GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get();
        for (const ObjectGuid& guid : npcs)
        {
            if (guid.GetEntry() != uint32(target->GetEntry())) continue;
            Creature* trainer = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_TRAINER);
            if (!trainer || !trainer->IsTrainerOf(bot, false) ||
                !LivingWowHasClassTraining(bot, trainer->GetEntry())) continue;
            session.currentErrandCapability = trainer->GetEntry();
            return ai->DoSpecificAction("trainer", Event("living party training", guid), true);
        }
        return false;
    }
    Event event("rpg action", "living-wow-verified-errand", nullptr);
    if (session.currentErrand == kErrandVendor)
        return ai->DoSpecificAction("sell", event, true);
    if (session.currentErrand == kErrandRepair)
        return ai->DoSpecificAction("repair", event, true);
    if (session.currentErrand == kErrandBank)
        return ai->DoSpecificAction("bank", Event("rpg action", "living-wow-safe-storage", nullptr), true);
    if (session.currentErrand == kErrandMail)
        return ai->DoSpecificAction("mail", Event("rpg action", "take", nullptr), true);
    if (session.currentErrand == kErrandAuction)
        return ai->DoSpecificAction("ah", Event("rpg action", "vendor", nullptr), true);
    if (session.currentErrand == kErrandProfession)
        return session.currentErrandCapability && ai->DoSpecificAction("cast custom nc spell",
            Event("organic economy", std::to_string(session.currentErrandCapability) + " 1", nullptr), true);
    return false;
}

bool PlayerbotRendezvousManager::VerifyErrand(const PartySession& session,
    const ErrandObservation& after) const
{
    uint32 errand = session.currentErrand;
    const ErrandObservation& before = session.errandBefore;
    if (errand == kErrandTraining)
    {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(session.botGuid);
        if (!bot || !session.currentErrandCapability ||
            LivingWowHasClassTraining(bot, session.currentErrandCapability)) return false;
        for (uint32 spell : after.knownSpells)
            if (!before.knownSpells.count(spell)) return true;
        return false;
    }
    if (errand == kErrandVendor)
        return after.vendorStacks < before.vendorStacks || after.bagUsage < before.bagUsage;
    if (errand == kErrandRepair)
        return after.durability > before.durability;
    if (errand == kErrandBank)
        return after.bankStacks < before.bankStacks || after.bagUsage < before.bagUsage;
    if (errand == kErrandMail)
        return after.mailPayloads < before.mailPayloads;
    if (errand == kErrandAuction)
        return after.auctionCount > before.auctionCount || after.auctionStacks < before.auctionStacks;
    if (errand == kErrandProfession)
        return after.professionSkill > before.professionSkill ||
            InventoryItemCount(sRandomPlayerbotMgr.GetPlayerBot(session.botGuid),
                session.currentErrandOutput) > session.currentErrandOutputCountBefore;
    return false;
}

void PlayerbotRendezvousManager::FinishCurrentErrand(PartySession& session, Player* bot,
    bool completed, const std::string& reason)
{
    const uint32 task = session.currentErrand;
    ErrandObservation after = ObserveErrandState(bot);
    // One trainer visit can finish while other class trainers still have lessons
    // (notably city teleports). Preserve the bundle until those visits complete.
    bool continueTraining = false;
    TravelDestination* nextTrainer = nullptr;
    WorldPosition* nextTrainerPosition = nullptr;
    if (completed && task == kErrandTraining && !session.freeTimeRecallRequested &&
        bot && bot->GetPlayerbotAI())
    {
        AiObjectContext* context = bot->GetPlayerbotAI()->GetAiObjectContext();
        context->ClearValues("trainable spells");
        context->ClearValues("available trainers");
        continueTraining = FindSettlementErrandDestination(bot, kErrandTraining,
            nextTrainer, nextTrainerPosition) &&
            nextTrainer->GetEntry() != int32(session.currentErrandCapability);
    }
    if (completed && !continueTraining) session.completedErrandMask |= task;
    else if (!completed) session.deferredErrandMask |= task;
    PartySettlementErrand& record = session.errands[task];
    record.type = task;
    record.taskId = session.currentErrandId;
    record.routeAttempts = session.errandRouteAttempts;
    record.operationAttempts = session.errandOperationAttempts;
    record.before = session.errandBefore;
    record.after = after;
    record.phase = completed ? PartyActivityPhase::completed : PartyActivityPhase::deferred;
    record.outcomeCode = reason;
    session.automaticErrandMask = session.automaticErrandScopeMask &
        ~(session.completedErrandMask | session.deferredErrandMask);
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::party_errand,
        completed ? PartyActivityPhase::completed : PartyActivityPhase::deferred,
        completed ? "task_completed" : "task_deferred", reason, task,
        &session.errandBefore, &after);
    if (bot && bot->GetPlayerbotAI())
    {
        TravelTarget* target = bot->GetPlayerbotAI()->GetAiObjectContext()->
            GetValue<TravelTarget*>("travel target")->Get();
        sTravelMgr.SetNullTravelTarget(target);
        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("no active travel destinations");
    }
    session.currentErrand = 0;
    session.currentErrandCapability = 0;
    session.currentErrandOutput = 0;
    session.currentErrandOutputCountBefore = 0;
    session.currentErrandLocal = false;
    session.currentErrandId.clear();
    session.errandOperationAccepted = false;
    session.errandRelocationPending = false;
    session.errandLastDistance = -1.0f;
    if (continueTraining)
    {
        // Each verified trainer visit gets its own task id and before/after record.
        record.taskId.clear();
        sLog.outString("Living WoW errand event=training_continue bot=%u next_trainer=%d",
            session.botGuid, nextTrainer->GetEntry());
    }
    if (!session.freeTimeRecallRequested && !StartNextVerifiedErrand(session, bot))
        session.freeTimeRecallRequested = true;
    PersistPartySession(session);
}

void PlayerbotRendezvousManager::UpdateVerifiedErrand(PartySession& session, Player* bot,
    Player* player, std::chrono::steady_clock::time_point now)
{
    if (!bot || !player) { session.freeTimeRecallRequested = true; return; }
    // Both same-map and cross-map teleports await an acknowledgement. Do not
    // issue movement or service interactions against the pre-transfer position.
    if (bot->IsBeingTeleported()) return;
    if (session.freeTimeRecallRequested)
    {
        // Recall never begins another operation. An already accepted atomic
        // craft/mail/vendor operation may finish; once its cast is complete,
        // verify it exactly once and defer every untouched task.
        if (bot->IsInCombat() || (session.errandOperationAccepted &&
            bot->IsNonMeleeSpellCasted(false)))
            return;
        if (session.currentErrand)
        {
            const std::string recallOutcome = session.reason == "bundle_deadline" ?
                "bundle_deadline" : "party_recall";
            ErrandObservation after = ObserveErrandState(bot);
            bool completed = session.errandOperationAccepted && VerifyErrand(session, after);
            const uint32 task = session.currentErrand;
            if (completed) session.completedErrandMask |= task;
            else session.deferredErrandMask |= task;
            PartySettlementErrand& record = session.errands[task];
            record.type = task;
            record.taskId = session.currentErrandId;
            record.routeAttempts = session.errandRouteAttempts;
            record.operationAttempts = session.errandOperationAttempts;
            record.before = session.errandBefore;
            record.after = after;
            record.phase = completed ? PartyActivityPhase::completed : PartyActivityPhase::deferred;
            record.outcomeCode = completed ? "verified_during_recall" : recallOutcome;
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::party_errand,
                completed ? PartyActivityPhase::completed : PartyActivityPhase::deferred,
                completed ? "task_completed" : "task_deferred",
                completed ? "verified_during_recall" : recallOutcome, task,
                &session.errandBefore, &after);
        }
        uint32 untouched = session.automaticErrandScopeMask &
            ~(session.completedErrandMask | session.deferredErrandMask);
        session.deferredErrandMask |= untouched;
        const uint32 taskTypes[] = {kErrandVendor, kErrandRepair, kErrandBank,
            kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
        for (uint32 type : taskTypes)
            if (untouched & type)
            {
                PartySettlementErrand& record = session.errands[type];
                record.phase = PartyActivityPhase::deferred;
                record.outcomeCode = session.reason == "bundle_deadline" ?
                    "bundle_deadline" : "party_recall";
            }
        TravelTarget* target = bot->GetPlayerbotAI()->GetAiObjectContext()->
            GetValue<TravelTarget*>("travel target")->Get();
        sTravelMgr.SetNullTravelTarget(target);
        session.currentErrand = 0;
        session.currentErrandCapability = 0;
        session.currentErrandOutput = 0;
        session.currentErrandOutputCountBefore = 0;
        session.currentErrandLocal = false;
        session.currentErrandId.clear();
        session.automaticErrandMask = 0;
        session.errandOperationAccepted = false;
        session.errandRelocationPending = false;
        PersistPartySession(session);
        return;
    }
    if (bot->IsInCombat())
    {
        if (session.errandBlockedSince.time_since_epoch().count() == 0)
        {
            session.errandBlockedSince = now;
            if (session.currentErrand)
                session.errands[session.currentErrand].phase = PartyActivityPhase::blocked;
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::combat, PartyActivityPhase::blocked,
                "task_blocked", "combat", session.currentErrand);
        }
        return;
    }
    if (session.errandBlockedSince.time_since_epoch().count() != 0)
    {
        auto paused = now - session.errandBlockedSince;
        if (session.currentErrandDeadline.time_since_epoch().count())
            session.currentErrandDeadline += paused;
        session.errandTravel.Reset(now, bot->GetMapId(), bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ());
        session.errandBlockedSince = std::chrono::steady_clock::time_point();
        if (session.currentErrand)
            session.errands[session.currentErrand].phase = session.errandOperationAccepted ?
                PartyActivityPhase::verifying :
                (session.currentErrandLocal ? PartyActivityPhase::performing : PartyActivityPhase::traveling);
    }
    if (!session.currentErrand)
    {
        if (!StartNextVerifiedErrand(session, bot))
        {
            session.freeTimeRecallRequested = true;
            PersistPartySession(session);
            return;
        }
        PersistPartySession(session);
    }
    TravelTarget* target = session.currentErrandLocal ? nullptr :
        bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get();
    if (!session.currentErrandLocal && (!target || !target->GetPosition()))
    {
        FinishCurrentErrand(session, bot, false, "route_lost");
        return;
    }
    if (!session.currentErrandLocal && TryErrandServiceCatchup(session, bot, target, now))
        return;
    if (session.errandRelocationPending)
    {
        WorldPosition* position = target->GetPosition();
        bool stillSafe = position && position->getMapId() == bot->GetMapId() &&
            IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) &&
            IsPointUnobserved(bot, position->getX(), position->getY(), position->getZ());
        if (!stillSafe)
        {
            FinishCurrentErrand(session, bot, false, "critical_fallback_unavailable");
            return;
        }
        if (!ClaimRelocationSlot())
            return;
        bot->GetPlayerbotAI()->StopMoving();
        bot->NearTeleportTo(position->getX(), position->getY(), position->getZ(), position->getO());
        target->SetStatus(TravelStatus::TRAVEL_STATUS_WORK);
        session.errandRelocationPending = false;
        session.errandFallbackUsed = true;
        session.errands[session.currentErrand].phase = PartyActivityPhase::traveling;
        session.errandLastDistance = target->Distance(bot);
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::party_errand, PartyActivityPhase::traveling,
            "fallback_travel_used", "critical_service_route", session.currentErrand,
            &session.errandBefore, nullptr);
        return;
    }
    float distance = session.currentErrandLocal ? 0.0f : target->Distance(bot);
    bool reached = session.currentErrandLocal ||
        (target->GetPosition()->getMapId() == bot->GetMapId() && distance <= INTERACTION_DISTANCE);
    if (reached && !session.currentErrandDeadline.time_since_epoch().count())
        session.currentErrandDeadline = now + std::chrono::seconds(
            std::max<uint32>(10, sPlayerbotAIConfig.chatDirectorPartyTaskActiveDeadlineSeconds));
    if (session.currentErrandDeadline.time_since_epoch().count() && now >= session.currentErrandDeadline)
    {
        if (session.errandOperationAccepted && bot->IsNonMeleeSpellCasted(false))
            return;
        if (session.errandOperationAccepted)
        {
            bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bag space");
            bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bank space");
            bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("durability inventory");
            ErrandObservation after = ObserveErrandState(bot);
            if (VerifyErrand(session, after))
            {
                FinishCurrentErrand(session, bot, true, "verified_at_task_deadline");
                return;
            }
        }
        FinishCurrentErrand(session, bot, false, "task_active_deadline");
        return;
    }
    if (!reached)
    {
        session.errandTravel.Observe(now, bot->GetMapId(), bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ(), bot->IsTaxiFlying());
        if (session.errandTravel.Expired(now))
        {
            const uint32 maxRoutes = std::max<uint32>(1, sPlayerbotAIConfig.chatDirectorPartyTaskRouteAttempts);
            if (session.errandRouteAttempts < maxRoutes)
            {
                ++session.errandRouteAttempts;
                session.errands[session.currentErrand].routeAttempts = session.errandRouteAttempts;
                TravelDestination* destination = nullptr; WorldPosition* position = nullptr;
                if (FindSettlementErrandDestination(bot, session.currentErrand, destination, position))
                {
                    // A retry must discard the path that failed, not only
                    // reinstall the same destination over its cached waypoints.
                    bot->GetPlayerbotAI()->StopMoving();
                    bot->GetPlayerbotAI()->GetAiObjectContext()->
                        GetValue<LastMovement&>("last movement")->Get().clear();
                    sTravelMgr.SetNullTravelTarget(target);
                    target->SetTarget(destination, position); target->SetForced(true);
                    target->SetStatus(TravelStatus::TRAVEL_STATUS_TRAVEL);
                    session.errandLastDistance = target->Distance(bot);
                    session.errandTravel.Reset(now, bot->GetMapId(), bot->GetPositionX(),
                        bot->GetPositionY(), bot->GetPositionZ());
                    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                        PartyActivityOwner::party_errand, PartyActivityPhase::traveling,
                        "route_retried", "route_no_progress", session.currentErrand);
                    return;
                }
            }
            bool criticalBags = session.errandBefore.bagUsage >=
                std::min<uint32>(100, sPlayerbotAIConfig.chatDirectorPartyCriticalBagUsagePercent) &&
                (session.currentErrand == kErrandVendor || session.currentErrand == kErrandBank);
            bool criticalRepair = session.currentErrand == kErrandRepair &&
                session.errandBefore.durability == 0;
            bool committed = !sPlayerbotAIConfig.chatDirectorPartyCommittedActionsOnly ||
                (!session.currentErrandId.empty() && session.currentErrand != 0);
            bool critical = criticalBags || criticalRepair;
            TravelDestination* destination = nullptr; WorldPosition* position = nullptr;
            bool canFallback = critical && committed &&
                sPlayerbotAIConfig.chatDirectorPartyFallbackTravel &&
                FindSettlementErrandDestination(bot, session.currentErrand, destination, position) &&
                position && position->getMapId() == bot->GetMapId() &&
                IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ()) &&
                IsPointUnobserved(bot, position->getX(), position->getY(), position->getZ());
            if (canFallback)
            {
                sTravelMgr.SetNullTravelTarget(target);
                target->SetTarget(destination, position); target->SetForced(true);
                target->SetStatus(TravelStatus::TRAVEL_STATUS_PREPARE);
                session.errandRelocationPending = true;
                session.errands[session.currentErrand].phase = PartyActivityPhase::departing;
                QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                    PartyActivityOwner::party_errand, PartyActivityPhase::departing,
                    "fallback_relocation_queued", "critical_service_route", session.currentErrand);
                return;
            }
            FinishCurrentErrand(session, bot, false, critical ?
                "critical_fallback_unavailable" : "route_attempts_exhausted");
            return;
        }
        if (now >= session.nextErrandStep)
        {
            session.nextErrandStep = now + std::chrono::seconds(2);
            bot->GetPlayerbotAI()->DoSpecificAction("move to travel target",
                Event("living party verified errand", ErrandName(session.currentErrand), player), true);
        }
        return;
    }

    bot->GetPlayerbotAI()->StopMoving();
    if (bot->IsNonMeleeSpellCasted(false)) return;
    if (now < session.nextErrandStep) return;
    if (session.errandOperationAccepted)
    {
        session.errands[session.currentErrand].phase = PartyActivityPhase::verifying;
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::party_errand, PartyActivityPhase::verifying,
            "task_verifying", "checking_postcondition", session.currentErrand,
            &session.errandBefore, nullptr);
        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bag space");
        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("bank space");
        bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("durability inventory");
        ErrandObservation after = ObserveErrandState(bot);
        if (VerifyErrand(session, after))
        {
            FinishCurrentErrand(session, bot, true, session.errandFallbackUsed ?
                "verified_after_fallback" : "verified_postcondition");
            return;
        }
        session.errandOperationAccepted = false;
    }
    if (session.errandOperationAttempts >=
        std::max<uint32>(1, sPlayerbotAIConfig.chatDirectorPartyTaskOperationAttempts))
    {
        FinishCurrentErrand(session, bot, false, "postcondition_not_met");
        return;
    }
    ++session.errandOperationAttempts;
    session.errands[session.currentErrand].operationAttempts = session.errandOperationAttempts;
    session.errands[session.currentErrand].phase = PartyActivityPhase::performing;
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest npcs");
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest npcs no los");
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest game objects no los");
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::party_errand, PartyActivityPhase::performing,
        "task_executing", "validated_operation", session.currentErrand,
        &session.errandBefore, nullptr);
    session.errandOperationAccepted = ExecuteVerifiedErrand(session, bot);
    if (session.errandOperationAccepted)
        session.errands[session.currentErrand].phase = PartyActivityPhase::verifying;
    session.nextErrandStep = now + std::chrono::seconds(session.currentErrand == kErrandProfession ? 5 : 2);
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::party_errand, PartyActivityPhase::performing,
        "operation_attempted", session.errandOperationAccepted ? "accepted" : "rejected",
        session.currentErrand, &session.errandBefore, nullptr);
}

void PlayerbotRendezvousManager::RecoverStalePartyCombat(PartySession& session, Player* bot, Player* human)
{
    // Run during arrival as well as follow. A pending arrival otherwise waits
    // forever for a combat engine which can only select invalid targets.
    const auto now = std::chrono::steady_clock::now();
    bool ungroundedCombat = bot && human && bot->IsInWorld() && human->IsInWorld() &&
        bot->IsAlive() && human->IsAlive() && bot->GetPlayerbotAI() &&
        bot->GetMapId() == human->GetMapId() && bot->GetInstanceId() == human->GetInstanceId() &&
        bot->GetDistance(human) > 20.0f && bot->IsInCombat() && !human->IsInCombat() &&
        !bot->GetVictim() && bot->getAttackers().empty() &&
        !bot->getHostileRefManager().getFirst() && !bot->IsNonMeleeSpellCasted(false) &&
        !bot->hasUnitState(UNIT_STAT_CAN_NOT_REACT_OR_LOST_CONTROL) &&
        !bot->IsBeingTeleported() && !bot->IsTaxiFlying() && !bot->GetTransport() &&
        !bot->InBattleGround() && !bot->duel;
    if (!ungroundedCombat)
    {
        session.staleCombatSince = std::chrono::steady_clock::time_point();
        return;
    }
    // A warlock/hunter can have no personal victim while its pet is fighting.
    // Never end that legitimate combat to expedite an arrival.
    if (Unit* pet = bot->GetPet())
    {
        if (pet->IsInCombat() || pet->GetVictim() || !pet->getAttackers().empty() ||
            pet->getHostileRefManager().getFirst() || pet->IsNonMeleeSpellCasted(false))
        {
            session.staleCombatSince = std::chrono::steady_clock::time_point();
            return;
        }
    }
    if (session.staleCombatSince.time_since_epoch().count() == 0)
    {
        session.staleCombatSince = now;
        session.reason = "stale_combat_follow_blocked";
        LogPartyEvent(session, "stale_combat_follow_blocked");
    }
    if (std::chrono::duration_cast<std::chrono::seconds>(now - session.staleCombatSince).count() < 12)
        return;

    bot->CombatStop(true);
    bot->GetPlayerbotAI()->ChangeEngine(BotState::BOT_STATE_NON_COMBAT);
    session.staleCombatSince = std::chrono::steady_clock::time_point();
    session.lastFollowProgress = now - std::chrono::seconds(18);
    session.nextFollowRepair = now;
    session.nextApproachAttempt = now;
    session.reason = "stale_combat_cleared";
    LogPartyEvent(session, "stale_combat_cleared");
}

bool PlayerbotRendezvousManager::StartPartyApproach(PartySession& session, Player* bot, Player* player)
{
    if (!bot || !player || !bot->IsInWorld() || !player->IsInWorld())
    {
        session.reason = "participant_unavailable";
        return false;
    }
    if (!bot->IsAlive())
    {
        session.reason = "bot_dead";
        return false;
    }
    if (bot->IsInCombat())
    {
        session.reason = "bot_in_combat";
        return false;
    }
    if (bot->IsTaxiFlying())
    {
        session.reason = "bot_on_taxi";
        return false;
    }
    if (bot->InBattleGround() || player->InBattleGround())
    {
        session.reason = "restricted_map";
        return false;
    }
    if (!player->IsAlive())
    {
        session.reason = "player_dead";
        return false;
    }
    if (!PartyInstanceBoundarySafe(bot, player))
    {
        auto restricted = freshRestrictedPartyRevisions.find(bot->GetGUIDLow());
        if (restricted == freshRestrictedPartyRevisions.end() ||
            restricted->second != session.partySessionRevision)
        {
            freshRestrictedPartyRevisions[bot->GetGUIDLow()] = session.partySessionRevision;
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::rendezvous, PartyActivityPhase::blocked,
                "arrival_blocked", "cross_instance_mismatch");
        }
        session.reason = "cross_instance_mismatch";
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    bool sameMap = bot->GetMapId() == player->GetMapId() && bot->GetInstanceId() == player->GetInstanceId();
    float distance = sameMap ? bot->GetDistance(player) : 100000.0f;
    uint32 triggerSeconds = std::max<uint32>(10, std::min<uint32>(300,
        sPlayerbotAIConfig.chatDirectorRendezvousTriggerSeconds));
    bool needsCatchup = !sameMap || distance > kRunSpeedYardsPerSecond * triggerSeconds;

    if (needsCatchup)
    {
        if (!sPlayerbotAIConfig.chatDirectorRendezvousCatchup)
        {
            session.reason = "catchup_disabled";
            return false;
        }
        // Authorized catch-up and errand returns do not wait on a per-bot
        // timer. Keep the safety checks and shared world-update relocation slot.

        // FindStagingPoint uses the target player's map and authoritative path
        // data. This also permits a /who invite from another outdoor zone.
        float stageX = 0.0f, stageY = 0.0f, stageZ = 0.0f;
        if (!FindStagingPoint(bot, player, stageX, stageY, stageZ))
        {
            session.reason = "no_hidden_staging_point";
            return false;
        }
        if (!ClaimRelocationSlot())
        {
            session.reason = "relocation_budget_wait";
            return false;
        }
        GenericTransport* transport = bot->GetTransport();
        bot->GetPlayerbotAI()->StopMoving();
        if (transport)
        {
            // Playerbots' normal MoveOffTransport path removes the passenger
            // before teleporting. Reuse the same authoritative transition for
            // party assists instead of leaving zeppelin passengers pending
            // forever.
            transport->RemovePassenger(bot);
            LogPartyEvent(session, "transport_detached_for_arrival");
        }
        bool crossMapRelocation = !sameMap;
        if (sameMap)
            bot->NearTeleportTo(stageX, stageY, stageZ, bot->GetAngle(player));
        else if (!bot->TeleportTo(player->GetMapId(), stageX, stageY, stageZ, player->GetOrientation()))
        {
            session.reason = "cross_map_teleport_rejected";
            return false;
        }
        session.relocated = true;
        session.forceRelocation = false;
        LogPartyEvent(session, "relocated_for_arrival");
        if (crossMapRelocation)
        {
            session.reason = "worldport_ack_pending";
            session.state = "relocating";
            session.stateSince = now;
            session.approachIssued = false;
            PersistPartySession(session);
            return true;
        }
    }
    else
        LogPartyEvent(session, "ordinary_arrival");

    session.reason.clear();
    session.state = "approaching";
    session.stateSince = now;
    session.approachIssued = false;
    session.lastHumanDistance = bot->GetDistance(player);
    session.lastFollowProgress = now;
    session.nextApproachAttempt = now;
    PersistPartySession(session);
    return true;
}

bool PlayerbotRendezvousManager::ReturnPartyToActivity(PartySession& session, Player* bot)
{
    if (!bot || !bot->IsInWorld()) return false;
    auto releaseExternalState = [&](PartyActivityPhase phase, const std::string& reason)
    {
        auto lease = externalLeases.find(session.botGuid);
        if (lease == externalLeases.end()) return;
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            lease->second.owner, phase, "lease_released", reason);
        externalLeases.erase(lease);
    };
    auto restoreAutonomousState = [bot]()
    {
        ClearPartyCatchup(bot);
        PlayerbotAI* ai = bot->GetPlayerbotAI();
        if (!ai) return;
        AiObjectContext* context = ai->GetAiObjectContext();
        ai->StopMoving();
        sTravelMgr.SetNullTravelTarget(context->GetValue<TravelTarget*>("travel target")->Get());
        context->GetValue<GuidPosition>("rpg target")->Set(GuidPosition());
        context->ClearValues("possible rpg targets");
        context->ClearValues("nearest npcs");
        context->ClearValues("nearest npcs no los");
        context->ClearValues("no active travel destinations");
        ai->SetMaster(nullptr);
        ai->RequestStrategyReset(true);
        ai->DoSpecificAction("reset travel target",
            Event("living party autonomous resume"), true);
    };
    auto failReturn = [&](const std::string& reason)
    {
        restoreAutonomousState();
        session.reason = reason;
        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
            PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
            "activity_restore_failed", reason);
        releaseExternalState(PartyActivityPhase::failed, reason);
        ClearPersistedPartySession(session.botGuid);
        return true;
    };

    bool sameOriginMap = bot->GetMapId() == session.originMapId &&
        bot->GetInstanceId() == session.originInstanceId;
    float originDx = bot->GetPositionX() - session.originX;
    float originDy = bot->GetPositionY() - session.originY;
    float originDz = bot->GetPositionZ() - session.originZ;
    bool originNearby = sameOriginMap &&
        originDx * originDx + originDy * originDy + originDz * originDz <= 144.0f;
    bool detachedDungeonReturn = session.state == "dungeon_return";
    const MapEntry* originEntry = sMapStore.LookupEntry(session.originMapId);
    bool originInstanceUnavailable = detachedDungeonReturn && !sameOriginMap &&
        (!originEntry || originEntry->IsDungeon());
    if (originInstanceUnavailable)
    {
        // Never return from one instance to another persisted instance. The
        // dungeon-return state first exits through the bot's normal hearth
        // action; only after the bot is attached to an outdoor map may the
        // terminal cleanup release ownership and autonomous strategies.
        if (bot->GetMap()->IsDungeon()) return false;
        return failReturn("origin_instance_unavailable");
    }
    if ((!bot->GetMap()->IsDungeon() || detachedDungeonReturn) &&
        (session.relocated || !originNearby))
    {
        const auto now = std::chrono::steady_clock::now();
        long waitingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - session.stateSince).count();
        uint32 returnWait = std::max<uint32>(30,
            sPlayerbotAIConfig.chatDirectorPartyReturnWaitSeconds);
        if (session.reason == "return_worldport_ack_timeout")
            return failReturn("return_worldport_ack_timeout");

        if (!sameOriginMap)
        {
            Map* originMap = sMapMgr.FindMap(session.originMapId, session.originInstanceId);
            if (!originMap && !detachedDungeonReturn)
                return waitingSeconds < returnWait ? false :
                    failReturn("origin_map_unavailable");

            bool originHidden = !originMap || IsPointUnobservedOnMap(originMap, bot,
                session.originX, session.originY, session.originZ);
            bool departureHidden = IsPointUnobserved(bot, bot->GetPositionX(),
                bot->GetPositionY(), bot->GetPositionZ());
            if (!originHidden || !departureHidden)
            {
                if (detachedDungeonReturn)
                {
                    std::string blocked = !originHidden ? "origin_observed" :
                        "departure_observed";
                    if (session.reason != blocked)
                    {
                        session.reason = blocked;
                        PersistPartySession(session);
                        QueueActivityTelemetry(session.botGuid, session.playerGuid,
                            session.groupId, PartyActivityOwner::rendezvous,
                            PartyActivityPhase::blocked, "party_return_blocked", blocked);
                    }
                    return false;
                }
                return waitingSeconds < returnWait ? false :
                    failReturn(!originHidden ? "origin_observed" : "departure_observed");
            }
            if (!ClaimRelocationSlot())
                return false;
            if (bot->TeleportTo(session.originMapId, session.originX, session.originY,
                session.originZ, session.originO))
            {
                session.state = "returning";
                session.reason = "return_worldport_ack_pending";
                session.stateSince = now;
                PersistPartySession(session);
                return false;
            }
            if (detachedDungeonReturn)
            {
                if (session.reason != "return_teleport_rejected")
                {
                    session.reason = "return_teleport_rejected";
                    PersistPartySession(session);
                    QueueActivityTelemetry(session.botGuid, session.playerGuid,
                        session.groupId, PartyActivityOwner::rendezvous,
                        PartyActivityPhase::blocked, "party_return_blocked",
                        session.reason);
                }
                return false;
            }
            return waitingSeconds < returnWait ? false :
                failReturn("return_teleport_rejected");
        }

        bool departureHidden = IsPointUnobserved(bot, bot->GetPositionX(),
            bot->GetPositionY(), bot->GetPositionZ());
        bool originHidden = IsPointUnobserved(bot, session.originX,
            session.originY, session.originZ);
        if (!departureHidden || !originHidden)
            return waitingSeconds < returnWait ? false :
                failReturn(!originHidden ? "origin_observed" : "departure_observed");
        if (!ClaimRelocationSlot())
            return false;
        bot->NearTeleportTo(session.originX, session.originY,
            session.originZ, session.originO);
        session.relocated = false;
    }

    // End the party-owned route without leaving the bot stripped of every
    // autonomous movement strategy. Do not replay previousActivity (it is
    // diagnostic text, not a validated command); ask Playerbots to rebuild its
    // normal strategies and choose a fresh authoritative travel target on the
    // next clean AI tick.
    restoreAutonomousState();
    session.reason = "activity_restored";
    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
        PartyActivityOwner::rendezvous, PartyActivityPhase::completed,
        "activity_restored", "party_session_ended");
    releaseExternalState(PartyActivityPhase::completed, "party_session_ended");
    ClearPersistedPartySession(session.botGuid);
    return true;
}

void PlayerbotRendezvousManager::UpdatePartyAssists()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto iterator = partySessions.begin(); iterator != partySessions.end(); )
    {
        PartySession& session = iterator->second;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(session.botGuid);
        bool erase = false;
        if (!bot)
        {
            session.reason = "party_bot_unavailable";
            QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
                "party_session_failed", session.reason);
            LogPartyEvent(session, "party_session_failed");
            erase = true;
        }
        else if (!bot->IsInWorld())
        {
            // Cross-map TeleportTo detaches the player until the worldport ACK.
            // Preserve the party session and its movement lease across that
            // bounded transit window instead of reconstructing it from scratch.
            if (session.state == "free_time" && bot->IsBeingTeleported())
            {
                if (!session.errandWorldportSince.time_since_epoch().count())
                    session.errandWorldportSince = now;
                if (now - session.errandWorldportSince < std::chrono::seconds(45))
                {
                    ++iterator;
                    continue;
                }
            }
            bool relocationTransit = session.state == "relocating" ||
                session.state == "returning";
            bool hearthTransit = session.state == "hearth_sync" &&
                session.hearthStarted.time_since_epoch().count() != 0;
            bool dungeonExitTransit = session.state == "dungeon_return" &&
                session.hearthStarted.time_since_epoch().count() != 0;
            std::chrono::steady_clock::time_point transitStarted =
                (hearthTransit || dungeonExitTransit) ? session.hearthStarted : session.stateSince;
            if ((relocationTransit || hearthTransit || dungeonExitTransit) &&
                std::chrono::duration_cast<std::chrono::seconds>(
                    now - transitStarted).count() < 45)
            {
                ++iterator;
                continue;
            }

            // A worldport that never attaches must still end in a typed
            // lifecycle result.  Returning bots no longer have a mixed party
            // to rediscover, so leave only world-independent cleanup queued on
            // their AI; it will take effect when the session is attached
            // again.  Outbound arrival/hearth sessions deliberately retain
            // the party master and are rediscovered by Update after attach.
            if (session.state == "returning" || dungeonExitTransit)
            {
                if (PlayerbotAI* ai = bot->GetPlayerbotAI())
                {
                    AiObjectContext* context = ai->GetAiObjectContext();
                    sTravelMgr.SetNullTravelTarget(
                        context->GetValue<TravelTarget*>("travel target")->Get());
                    context->GetValue<GuidPosition>("rpg target")->Set(GuidPosition());
                    context->ClearValues("possible rpg targets");
                    context->ClearValues("nearest npcs");
                    context->ClearValues("nearest npcs no los");
                    context->ClearValues("no active travel destinations");
                    ai->SetMaster(nullptr);
                    ai->RequestStrategyReset(true);
                }
                session.reason = dungeonExitTransit ?
                    "dungeon_exit_worldport_ack_timeout" : "return_worldport_ack_timeout";
                QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                    PartyActivityOwner::rendezvous, PartyActivityPhase::failed,
                    "activity_restore_failed", session.reason);
                auto lease = externalLeases.find(session.botGuid);
                if (lease != externalLeases.end())
                {
                    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                        lease->second.owner, PartyActivityPhase::failed,
                        "lease_released", session.reason);
                    externalLeases.erase(lease);
                }
                ClearPersistedPartySession(session.botGuid);
                LogPartyEvent(session, "activity_restore_failed");
            }
            else if (session.state == "relocating" || session.state == "hearth_sync")
            {
                session.reason = session.state == "hearth_sync" ?
                    "hearth_worldport_ack_timeout" : "worldport_ack_timeout";
                QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                    session.state == "hearth_sync" ? PartyActivityOwner::transport :
                        PartyActivityOwner::rendezvous,
                    PartyActivityPhase::failed, "party_transit_failed", session.reason);
                LogPartyEvent(session, "party_transit_failed");
            }
            else
            {
                session.reason = "party_bot_world_unavailable";
                QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                    GetPartyActivityOwner(session.botGuid), PartyActivityPhase::failed,
                    "party_session_failed", session.reason);
                LogPartyEvent(session, "party_session_failed");
            }
            erase = true;
        }
        else
        {
            session.errandWorldportSince = std::chrono::steady_clock::time_point();
            Group* group = bot->GetGroup();
            bool originalParty = group && group->GetId() == session.groupId;
            bool rosterChanged = false;
            if (originalParty)
            {
                // Observe every roster/leader transition, not only chat and
                // telemetry samples. A leave/rejoin cycle therefore cannot
                // revive an unexpired offer from an earlier party lifecycle.
                uint32 currentRosterSignature = GetPartyRosterSignature(bot);
                rosterChanged = currentRosterSignature != session.partyRosterSignature;
                session.partyRosterSignature = currentRosterSignature;
                session.partySessionRevision = GetPartySessionRevision(bot);
                session.partySessionId = GetPartySessionId(bot);
            }
            Player* human = originalParty ? FindPartyHuman(bot) : nullptr;
            if (rosterChanged)
            {
                if (human) session.playerGuid = human->GetGUIDLow();
                PersistPartySession(session);
            }
            bool waitingForReconnect = false;

            // Group membership persists while a real player is temporarily
            // disconnected. Keep the bots and their party-assist state intact
            // for a bounded reconnect window, but do not delay an intentional
            // leave because the player's group membership is then already gone.
            ObjectGuid assistedHuman(HIGHGUID_PLAYER, session.playerGuid);
            bool assistedHumanStillMember = originalParty && group->IsMember(assistedHuman);
            if (!human && assistedHumanStillMember)
            {
                if (session.humanAbsentSince.time_since_epoch().count() == 0)
                {
                    session.humanAbsentSince = now;
                    session.reason = "waiting_for_human_reconnect";
                    LogPartyEvent(session, "reconnect_grace_started");
                }
                uint32 graceSeconds = std::max<uint32>(60, std::min<uint32>(900,
                    sPlayerbotAIConfig.chatDirectorPartyDisconnectGraceSeconds));
                waitingForReconnect = std::chrono::duration_cast<std::chrono::seconds>(
                    now - session.humanAbsentSince).count() < graceSeconds;
            }
            else if (human && session.humanAbsentSince.time_since_epoch().count() != 0)
            {
                session.humanAbsentSince = std::chrono::steady_clock::time_point();
                session.reason.clear();
                LogPartyEvent(session, "human_reconnected");
            }

            // During logout and instance/map transfers the human can remain
            // an authoritative group member while temporarily having no live
            // Player object. Preserve the reconnect grace state, but do not
            // fall through into active/free-time/hearth branches that
            // dereference the absent human. The next world update resumes the
            // same session as soon as FindPartyHuman can resolve the player.
            if (waitingForReconnect)
            {
                ++iterator;
                continue;
            }

            // A bot in a human party may reach a spirit healer only because
            // its automated corpse navigation failed. Do not make the human
            // party wait through resurrection sickness for that automation
            // failure. Keep the normal durability loss, and leave autonomous
            // and bot-only resurrection behavior unchanged.
            if (originalParty && human && bot->IsAlive() &&
                bot->HasAura(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS))
            {
                bot->RemoveAurasDueToSpell(SPELL_ID_PASSIVE_RESURRECTION_SICKNESS);
                LogPartyEvent(session, "party_resurrection_sickness_cleared");
            }

            // The normal dead strategy can be starved by a persistent
            // human-master follow goal. Preserve the party rendezvous while
            // first asking the existing corpse actions to recover normally,
            // then fall back to the normal spirit-healer path after a bounded
            // wait. Never teleport or resurrect the corpse directly here.
            if (originalParty && human && !bot->IsAlive())
            {
                if (session.deadRecoveryStarted.time_since_epoch().count() == 0)
                {
                    session.deadRecoveryStarted = now;
                    session.nextDeadRecoveryAttempt = now;
                    session.deadRecoveryAttempts = 0;
                    session.reason = "waiting_for_corpse_recovery";
                    LogPartyEvent(session, "dead_recovery_started");
                }

                if (now >= session.nextDeadRecoveryAttempt)
                {
                    long recoverySeconds = std::chrono::duration_cast<std::chrono::seconds>(
                        now - session.deadRecoveryStarted).count();
                    Corpse* corpse = bot->GetCorpse();
                    const char* action = !corpse ? "auto release" :
                        (recoverySeconds >= 60 ? "spirit healer" : "find corpse");
                    bool accepted = bot->GetPlayerbotAI()->DoSpecificAction(
                        action, Event("living party dead recovery", "", human), true);
                    ++session.deadRecoveryAttempts;
                    session.nextDeadRecoveryAttempt = now + std::chrono::seconds(10);
                    session.reason = std::string(action) + (accepted ? "_accepted" : "_not_ready");
                    LogPartyEvent(session, accepted ? "dead_recovery_step" : "dead_recovery_wait");
                }

                ++iterator;
                continue;
            }

            if (session.deadRecoveryStarted.time_since_epoch().count() != 0)
            {
                session.deadRecoveryStarted = std::chrono::steady_clock::time_point();
                session.nextDeadRecoveryAttempt = std::chrono::steady_clock::time_point();
                session.deadRecoveryAttempts = 0;
                session.state = "pending";
                session.reason = "dead_recovery_completed";
                session.forceRelocation = true;
                session.approachIssued = false;
                session.nextApproachAttempt = std::chrono::steady_clock::time_point();
                session.stateSince = now;
                PersistPartySession(session);
                LogPartyEvent(session, "dead_recovery_completed");
            }

            // Human-led mixed parties take a natural, bounded break when they
            // settle in a capital or a genuine service hub. Only bots with
            // authoritative work leave follow, and their start times are
            // staggered so entering town does not produce a chat chorus.
            if (originalParty && human && session.state == "active" &&
                GetPartyActivityOwner(session.botGuid) == PartyActivityOwner::party_follow &&
                bot->IsAlive() && human->IsAlive() &&
                !bot->IsInCombat() && !human->IsInCombat() && !bot->IsTaxiFlying() && !bot->GetTransport() &&
                group->IsLeader(human->GetObjectGuid()) &&
                (session.postArrivalErrandGraceUntil.time_since_epoch().count() == 0 ||
                 now >= session.postArrivalErrandGraceUntil) &&
                (!session.nextSettlementCheck.time_since_epoch().count() || now >= session.nextSettlementCheck))
            {
                session.nextSettlementCheck = now + std::chrono::seconds(5);
                uint32 settlement = SettlementKey(bot, human);
                if (!settlement)
                {
                    session.settlementKey = 0;
                    session.automaticErrandReadyAt = std::chrono::steady_clock::time_point();
                }
                else if (session.settlementKey != settlement)
                {
                    session.settlementKey = settlement;
                    session.automaticErrandReadyAt = now + std::chrono::milliseconds(
                        600 + ((session.botGuid * 2654435761u) % 1901));
                    session.nextSettlementCheck = session.automaticErrandReadyAt;
                }
                else
                {
                    for (auto cooldown = session.automaticErrandCooldowns.begin();
                        cooldown != session.automaticErrandCooldowns.end(); )
                        if (cooldown->second <= now)
                            cooldown = session.automaticErrandCooldowns.erase(cooldown);
                        else
                            ++cooldown;
                    if (now >= session.automaticErrandReadyAt &&
                        session.automaticErrandCooldowns.find(settlement) ==
                            session.automaticErrandCooldowns.end())
                    {
                        uint32 errandMask = GroundedSettlementErrandMask(bot);
                        std::vector<std::string> errands = PersonalErrands(errandMask);
                        session.automaticErrandCooldowns[settlement] = now +
                            std::chrono::seconds(errands.empty() ? 120 : std::max<uint32>(60,
                                sPlayerbotAIConfig.chatDirectorPartyErrandCooldownSeconds));
                        if (!errands.empty())
                        {
                            std::string announcement = ErrandAnnouncement(errands);
                            if (BeginPartyFreeTime(bot, human, "automatic_settlement_errands"))
                            {
                                bot->GetPlayerbotAI()->SayToParty(announcement, true,
                                    PlayerbotAI::ChatMessageClass::social);
                                LogPartyEvent(session, "automatic_settlement_errands_started");
                            }
                        }
                    }
                }
            }

            bool canSyncHearth = originalParty && human &&
                GetPartyActivityOwner(session.botGuid) == PartyActivityOwner::party_follow &&
                bot->IsAlive() && human->IsAlive() &&
                !bot->IsInCombat() && !human->IsInCombat() &&
                !bot->IsTaxiFlying() && !bot->GetTransport() && !bot->IsBeingTeleported() &&
                session.state != "departing" && session.state != "hearth_sync" &&
                session.state != "free_time";
            if (canSyncHearth && IsCastingHearthstone(human))
            {
                session.state = "hearth_sync";
                session.reason = "human_hearthstone";
                session.hearthStartMapId = human->GetMapId();
                session.hearthStartX = human->GetPositionX();
                session.hearthStartY = human->GetPositionY();
                session.hearthStartZ = human->GetPositionZ();
                session.hearthStarted = now;
                bot->GetPlayerbotAI()->DoSpecificAction(
                    "hearthstone", Event("living party hearth", "follow human hearth", human), true);
                PersistPartySession(session);
                LogPartyEvent(session, "hearth_sync_started");
            }

            if (session.state == "dungeon_return")
            {
                // The mixed party is already gone and its master was cleared.
                // Return only to the persisted, validated pre-party origin,
                // with the same observer checks and one-teleport-per-update
                // serialization used by every rendezvous transition. This is
                // distinct from active-party catch-up, which may never cross
                // an instance boundary.
                const MapEntry* originEntry = sMapStore.LookupEntry(session.originMapId);
                bool originInstanceUnavailable = !originEntry || originEntry->IsDungeon();
                bool safeToReturn = bot->IsAlive() && !bot->IsInCombat() &&
                    !bot->GetTransport() && !bot->IsTaxiFlying() &&
                    !bot->InBattleGround();
                if (originInstanceUnavailable && !bot->GetMap()->IsDungeon())
                {
                    if (ReturnPartyToActivity(session, bot))
                    {
                        LogPartyEvent(session, "origin_instance_unavailable");
                        erase = true;
                    }
                }
                else if (originInstanceUnavailable && safeToReturn &&
                    !IsCastingHearthstone(bot) &&
                    (session.nextApproachAttempt.time_since_epoch().count() == 0 ||
                     now >= session.nextApproachAttempt))
                {
                    // A stale/different dungeon origin cannot be a teleport
                    // destination. Leave the current instance through a real,
                    // validated player action and hold the lease until the
                    // resulting worldport attaches outdoors.
                    session.nextApproachAttempt = now + std::chrono::seconds(30);
                    bool accepted = bot->GetPlayerbotAI()->DoSpecificAction(
                        "hearthstone", Event("living party dungeon exit", "", nullptr), true);
                    if (accepted)
                    {
                        session.hearthStarted = now;
                        session.reason = "origin_instance_unavailable_exit_started";
                        PersistPartySession(session);
                        QueueActivityTelemetry(session.botGuid, session.playerGuid,
                            session.groupId, PartyActivityOwner::rendezvous,
                            PartyActivityPhase::returning, "dungeon_exit_started",
                            "normal_hearth_exit");
                    }
                    else if (session.reason != "origin_instance_unavailable_exit_blocked")
                    {
                        session.reason = "origin_instance_unavailable_exit_blocked";
                        PersistPartySession(session);
                        QueueActivityTelemetry(session.botGuid, session.playerGuid,
                            session.groupId, PartyActivityOwner::rendezvous,
                            PartyActivityPhase::blocked, "party_return_blocked",
                            session.reason);
                    }
                }
                else if (!originInstanceUnavailable && safeToReturn &&
                    (session.nextApproachAttempt.time_since_epoch().count() == 0 ||
                     now >= session.nextApproachAttempt))
                {
                    session.nextApproachAttempt = now + std::chrono::seconds(5);
                    if (ReturnPartyToActivity(session, bot))
                    {
                        LogPartyEvent(session, session.reason == "activity_restored" ?
                            "dungeon_return_completed" : "activity_restore_failed");
                        erase = true;
                    }
                }
                else if (!safeToReturn && session.reason != "dungeon_return_safety_blocked" &&
                    std::chrono::duration_cast<std::chrono::seconds>(
                        now - session.stateSince).count() >= 180)
                {
                    session.reason = "dungeon_return_safety_blocked";
                    PersistPartySession(session);
                    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                        PartyActivityOwner::rendezvous, PartyActivityPhase::blocked,
                        "party_return_blocked", session.reason);
                    LogPartyEvent(session, "dungeon_return_safety_blocked");
                }
            }
            else if (session.state == "departing")
            {
                bool canRestoreInsideOrigin = bot->GetMap()->IsDungeon() &&
                    !session.relocated && session.originMapId == bot->GetMapId() &&
                    session.originInstanceId == bot->GetInstanceId() && bot->IsAlive() &&
                    !bot->IsInCombat() && !bot->GetTransport() && !bot->IsTaxiFlying();
                if (PartySafeToRelease(bot) || canRestoreInsideOrigin)
                {
                    long elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session.stateSince).count();
                    bool hidden = IsPointUnobserved(bot, bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ());
                    if (hidden || elapsed >= std::max<uint32>(15,
                        std::min<uint32>(300, sPlayerbotAIConfig.chatDirectorRendezvousDepartureSeconds)))
                    {
                        if (ReturnPartyToActivity(session, bot))
                        {
                            LogPartyEvent(session, session.reason == "activity_restored" ?
                                (hidden ? "activity_restored" : "natural_resume_no_visible_teleport") :
                                "activity_restore_failed");
                            erase = true;
                        }
                    }
                }
            }
            else if ((!originalParty || !human) && !waitingForReconnect &&
                session.state != "returning")
            {
                bool releaseSafe = bot->IsAlive() && !bot->IsInCombat() &&
                    !bot->GetTransport() && !bot->IsTaxiFlying() && !bot->InBattleGround();
                if (session.state != "departing" && session.state != "dungeon_return" &&
                    releaseSafe)
                {
                    uint32 unfinishedErrands = session.automaticErrandScopeMask &
                        ~(session.completedErrandMask | session.deferredErrandMask);
                    session.deferredErrandMask |= unfinishedErrands;
                    const uint32 errandTypes[] = {kErrandVendor, kErrandRepair,
                        kErrandBank, kErrandMail, kErrandAuction, kErrandProfession, kErrandTraining};
                    for (uint32 type : errandTypes)
                    {
                        if (!(unfinishedErrands & type)) continue;
                        PartySettlementErrand& record = session.errands[type];
                        record.type = type;
                        record.phase = PartyActivityPhase::deferred;
                        record.outcomeCode = "party_ended";
                        QueueActivityTelemetry(session.botGuid, session.playerGuid,
                            session.groupId, PartyActivityOwner::party_errand,
                            PartyActivityPhase::deferred, "task_deferred",
                            "party_ended", type);
                    }
                    session.currentErrand = 0;
                    session.currentErrandId.clear();
                    session.automaticErrandMask = 0;
                    if (group && group->GetId() == session.groupId && !PartyHasHuman(bot))
                    {
                        WorldPacket packet;
                        packet << uint32(PARTY_OP_LEAVE) << bot->GetName() << uint32(0);
                        bot->GetSession()->HandleGroupDisbandOpcode(packet);
                    }
                    auto activeLease = externalLeases.find(session.botGuid);
                    if (activeLease != externalLeases.end())
                    {
                        QueueActivityTelemetry(session.botGuid, session.playerGuid,
                            session.groupId, activeLease->second.owner,
                            PartyActivityPhase::deferred, "lease_released",
                            "party_session_ended");
                        externalLeases.erase(activeLease);
                    }
                    bool inDungeon = bot->GetMap()->IsDungeon();
                    bool originIsCurrentInstance = session.originMapId == bot->GetMapId() &&
                        session.originInstanceId == bot->GetInstanceId();
                    if (inDungeon && !originIsCurrentInstance)
                    {
                        ClearMovementState(bot, nullptr, false);
                        session.state = "dungeon_return";
                        const MapEntry* originEntry = sMapStore.LookupEntry(session.originMapId);
                        session.reason = !originEntry || originEntry->IsDungeon() ?
                            "origin_instance_unavailable" : "instance_exit_required";
                        session.stateSince = now;
                        PersistPartySession(session);
                        QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                            PartyActivityOwner::rendezvous, PartyActivityPhase::deferred,
                            "party_return_deferred", session.reason);
                        LogPartyEvent(session, "dungeon_return_started");
                        ++iterator;
                        continue;
                    }
                    session.state = "departing";
                    session.reason = originalParty ? "last_human_left" : "party_ended";
                    session.stateSince = now;
                    // Walk away naturally first; the hidden return occurs only
                    // after no real player can observe either endpoint.
                    float angle = bot->GetOrientation();
                    float x = bot->GetPositionX() + std::cos(angle) * 45.0f;
                    float y = bot->GetPositionY() + std::sin(angle) * 45.0f;
                    float z = bot->GetMap()->GetHeight(x, y, bot->GetPositionZ() + 10.0f);
                    if (z > -100000.0f)
                        bot->GetMotionMaster()->MovePoint(bot->GetMapId(), x, y, z, FORCED_MOVEMENT_RUN);
                    PersistPartySession(session);
                    LogPartyEvent(session, "departure_started");
                }
            }
            else if (session.state == "hearth_sync")
            {
                long elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - session.hearthStarted).count();
                if (!IsCastingHearthstone(human))
                {
                    if (elapsed >= 8)
                    {
                        // A bot's own hearth bind may differ from the human's.
                        // Reuse the authoritative party rendezvous after the cast
                        // so every bot converges on the human's actual destination.
                        session.state = "pending";
                        session.reason = "follow_human_hearth_destination";
                        session.forceRelocation = true;
                        session.approachIssued = false;
                        session.nextApproachAttempt = std::chrono::steady_clock::time_point();
                        session.stateSince = now;
                        PersistPartySession(session);
                        LogPartyEvent(session, "hearth_destination_queued");
                    }
                    else
                    {
                        Spell* spell = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                        if (spell && spell->m_spellInfo && spell->m_spellInfo->Id == 8690)
                            bot->InterruptSpell(CURRENT_GENERIC_SPELL, false);
                        bool nearby = bot->GetMapId() == human->GetMapId() &&
                            bot->GetInstanceId() == human->GetInstanceId() && bot->IsWithinDistInMap(human, 12.0f);
                        session.state = nearby ? "active" : "approaching";
                        session.reason = "human_hearth_cancelled";
                        session.approachIssued = false;
                        session.stateSince = now;
                        if (nearby)
                            session.postArrivalErrandGraceUntil = now + std::chrono::seconds(
                                sPlayerbotAIConfig.chatDirectorPartyPostArrivalErrandGraceSeconds);
                        PersistPartySession(session);
                        LogPartyEvent(session, "hearth_sync_cancelled");
                    }
                }
            }
            else if (session.state == "free_time")
            {
                const bool independentErrands = sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands;
                bool humanMovedOn = !independentErrands && (human->GetMapId() != bot->GetMapId() ||
                    human->GetZoneId() != session.freeTimePlayerZoneId);
                bool automaticSettlement = session.reason == "automatic_settlement_errands";
                if (!independentErrands && automaticSettlement && !IsCapital(human) &&
                    sServerFacade.GetAreaId(human) != session.freeTimePlayerAreaId)
                    humanMovedOn = true;
                bool errandsFinished = false;
                bool automaticHardTimeout = false;
                bool automaticIdleTimeout = false;
                if ((!independentErrands && human->IsInCombat()) || humanMovedOn)
                {
                    bool newlyRequested = !session.freeTimeRecallRequested;
                    session.freeTimeRecallRequested = true;
                    session.reason = humanMovedOn ? "free_time_party_moved_on" :
                        "free_time_party_entered_combat";
                    if (newlyRequested) PersistPartySession(session);
                }
                if (sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands)
                {
                    UpdateVerifiedErrand(session, bot, human, now);
                    errandsFinished = session.freeTimeRecallRequested &&
                        session.automaticErrandMask == 0;
                }
                else if (automaticSettlement &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - session.stateSince).count() >= 45 &&
                    (!session.nextAutomaticErrandCheck.time_since_epoch().count() ||
                     now >= session.nextAutomaticErrandCheck))
                {
                    session.nextAutomaticErrandCheck = now + std::chrono::seconds(10);
                    uint32 previousErrands = session.automaticErrandMask;
                    // Keep the announced task list stable. A new goal or item
                    // acquired during free time belongs to the next visit and
                    // must not silently expand the promise already made.
                    uint32 remainingErrands = PersonalErrandMask(bot) & session.automaticErrandScopeMask;
                    float dx = bot->GetPositionX() - session.automaticErrandLastX;
                    float dy = bot->GetPositionY() - session.automaticErrandLastY;
                    bool traveled = dx * dx + dy * dy >= 25.0f;

                    if (remainingErrands != previousErrands)
                    {
                        session.automaticErrandMask = remainingErrands;
                        session.freeTimeUntil = std::min(session.automaticErrandHardDeadline,
                            now + std::chrono::minutes(5));
                        LogAutomaticErrandEvent(session, bot, "tasks_changed", previousErrands,
                            remainingErrands);
                        if (remainingErrands)
                            SetAutomaticErrandTarget(bot, remainingErrands);
                    }
                    if (traveled)
                    {
                        session.automaticErrandLastX = bot->GetPositionX();
                        session.automaticErrandLastY = bot->GetPositionY();
                        session.freeTimeUntil = std::min(session.automaticErrandHardDeadline,
                            now + std::chrono::minutes(5));
                        if (!session.nextAutomaticErrandProgressLog.time_since_epoch().count() ||
                            now >= session.nextAutomaticErrandProgressLog)
                        {
                            session.nextAutomaticErrandProgressLog = now + std::chrono::minutes(1);
                            LogAutomaticErrandEvent(session, bot, "travel_progress", remainingErrands,
                                remainingErrands);
                        }
                    }
                    errandsFinished = remainingErrands == 0;
                    automaticHardTimeout = !errandsFinished && now >= session.automaticErrandHardDeadline;
                    automaticIdleTimeout = !errandsFinished && !automaticHardTimeout &&
                        now >= session.freeTimeUntil;
                    bool firstStopReport = !session.freeTimeRecallRequested;
                    if (errandsFinished && firstStopReport)
                        LogAutomaticErrandEvent(session, bot, "completed", previousErrands, 0);
                    else if (automaticHardTimeout && firstStopReport)
                        LogAutomaticErrandEvent(session, bot, "hard_timeout", previousErrands,
                            remainingErrands);
                    else if (automaticIdleTimeout && firstStopReport)
                        LogAutomaticErrandEvent(session, bot, "idle_timeout", previousErrands,
                            remainingErrands);
                }
                bool freeTimeExpired = !independentErrands && (automaticSettlement ?
                    (automaticHardTimeout || automaticIdleTimeout) : now >= session.freeTimeUntil);
                if ((!independentErrands && human->IsInCombat()) || humanMovedOn || errandsFinished || freeTimeExpired)
                    session.freeTimeRecallRequested = true;
                if (session.freeTimeRecallRequested && !bot->IsInCombat())
                {
                    bool waitingForSummary = false;
                    if (sPlayerbotAIConfig.chatDirectorPartyVerifiedErrands &&
                        sPlayerbotAIConfig.chatDirectorPartyErrandSummaries && !session.errandSummarySent)
                    {
                        // Safety recalls do not wait for social text. Ordinary
                        // completions get one stable, per-bot delay so several
                        // errand runners do not speak in the same world tick.
                        if ((!independentErrands && human->IsInCombat()) || humanMovedOn)
                            session.errandSummarySent = true;
                        else
                        {
                            if (session.errandSummaryReadyAt.time_since_epoch().count() == 0)
                                session.errandSummaryReadyAt = now + std::chrono::milliseconds(
                                    600 + (((session.botGuid ^ 0x9e3779b9u) * 2654435761u) % 1901));
                            if (now < session.errandSummaryReadyAt)
                                waitingForSummary = true;
                            else
                            {
                                session.errandSummarySent = true;
                                bot->GetPlayerbotAI()->SayToParty(session.deferredErrandMask ?
                                    (session.completedErrandMask ?
                                        "I finished what I could. I'll handle the rest later and catch back up now." :
                                        "I couldn't finish that safely right now. I'm catching back up.") :
                                    "I'm done with my errands. I'm catching back up now.", true,
                                    PlayerbotAI::ChatMessageClass::social);
                            }
                        }
                    }
                    std::string returnReason = humanMovedOn ? "free_time_party_moved_on" :
                        errandsFinished ? "automatic_settlement_errands_complete" :
                        automaticHardTimeout ? "automatic_settlement_errands_hard_timeout" :
                        automaticIdleTimeout ? "automatic_settlement_errands_idle_timeout" :
                        "free_time_complete";
                    if (!waitingForSummary)
                        ResumePartyAssist(bot, human, returnReason);
                }
            }
            else if (session.state == "returning")
            {
                long transitSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                    now - session.stateSince).count();
                bool atOrigin = !bot->IsBeingTeleported() &&
                    bot->GetMapId() == session.originMapId &&
                    bot->GetInstanceId() == session.originInstanceId;
                if (atOrigin || transitSeconds >= 45)
                {
                    if (atOrigin)
                    {
                        session.relocated = false;
                        session.reason = "return_worldport_ack_complete";
                    }
                    else
                        session.reason = "return_worldport_ack_timeout";
                    if (ReturnPartyToActivity(session, bot))
                    {
                        LogPartyEvent(session, session.reason == "activity_restored" ?
                            "return_worldport_ack_complete" : "activity_restore_failed");
                        erase = true;
                    }
                }
            }
            else if (session.state == "relocating")
            {
                long transitSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                    now - session.stateSince).count();
                bool sameDestination = human && bot->GetMapId() == human->GetMapId() &&
                    bot->GetInstanceId() == human->GetInstanceId();
                if (!bot->IsBeingTeleported() && sameDestination)
                {
                    session.state = "approaching";
                    session.reason = "worldport_ack_complete";
                    session.stateSince = now;
                    session.approachIssued = true;
                    session.lastHumanDistance = bot->GetDistance(human);
                    session.lastFollowProgress = now;
                    session.nextApproachAttempt = now + std::chrono::seconds(4);
                    bot->GetMotionMaster()->MoveFollow(human, 2.0f, 0.0f, true, false);
                    PersistPartySession(session);
                    LogPartyEvent(session, "relocation_attached");
                }
                else if (transitSeconds >= 45)
                {
                    session.state = "pending";
                    session.reason = "worldport_ack_timeout";
                    session.forceRelocation = true;
                    session.approachIssued = false;
                    session.nextApproachAttempt = now + std::chrono::seconds(5);
                    session.stateSince = now;
                    PersistPartySession(session);
                    LogPartyEvent(session, "relocation_ack_timeout");
                }
            }
            else if (session.state == "instance_handoff")
            {
                // Recovered same-instance parties do not need or permit a
                // relocation. Perform the stale-route cleanup on the world
                // update, then complete the same atomic follow handoff used by
                // an outdoor arrival.
                BeginPartyHandoff(session, bot, human, "same_instance_party_recovered");
            }
            else if (session.state == "pending")
            {
                session.playerGuid = human->GetGUIDLow();
                RecoverStalePartyCombat(session, bot, human);
                // Do not wait for one bot to finish its visible run before
                // starting another. Only serialize the actual teleport calls
                // by one world update so group and movement state are never
                // mutated repeatedly inside the same update iteration.
                if (session.nextApproachAttempt.time_since_epoch().count() == 0 ||
                    now >= session.nextApproachAttempt)
                {
                    // Combat, transit, and destination terrain can make an
                    // arrival temporarily unavailable. Retry without requiring
                    // the bot's remote departure point to be unobserved.
                    session.nextApproachAttempt = now + std::chrono::seconds(5);
                    ++session.approachAttempts;
                    bool started = StartPartyApproach(session, bot, human);
                    if (!started &&
                        (session.approachAttempts == 1 || session.approachAttempts % 6 == 0))
                    {
                        LogPartyEvent(session, "arrival_retry_wait");
                    }
                }
            }
            else if (session.state == "approaching")
            {
                RecoverStalePartyCombat(session, bot, human);
                if (bot->GetMapId() == human->GetMapId() && bot->GetInstanceId() == human->GetInstanceId())
                {
                    const float distance = bot->GetDistance(human);
                    if (bot->IsWithinDistInMap(human, 12.0f))
                    {
                        BeginPartyHandoff(session, bot, human, "arrival_state_reset");
                    }
                    else if (!bot->IsInCombat() && !bot->IsBeingTeleported() &&
                        !bot->IsTaxiFlying() && !bot->GetTransport())
                    {
                        if (session.lastHumanDistance <= 0.0f ||
                            distance + 1.0f < session.lastHumanDistance)
                        {
                            session.lastHumanDistance = distance;
                            session.lastFollowProgress = now;
                        }

                        const long approachSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                            now - session.stateSince).count();
                        const long stalledSeconds = session.lastFollowProgress.time_since_epoch().count() == 0 ?
                            approachSeconds : std::chrono::duration_cast<std::chrono::seconds>(
                                now - session.lastFollowProgress).count();
                        const uint32 maximumSeconds = std::max<uint32>(10,
                            std::min<uint32>(60, sPlayerbotAIConfig.chatDirectorRendezvousMaximumSeconds));

                        if (approachSeconds >= maximumSeconds &&
                            (session.nextApproachAttempt.time_since_epoch().count() == 0 ||
                             now >= session.nextApproachAttempt))
                        {
                            float recoveryX = 0.0f, recoveryY = 0.0f, recoveryZ = 0.0f;
                            session.nextApproachAttempt = now + std::chrono::seconds(5);
                            if (FindPartyRecoveryPoint(bot, human, recoveryX,
                                recoveryY, recoveryZ) && ClaimRelocationSlot())
                            {
                                bot->GetPlayerbotAI()->StopMoving();
                                bot->NearTeleportTo(recoveryX, recoveryY, recoveryZ,
                                    bot->GetAngle(human));
                                session.relocated = true;
                                session.reason = "close_relocation_after_no_progress";
                                QueueActivityTelemetry(session.botGuid, session.playerGuid,
                                    session.groupId, PartyActivityOwner::rendezvous,
                                    PartyActivityPhase::traveling, "arrival_route_recovered",
                                    session.reason);
                                LogPartyEvent(session, "arrival_route_recovered");
                                BeginPartyHandoff(session, bot, human,
                                    "arrival_route_recovered");
                            }
                            else if (session.reason != "arrival_recovery_point_unavailable")
                            {
                                session.reason = "arrival_recovery_point_unavailable";
                                QueueActivityTelemetry(session.botGuid, session.playerGuid,
                                    session.groupId, PartyActivityOwner::rendezvous,
                                    PartyActivityPhase::blocked, "arrival_blocked",
                                    session.reason);
                                PersistPartySession(session);
                            }
                        }
                        else if ((!session.approachIssued || stalledSeconds >= 4) &&
                            (session.nextApproachAttempt.time_since_epoch().count() == 0 ||
                             now >= session.nextApproachAttempt))
                        {
                            // A live follow path can fail even after staging
                            // validation. Reissue it at a bounded cadence and
                            // keep measuring actual distance progress.
                            bot->GetPlayerbotAI()->StopMoving();
                            bot->GetMotionMaster()->MoveFollow(human, 2.0f, 0.0f, true, false);
                            if (session.approachIssued)
                                QueueActivityTelemetry(session.botGuid, session.playerGuid,
                                    session.groupId, PartyActivityOwner::rendezvous,
                                    PartyActivityPhase::traveling, "arrival_follow_reissued",
                                    "route_no_progress");
                            session.approachIssued = true;
                            session.nextApproachAttempt = now + std::chrono::seconds(4);
                        }
                    }
                }
            }
            else if (session.state == "handoff")
            {
                // RequestStrategyReset is applied at the next clean AI tick.
                // Wait for that handoff before installing the authoritative
                // follow movement, so stale travel/RPG generators cannot win
                // the same update and pull the bot away again.
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - session.stateSince).count() >= 1000)
                {
                    bot->GetPlayerbotAI()->SetMaster(human);
                    FollowAction follow(bot->GetPlayerbotAI());
                    Event followEvent("living party arrival");
                    follow.Execute(followEvent);
                    session.state = "active";
                    session.reason = "party_follow_restored";
                    session.stateSince = now;
                    session.postArrivalErrandGraceUntil = now + std::chrono::seconds(
                        sPlayerbotAIConfig.chatDirectorPartyPostArrivalErrandGraceSeconds);
                    session.lastHumanDistance = bot->GetDistance(human);
                    session.lastFollowProgress = now;
                    session.nextFollowRepair = now + std::chrono::seconds(6);
                    PersistPartySession(session);
                    QueueActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                        PartyActivityOwner::party_follow, PartyActivityPhase::completed,
                        "arrival_handoff_completed", "party_follow_restored");
                    LogPartyEvent(session, "arrived");
                }
            }
            else if (session.state == "active")
            {
                // An explicit vendor/travel command can intentionally keep the
                // party session live while its external lease owns movement.
                // Do not let follow repair, catch-up relocation, or settlement
                // maintenance replace that command's validated route.
                if (GetPartyActivityOwner(session.botGuid) != PartyActivityOwner::party_follow)
                {
                    ++iterator;
                    continue;
                }
                bool sameMap = bot->GetMapId() == human->GetMapId() &&
                    bot->GetInstanceId() == human->GetInstanceId();
                float distance = sameMap ? bot->GetDistance(human) : 100000.0f;
                // Use the same formation as normal follow, not a second destination.
                float followError = distance;
                Formation* formation = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<Formation*>("formation")->Get();
                if (sameMap && formation)
                {
                    WorldLocation target = formation->GetLocation();
                    if (!Formation::IsNullLocation(target) && target.mapid == bot->GetMapId())
                        followError = bot->GetDistance(target.coord_x, target.coord_y, target.coord_z);
                }
                float movedX = bot->GetPositionX() - session.followLastX;
                float movedY = bot->GetPositionY() - session.followLastY;
                bool moved = session.followPositionKnown && movedX * movedX + movedY * movedY >= 2.25f;
                session.followLastX = bot->GetPositionX(); session.followLastY = bot->GetPositionY();
                session.followPositionKnown = true;
                if (sameMap && followError <= 12.0f)
                {
                    session.staleCombatSince = std::chrono::steady_clock::time_point();
                    session.lastHumanDistance = followError;
                    session.lastFollowProgress = now;
                    session.nextFollowRepair = now + std::chrono::seconds(6);
                }
                else
                {
                    RecoverStalePartyCombat(session, bot, human);

                    // Loot progress owns a short local detour. Do not accumulate
                    // a catch-up timeout while it is making useful progress.
                    bool looting = YieldPartyFollowToLoot(bot);
                    if (looting) session.lastFollowProgress = now;
                    bool followBlocked = looting || bot->IsInCombat() || bot->IsBeingTeleported() ||
                        bot->IsTaxiFlying() || bot->GetTransport() ||
                        bot->IsNonMeleeSpellCasted(false) || !bot->GetPlayerbotAI()->CanMove();
                    if (!followBlocked)
                    {
                        if (session.lastFollowProgress.time_since_epoch().count() == 0 ||
                            followError + 1.5f < session.lastHumanDistance ||
                            (moved && bot->GetMotionMaster()->GetCurrentMovementGeneratorType() == FOLLOW_MOTION_TYPE &&
                             sServerFacade.GetChaseTarget(bot) == human))
                        {
                            session.lastHumanDistance = followError;
                            session.lastFollowProgress = now;
                        }
                        long stalled = std::chrono::duration_cast<std::chrono::seconds>(
                            now - session.lastFollowProgress).count();
                        if (sameMap && followError > 20.0f && stalled >= 6 &&
                            (session.nextFollowRepair.time_since_epoch().count() == 0 || now >= session.nextFollowRepair))
                        {
                            FollowAction follow(bot->GetPlayerbotAI());
                            Event followEvent("living party follow repair");
                            bool issued = follow.Execute(followEvent);
                            session.nextFollowRepair = now + std::chrono::seconds(6);
                            session.reason = "active_follow_reissued";
                            if (issued) LogPartyEvent(session, "active_follow_repaired");
                        }
                        if ((!sameMap || distance > 70.0f) && stalled >= 18)
                        {
                            session.state = "pending";
                            session.reason = "active_follow_stalled";
                            session.forceRelocation = true;
                            session.approachIssued = false;
                            session.nextApproachAttempt = std::chrono::steady_clock::time_point();
                            session.stateSince = now;
                            PersistPartySession(session);
                            LogPartyEvent(session, "active_follow_relocation_queued");
                        }
                    }
                }
            }
        }
        if (erase) iterator = partySessions.erase(iterator); else ++iterator;
    }
}

void PlayerbotRendezvousManager::LogPartyEvent(const PartySession& session, const char* event) const
{
    sLog.outString("Living WoW party rendezvous event=%s bot=%u player=%u group=%u state=%s relocated=%u prior_activity=%s reason=%s",
        event, session.botGuid, session.playerGuid, session.groupId, session.state.c_str(), session.relocated ? 1 : 0,
        session.previousActivity.c_str(), session.reason.c_str());
}

void PlayerbotRendezvousManager::LogAutomaticErrandEvent(const PartySession& session, Player* bot,
    const char* event, uint32 previousErrands, uint32 remainingErrands) const
{
    uint32 completedErrands = previousErrands & ~remainingErrands;
    sLog.outString("Living WoW party errands event=%s bot=%u player=%u group=%u completed=%s remaining=%s map=%u x=%.1f y=%.1f",
        event, session.botGuid, session.playerGuid, session.groupId,
        ErrandTelemetryNames(completedErrands).c_str(), ErrandTelemetryNames(remainingErrands).c_str(),
        bot ? bot->GetMapId() : 0, bot ? bot->GetPositionX() : 0.0f, bot ? bot->GetPositionY() : 0.0f);
}

void PlayerbotRendezvousManager::QueueActivityTelemetry(uint32 botGuid, uint32 playerGuid,
    uint32 groupId, PartyActivityOwner owner, PartyActivityPhase phase, const std::string& event,
    const std::string& reason, uint32 task, const ErrandObservation* before,
    const ErrandObservation* after)
{
    if (botGuid) activityTelemetryReporter = botGuid;
    activityTelemetry.push_back(BuildActivityTelemetry(botGuid, playerGuid, groupId, owner,
        phase, event, reason, task, before, after));
    while (activityTelemetry.size() > 500)
    {
        activityTelemetry.pop_front();
        ++activityTelemetryDropped;
    }
}

std::string PlayerbotRendezvousManager::BuildActivityTelemetry(uint32 botGuid, uint32 playerGuid,
    uint32 groupId, PartyActivityOwner owner, PartyActivityPhase phase, const std::string& event,
    const std::string& reason, uint32 task, const ErrandObservation* before,
    const ErrandObservation* after, uint32 aggregateCount)
{
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(botGuid);
    if (bot && bot->GetGroup())
    {
        if (!groupId) groupId = bot->GetGroup()->GetId();
        if (!playerGuid)
            if (Player* human = FindPartyHuman(bot)) playerGuid = human->GetGUIDLow();
    }
    Player* player = playerGuid ? sObjectAccessor.FindPlayer(
        ObjectGuid(HIGHGUID_PLAYER, playerGuid)) : nullptr;
    Player* partyScope = bot && bot->GetGroup() && (!groupId || bot->GetGroup()->GetId() == groupId) ?
        bot : (player && player->GetGroup() && (!groupId || player->GetGroup()->GetId() == groupId) ?
            player : nullptr);
    std::string partySessionId;
    if (partyScope)
        partySessionId = GetPartySessionId(partyScope);
    else
    {
        auto retainedParty = partySessions.find(botGuid);
        if (retainedParty != partySessions.end() && !retainedParty->second.partySessionId.empty())
            partySessionId = retainedParty->second.partySessionId;
        else
            partySessionId = groupId ? "party:" + std::to_string(groupId) + ":0" :
                "player:" + std::to_string(playerGuid);
    }
    if (!activitySequence) activitySequence = uint64(time(nullptr)) * 1000000ULL;
    uint64 revision = ++activitySequence;
    bool terminal = phase == PartyActivityPhase::completed || phase == PartyActivityPhase::deferred ||
        phase == PartyActivityPhase::failed;
    std::string outcome = terminal ? ActivityToken(reason.empty() ? event : reason) : "none";
    std::ostringstream json;
    json << "{\"event_id\":\"party-activity-" << botGuid << '-' << time(nullptr) << '-' << revision
         << "\",\"bot_guid\":" << botGuid << ",\"player_guid\":" << playerGuid
         << ",\"party_session_id\":\"" << partySessionId
         << "\",\"state_revision\":" << revision
         << ",\"movement_owner\":\"" << PartyActivityOwnerName(owner)
         << "\",\"phase\":\"" << PartyActivityPhaseName(phase)
         << "\",\"task_type\":\"" << ErrandName(task)
         << "\",\"outcome_code\":\"" << outcome
         << "\",\"suppressed_class\":\"none"
         << "\",\"latency_ms\":0,\"detail\":{\"event\":\""
         << ActivityJsonString(event) << "\",\"group_id\":" << groupId;
    auto partyTask = partySessions.find(botGuid);
    if (task && partyTask != partySessions.end())
    {
        auto record = partyTask->second.errands.find(task);
        if (record != partyTask->second.errands.end())
            json << ",\"task_id\":\"" << ActivityJsonString(record->second.taskId) << '"'
                 << ",\"route_attempts\":" << record->second.routeAttempts
                 << ",\"operation_attempts\":" << record->second.operationAttempts;
    }
    auto appendObservation = [&json](const char* name, const ErrandObservation* value)
    {
        if (!value) return;
        json << ",\"" << name << "\":{\"bag_usage\":" << (uint32)value->bagUsage
             << ",\"durability\":" << (uint32)value->durability
             << ",\"vendor_stacks\":" << value->vendorStacks
             << ",\"bank_stacks\":" << value->bankStacks
             << ",\"auction_stacks\":" << value->auctionStacks
             << ",\"mail_payloads\":" << value->mailPayloads
             << ",\"auction_count\":" << value->auctionCount
             << ",\"profession_skill\":" << value->professionSkill
             << ",\"known_spells\":" << value->knownSpells.size()
             << ",\"inventory_signature\":" << value->inventorySignature << '}';
    };
    appendObservation("before", before);
    appendObservation("after", after);
    if (aggregateCount) json << ",\"count\":" << aggregateCount;
    json << "}}";
    return json.str();
}

void PlayerbotRendezvousManager::RecordSuppressedActivity(Player* bot, const std::string& origin,
    const std::string& suppressionClass, const std::string& actionClass, uint32 count)
{
    if (!bot || !count)
        return;

    std::string safeOrigin = ActivityEnumToken(origin, "autonomous");
    std::string safeClass = ActivityEnumToken(suppressionClass, "operational");
    std::string safeAction = ActivityEnumToken(actionClass, "unknown");
    std::string key = safeOrigin + '|' + safeClass + '|' + safeAction;
    // Keep the world-thread accumulator bounded even if future adapters add
    // parameterized action names. Overflow remains visible as one safe bucket.
    // Thirty-two global buckets per minute also fit beneath the gateway's
    // seven-day 500k event cap at the absolute worst sustained rate.
    if (suppressedActivityAggregates.find(key) == suppressedActivityAggregates.end() &&
        suppressedActivityAggregates.size() >= 31)
    {
        safeOrigin = "autonomous";
        safeClass = "operational";
        safeAction = "other";
        key = "suppression_overflow";
    }

    SuppressedActivityAggregate& aggregate = suppressedActivityAggregates[key];
    aggregate.botGuid = aggregate.botGuid ? aggregate.botGuid : bot->GetGUIDLow();
    aggregate.origin = safeOrigin;
    aggregate.suppressionClass = safeClass;
    aggregate.actionClass = safeAction;
    aggregate.count = std::min<uint32>(
        100000, aggregate.count + std::min<uint32>(100000, count));
}

std::vector<std::string> PlayerbotRendezvousManager::DrainPartyActivityTelemetry(
    bool includeSnapshots, size_t* transitionCount)
{
    const auto now = std::chrono::steady_clock::now();
    if (!nextSuppressionTelemetryFlush.time_since_epoch().count())
        nextSuppressionTelemetryFlush = now + std::chrono::minutes(1);
    const bool flushSuppression = now >= nextSuppressionTelemetryFlush;
    // The director drains ordinary activity every five seconds. Suppression
    // counts deliberately remain coalesced for a full minute, producing at
    // most 32 safe aggregate events per minute across the whole realm.
    if (flushSuppression)
    {
        for (const auto& pair : suppressedActivityAggregates)
        {
            const SuppressedActivityAggregate& aggregate = pair.second;
            Player* bot = sRandomPlayerbotMgr.GetPlayerBot(aggregate.botGuid);
            Player* human = bot ? FindPartyHuman(bot) : nullptr;
            uint32 playerGuid = human ? human->GetGUIDLow() : 0;
            std::string partySessionId = bot ? GetPartySessionId(bot) : "player:0";
            auto retainedParty = partySessions.find(aggregate.botGuid);
            if (retainedParty != partySessions.end())
            {
                if (!playerGuid) playerGuid = retainedParty->second.playerGuid;
                if (!retainedParty->second.partySessionId.empty())
                    partySessionId = retainedParty->second.partySessionId;
            }
            else
            {
                auto rendezvous = sessions.find(aggregate.botGuid);
                if (rendezvous != sessions.end())
                {
                    if (!playerGuid) playerGuid = rendezvous->second.playerGuid;
                    Player* player = playerGuid ? sObjectAccessor.FindPlayer(
                        ObjectGuid(HIGHGUID_PLAYER, playerGuid)) : nullptr;
                    partySessionId = player && (!bot || !bot->GetGroup() ||
                        player->GetGroup() == bot->GetGroup()) ? GetPartySessionId(player) :
                        "player:" + std::to_string(playerGuid);
                }
            }
            PartyActivityOwner owner = GetPartyActivityOwner(aggregate.botGuid);
            PartyActivityPhase phase = GetPartyActivityPhase(aggregate.botGuid);
            std::string suppressedClass = ActivityEnumToken(
                aggregate.suppressionClass + '_' + aggregate.actionClass, "operational_other");
            if (!activitySequence) activitySequence = uint64(time(nullptr)) * 1000000ULL;
            uint64 revision = ++activitySequence;
            std::ostringstream json;
            json << "{\"event_id\":\"party-activity-" << aggregate.botGuid << '-' << time(nullptr)
                 << '-' << revision << "\",\"bot_guid\":" << aggregate.botGuid
                 << ",\"player_guid\":" << playerGuid
                 << ",\"party_session_id\":\"" << partySessionId
                 << "\",\"state_revision\":" << revision
                 << ",\"movement_owner\":\"" << PartyActivityOwnerName(owner)
                 << "\",\"phase\":\"" << PartyActivityPhaseName(phase)
                 << "\",\"task_type\":\"" << aggregate.actionClass
                 << "\",\"outcome_code\":\"suppressed\",\"suppressed_class\":\""
                 << suppressedClass
                 << "\",\"latency_ms\":0,\"detail\":{\"event\":\"chat_suppressed\",\"count\":"
                 << aggregate.count << ",\"origin\":\"" << aggregate.origin
                 << "\",\"action_class\":\"" << aggregate.actionClass << "\"}}";
            activityTelemetryReporter = aggregate.botGuid;
            activityTelemetry.push_back(json.str());
            while (activityTelemetry.size() > 500)
            {
                activityTelemetry.pop_front();
                ++activityTelemetryDropped;
            }
        }
        suppressedActivityAggregates.clear();
        nextSuppressionTelemetryFlush = now + std::chrono::minutes(1);
    }

    std::vector<std::string> result;
    result.reserve(activityTelemetry.size() + (includeSnapshots ?
        partySessions.size() + externalLeases.size() + sessions.size() : 0));
    // Transition delivery always wins. Snapshot generation never enters the
    // bounded transition deque, so a large active party set cannot evict the
    // event that explains how a lease changed.
    while (!activityTelemetry.empty())
    {
        result.push_back(std::move(activityTelemetry.front()));
        activityTelemetry.pop_front();
    }
    // Transport health is itself observable, but the aggregate is bounded to
    // one event per drain rather than producing one row per retried item.
    if (activityTelemetryReporter && activityTelemetryRetried)
    {
        result.push_back(BuildActivityTelemetry(activityTelemetryReporter, 0, 0,
            GetPartyActivityOwner(activityTelemetryReporter),
            GetPartyActivityPhase(activityTelemetryReporter), "telemetry_retried",
            "delivery_retry", 0, nullptr, nullptr, activityTelemetryRetried));
        activityTelemetryRetried = 0;
    }
    if (activityTelemetryReporter && activityTelemetryDropped)
    {
        result.push_back(BuildActivityTelemetry(activityTelemetryReporter, 0, 0,
            GetPartyActivityOwner(activityTelemetryReporter),
            GetPartyActivityPhase(activityTelemetryReporter), "telemetry_dropped",
            "bounded_queue_overflow", 0, nullptr, nullptr, activityTelemetryDropped));
        activityTelemetryDropped = 0;
    }
    if (transitionCount) *transitionCount = result.size();
    if (includeSnapshots)
    {
        for (const auto& pair : partySessions)
        {
            const PartySession& session = pair.second;
            result.push_back(BuildActivityTelemetry(session.botGuid, session.playerGuid, session.groupId,
                GetPartyActivityOwner(session.botGuid), GetPartyActivityPhase(session.botGuid),
                "lease_snapshot", session.reason, session.currentErrand));
        }
        for (const auto& pair : externalLeases)
            if (partySessions.find(pair.first) == partySessions.end() &&
                pair.second.expires > std::chrono::steady_clock::now())
                result.push_back(BuildActivityTelemetry(pair.first, 0, 0,
                    pair.second.owner, pair.second.phase, "lease_snapshot", pair.second.reason));
        for (const auto& pair : sessions)
            if (partySessions.find(pair.first) == partySessions.end() &&
                externalLeases.find(pair.first) == externalLeases.end())
            {
                const Session& session = pair.second;
                result.push_back(BuildActivityTelemetry(session.botGuid, session.playerGuid, 0,
                    GetPartyActivityOwner(session.botGuid), GetPartyActivityPhase(session.botGuid),
                    "lease_snapshot", session.reason));
            }
    }
    return result;
}

void PlayerbotRendezvousManager::RequeuePartyActivityTelemetry(
    const std::vector<std::string>& transitions)
{
    if (transitions.empty()) return;
    activityTelemetryRetried += static_cast<uint32>(transitions.size());
    // Failed in-flight transitions are older than anything queued while the
    // request was running, so restore them at the front in original order.
    for (std::vector<std::string>::const_reverse_iterator event = transitions.rbegin();
         event != transitions.rend(); ++event)
        activityTelemetry.push_front(*event);
    while (activityTelemetry.size() > 500)
    {
        activityTelemetry.pop_back();
        ++activityTelemetryDropped;
    }
}

bool PlayerbotRendezvousManager::IsActive(uint32 botGuid, uint32 playerGuid) const
{
    return Find(botGuid, playerGuid) != nullptr;
}

bool PlayerbotRendezvousManager::WasRelocated(uint32 botGuid, uint32 playerGuid) const
{
    const Session* session = Find(botGuid, playerGuid);
    return session && session->relocated;
}

std::string PlayerbotRendezvousManager::State(uint32 botGuid, uint32 playerGuid) const
{
    const Session* session = Find(botGuid, playerGuid);
    return session ? session->state : "none";
}

void PlayerbotRendezvousManager::LogEvent(const Session& session, const char* event)
{
    sLog.outString("Living WoW rendezvous event=%s action=%s bot=%u player=%u state=%s relocated=%u prior_activity=%s reason=%s",
        event, session.actionId.c_str(), session.botGuid, session.playerGuid, session.state.c_str(),
        session.relocated ? 1 : 0, session.previousActivity.c_str(), session.reason.c_str());
    QueueActivityTelemetry(session.botGuid, session.playerGuid, 0,
        GetPartyActivityOwner(session.botGuid), GetPartyActivityPhase(session.botGuid),
        event, session.reason.empty() ? session.actionId : session.reason);
}
