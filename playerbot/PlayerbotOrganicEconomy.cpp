#include "botpch.h"
#include "PlayerbotOrganicEconomy.h"
#include "LivingServiceExecution.h"
#include "LivingProfessionPlan.h"
#include "PlayerbotInventoryPressure.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"

#include "PlayerbotAI.h"
#include "PlayerbotBuildProfile.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotLLMInterface.h"
#include "PlayerbotRendezvousManager.h"
#include "RandomPlayerbotMgr.h"
#include "ServerFacade.h"
#include "TravelMgr.h"
#include "strategy/ItemVisitors.h"
#include "strategy/values/ItemUsageValue.h"
#include "strategy/values/TravelValues.h"
#include "strategy/actions/BankAction.h"
#include "strategy/actions/AhAction.h"
#include "strategy/actions/MovementActions.h"
#include "Entities/GameObject.h"
#include "Mails/Mail.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace
{
    constexpr uint32 FocusService = 0x80000000u;
    class RecipeServiceMovement : public ai::MovementAction {
    public:
        explicit RecipeServiceMovement(PlayerbotAI* ai) : MovementAction(ai,"recipe service") {}
        bool To(const WorldPosition& position) {
            return MoveTo(position.getMapId(),position.getX(),position.getY(),position.getZ());
        }
    };

    // Immutable world spawns, cached once per requested focus, not per bot tick.
    // Actual availability and range are still checked by native CheckCast.
    const std::vector<WorldPosition>& CraftStations(uint32 focus) {
        static std::map<uint32,std::vector<WorldPosition>> cache;
        auto found=cache.find(focus);if(found!=cache.end()) return found->second;
        auto& points=cache[focus];
        auto rows=WorldDatabase.PQuery("SELECT g.map,g.position_x,g.position_y,g.position_z FROM gameobject g JOIN gameobject_template t ON t.entry=g.id WHERE t.type=%u AND t.data0=%u AND g.map IN (0,1,530) ORDER BY g.guid LIMIT 1024",uint32(GAMEOBJECT_TYPE_SPELL_FOCUS),focus);
        if(rows) do {auto* f=rows->Fetch();points.emplace_back(f[0].GetUInt32(),f[1].GetFloat(),f[2].GetFloat(),f[3].GetFloat());} while(rows->NextRow());
        return points;
    }

    uint32 GoalRecipe(uint32 guid,const std::string& goal) {
        const std::string prefix="profession:"+std::to_string(guid)+":";
        if(goal.compare(0,prefix.size(),prefix)!=0) return 0;
        const std::string suffix=goal.substr(prefix.size());
        if(suffix.empty()||suffix.size()>9||suffix.find_first_not_of("0123456789")!=std::string::npos) return 0;
        return uint32(std::stoul(suffix));
    }
    std::vector<uint32> EconomyWorkOrder(std::vector<uint32> ready, const std::set<uint32>& verifying, uint32 cursor)
    {
        if (ready.empty()) return ready;
        std::rotate(ready.begin(), ready.begin() + cursor % ready.size(), ready.end());
        std::stable_partition(ready.begin(), ready.end(), [&](uint32 guid) { return verifying.count(guid) != 0; });
        if (ready.size() > 8) ready.resize(8);
        return ready;
    }

    bool PreserveCraftResult(const std::string& attemptGoal, const std::string& activeGoal, uint32 started, uint32 now)
    {
        return attemptGoal == activeGoal && now >= started && now - started < 120;
    }

    bool JsonBool(const std::string& source, const std::string& key, bool fallback)
    {
        std::smatch match;
        std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
        return std::regex_search(source, match, pattern) ? match[1].str() == "true" : fallback;
    }

    uint32 JsonUInt(const std::string& source, const std::string& key, uint32 fallback)
    {
        std::smatch match;
        std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*([0-9]+)");
        return std::regex_search(source, match, pattern) ? uint32(std::stoul(match[1].str())) : fallback;
    }

    bool IsCity(uint32 zone)
    {
        static const std::set<uint32> cities = {1497,1519,1537,1637,1638,1657,3487,3557,3703};
        return cities.find(zone) != cities.end();
    }

    std::vector<uint32> KnownCraftOutputs(Player* bot, uint32 limit = 8)
    {
        std::vector<uint32> outputs;
        std::set<uint32> seen;
        for (const auto& spellPair : bot->GetSpellMap())
        {
            SpellEntry const* spell = sServerFacade.LookupSpellInfo(spellPair.first);
            if (!spell || spellPair.second.state == PLAYERSPELL_REMOVED || spellPair.second.disabled)
                continue;
            for (uint32 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
            {
                if (spell->Effect[effect] != SPELL_EFFECT_CREATE_ITEM || !spell->EffectItemType[effect])
                    continue;
                uint32 item = spell->EffectItemType[effect];
                if (sObjectMgr.GetItemPrototype(item) && seen.insert(item).second)
                    outputs.push_back(item);
                if (outputs.size() >= limit)
                    return outputs;
            }
        }
        return outputs;
    }

    bool HasAuctionSurplus(Player* bot)
    {
        ai::ListItemsVisitor inventory;
        bot->GetPlayerbotAI()->InventoryIterateItems(&inventory, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        for (const auto& entry : inventory.items)
        {
            if (entry.second <= 0)
                continue;
            ai::ItemQualifier qualifier(entry.first);
            ai::ItemUsage usage = bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::ItemUsage>(
                "item usage", qualifier.GetQualifier())->Get();
            if (usage == ai::ItemUsage::ITEM_USAGE_AH)
                return true;
        }
        return false;
    }

    // Primary-profession recipes only: knowing Cooking must not turn every
    // career into a cooking goal. Spell/skill and all reagents are real state.
    uint32 CraftSkill(Player* bot, const SpellEntry* spell)
    {
        if (!spell || spell->Effect[0] != SPELL_EFFECT_CREATE_ITEM || !spell->EffectItemType[0]) return 0;
        auto bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spell->Id);
        for (auto it = bounds.first; it != bounds.second; ++it)
        {
            const auto* line = it->second;
            const uint32 value = bot->GetSkillValuePure(line->skillId);
            if (LivingProfessions::Primary(line->skillId) && value &&
                value < bot->GetSkillMaxPure(line->skillId) && value < line->max_value)
                return line->skillId;
        }
        return 0;
    }

    bool SafeCraftReagents(Player* bot, const SpellEntry* spell, bool includeBank = false, bool requireCounts = true)
    {
        ai::FindAllItemVisitor visitor;
        bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BAGS);
        if (includeBank)
            bot->GetPlayerbotAI()->InventoryIterateItems(&visitor, IterateItemsMask::ITERATE_ITEMS_IN_BANK);
        std::map<uint32, uint32> needed;
        for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
            if (spell->Reagent[i] > 0 && spell->ReagentCount[i]) needed[spell->Reagent[i]] += spell->ReagentCount[i];
        for (const auto& reagent : needed)
        {
            // The native spell consumes stacks itself. Reject the whole entry
            // if any stack is promised, rather than risk consuming that stack.
            if (sGuildSupplies.ReservedEntry(bot->GetGUIDLow(), reagent.first) ||
                ai::ItemUsageValue::IsNeededForQuest(bot, reagent.first, true)) return false;
            for (Item* item : visitor.GetResult())
                if (item->GetEntry() == reagent.first &&
                    sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow())) return false;
            if (requireCounts && bot->GetItemCount(reagent.first, includeBank) < reagent.second) return false;
        }
        return !needed.empty(); // No conjuring or reagent cheats.
    }

    uint32 ReadyCraftSpell(Player* bot)
    {
        // Called on the existing ten-minute planning snapshot, not every tick.
        // CanCastSpell validates tools, local forge/anvil, bag space and cooldown.
        for (const auto& known : bot->GetSpellMap())
        {
            if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled) continue;
            const auto* spell = sServerFacade.LookupSpellInfo(known.first);
            if (!CraftSkill(bot, spell) || !SafeCraftReagents(bot, spell)) continue;
            SpellCastResult result=SPELL_CAST_OK;
            if (bot->GetPlayerbotAI()->CanCastSpell(known.first, bot, 0, true, nullptr, false, false, false, &result) ||
                result==SPELL_FAILED_MOVING || result==SPELL_FAILED_NOT_STANDING) return known.first;
            if(result==SPELL_FAILED_REQUIRES_SPELL_FOCUS && spell->RequiresSpellFocus) {
                const WorldPosition here(bot);
                for(const auto& point:CraftStations(spell->RequiresSpellFocus))
                    if(point.getMapId()==bot->GetMapId() && here.distance(point)<=600) return known.first;
            }
        }
        return 0;
    }

    uint32 BankCraftSpell(Player* bot)
    {
        // Only propose a bank trip when ALL ingredients already exist in this
        // character's real possessions. No speculative shopping or remote craft.
        for (const auto& known : bot->GetSpellMap())
        {
            if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled) continue;
            const auto* spell = sServerFacade.LookupSpellInfo(known.first);
            if (CraftSkill(bot, spell) && !SafeCraftReagents(bot, spell) &&
                SafeCraftReagents(bot, spell, true)) return known.first;
        }
        return 0;
    }

    uint32 MarketCraftSpell(Player* bot)
    {
        // Use real cached listings, not hypothetical market stock. At most two
        // missing reagent types per job; larger gathering chains are separate.
        for (const auto& known : bot->GetSpellMap())
        {
            if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled) continue;
            const auto* spell = sServerFacade.LookupSpellInfo(known.first);
            if (!CraftSkill(bot, spell) || !SafeCraftReagents(bot, spell, true, false)) continue;
            std::map<uint32,uint32> needed;
            for (uint32 i=0;i<MAX_SPELL_REAGENTS;++i)
                if (spell->Reagent[i]>0 && spell->ReagentCount[i]) needed[spell->Reagent[i]]+=spell->ReagentCount[i];
            uint32 missing=0;bool obtainable=true;
            for (const auto& reagent:needed)
            {
                const uint32 owned=bot->GetItemCount(reagent.first,true);
                if (owned>=reagent.second) continue;
                if (++missing>2 || (!ai::AhBidAction::HasPendingMaterial(bot,reagent.first) &&
                    !ai::AhBidAction::HasMaterialOffer(bot,reagent.first,reagent.second-owned)))
                { obtainable=false;break; }
            }
            if (obtainable && missing) return known.first;
        }
        return 0;
    }

    uint32 PendingRecipeSpell(Player* bot)
    {
        // Native mailbox state survives replanning/restarts. Prioritize paid
        // attachments even if a different ingredient's auction has disappeared.
        for(const auto& known:bot->GetSpellMap()) {
            if(known.second.state==PLAYERSPELL_REMOVED || known.second.disabled) continue;
            const auto* spell=sServerFacade.LookupSpellInfo(known.first);
            if(!spell || !spell->EffectItemType[0]) continue;
            for(uint32 i=0;i<MAX_SPELL_REAGENTS;++i)
                if(spell->Reagent[i]>0 && spell->ReagentCount[i] && ai::AhBidAction::HasPendingMaterial(bot,spell->Reagent[i]))
                    return known.first;
        }
        return 0;
    }
}

PlayerbotOrganicEconomy& PlayerbotOrganicEconomy::instance()
{
    static PlayerbotOrganicEconomy economy;
    return economy;
}

bool PlayerbotOrganicEconomy::CanLearnProfessionSpell(Player* bot, uint32 learnedSpell) const
{
    if (!bot || bot->isRealPlayer() || !bot->GetPlayerbotAI()) return true;
    const SpellLearnSkillNode* node = sSpellMgr.GetSpellLearnSkill(learnedSpell);
    if (!node || !LivingProfessions::Primary(node->skill) || bot->GetSkillValue(node->skill)) return true;
    auto row = profiles.find(bot->GetGUIDLow());
    // The first population snapshot may not have loaded yet. Defer only new
    // primary professions until that happens; existing skills remain trainable.
    if (row == profiles.end()) return policy.mode == "off";
    const Profile& p = row->second;
    unsigned learnedCount = 0;
    for (unsigned skill : LivingProfessions::Skills)
        if (bot->GetSkillValue(skill)) ++learnedCount;
    return LivingProfessions::Allows(false, p.career, p.planVersion,
        p.intendedOne, p.intendedTwo, node->skill, false, learnedCount);
}

PlayerbotOrganicEconomy::Policy PlayerbotOrganicEconomy::LoadPolicy()
{
    Policy result;
    std::ifstream input("/srv/living-wow/config/economy.json");
    if (!input)
        return result;
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    std::smatch mode;
    if (std::regex_search(source, mode, std::regex("\\\"mode\\\"\\s*:\\s*\\\"(off|observe|active)\\\"")))
        result.mode = mode[1].str();
    result.careers = JsonBool(source, "careerGoals", false);
    result.posting = JsonBool(source, "characterAuctionPosting", false);
    result.buying = JsonBool(source, "characterAuctionBuying", false);
    result.commissions = JsonBool(source, "craftingCommissions", false);
    // The feature flag is the execution gate. The validated launcher keeps
    // advertising.enabled synchronized, but matching a bare "enabled" key
    // here would accidentally read the commissions section first.
    result.advertising = JsonBool(source, "professionAdvertising", false);
    result.cadenceSeconds = std::max<uint32>(60, std::min<uint32>(3600, JsonUInt(source, "cadenceSeconds", 600)));
    result.botAdCooldownSeconds = std::max<uint32>(60, JsonUInt(source, "botCooldownSeconds", 3600));
    result.channelAdCooldownSeconds = std::max<uint32>(60, JsonUInt(source, "channelCooldownSeconds", 600));
    return result;
}

std::map<uint32, PlayerbotOrganicEconomy::Profile> PlayerbotOrganicEconomy::LoadProfiles()
{
    std::map<uint32, Profile> profiles;
    std::unique_ptr<QueryResult> result = CharacterDatabase.Query(
        "SELECT profile.character_guid,profile.career_participant,COALESCE(profile.intended_profession_one,0),"
        "COALESCE(profile.intended_profession_two,0),COALESCE(goal.capability_ref,''),"
        "COALESCE(goal.goal_type,''),COALESCE(goal.state,''),profile.profession_plan_version,actor.race,"
        "COALESCE(UNIX_TIMESTAMP(goal.created_at),0),COALESCE(JSON_EXTRACT(goal.authoritative_payload,'$.paid_materials_until'),0) FROM organic_economy_profile profile "
        "JOIN characters actor ON actor.guid=profile.character_guid "
        "LEFT JOIN organic_economy_goal goal ON goal.goal_id=(SELECT MAX(candidate.goal_id) FROM organic_economy_goal candidate "
        "WHERE candidate.character_guid=profile.character_guid AND candidate.state IN ('active','proposed','candidate') "
        "AND (candidate.expires_at IS NULL OR candidate.expires_at>NOW()))");
    if (result)
    {
        do
        {
            Field* fields = result->Fetch();
            Profile profile;
            profile.career = fields[1].GetBool();
            profile.planVersion = fields[7].GetUInt32();
            profile.race = fields[8].GetUInt32();
            profile.createdAt = fields[9].GetUInt32();profile.committedUntil = fields[10].GetUInt32();
            profile.intendedOne = fields[2].GetUInt32();
            profile.intendedTwo = fields[3].GetUInt32();
            profile.currentGoalId = fields[4].GetString();
            profile.currentGoalType = fields[5].GetString();
            profile.currentGoalState = fields[6].GetString();
            profiles[fields[0].GetUInt32()] = profile;
        } while (result->NextRow());
    }

    LivingProfessions::Counts coverage[2] = {};
    unsigned population[2] = {};
    for (const auto& row : profiles)
    {
        const Profile& p = row.second;
        if (!p.career) continue;
        unsigned faction = LivingProfessions::Faction(p.race);
        ++population[faction];
        LivingProfessions::Add(coverage[faction], p.intendedOne);
        if (p.intendedTwo != p.intendedOne) LivingProfessions::Add(coverage[faction], p.intendedTwo);
    }

    // The schema migration seeds profiles for bots that exist at install time,
    // but the configured population may grow later. Enrol newly created live
    // random bots here so economy participation scales with the population.
    // Existing profiles and learned professions are never rewritten.
    for (uint32 guid : sRandomPlayerbotMgr.GetChatBotGuids())
    {
        if (profiles.find(guid) != profiles.end())
            continue;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        if (!bot || !bot->GetSession())
            continue;

        uint32 account = bot->GetSession()->GetAccountId();
        uint64 seed = uint64(guid) * 1103515245ULL + uint64(account) * 12345ULL;
        Profile profile;
        profile.career = (seed % 100ULL) < 80ULL;
        profile.race = bot->getRace();
        profile.planVersion = 1;
        unsigned faction = LivingProfessions::Faction(profile.race);
        std::vector<unsigned> learned;
        for (unsigned skill : LivingProfessions::Skills)
            if (bot->GetSkillValue(skill)) learned.push_back(skill);
        auto planned = LivingProfessions::Choose(bot->getClass(), LivingProfessions::Mix(seed),
            learned, coverage[faction], population[faction] + 1);
        profile.intendedOne = planned.first;
        profile.intendedTwo = planned.second;
        if (profile.career)
        {
            ++population[faction];
            LivingProfessions::Add(coverage[faction], planned.first);
            LivingProfessions::Add(coverage[faction], planned.second);
        }
        uint32 generosity = uint32((uint64(guid) * 1664525ULL + 1013904223ULL) % 101ULL);
        uint32 thrift = uint32((uint64(guid) * 22695477ULL + 1ULL) % 101ULL);
        uint32 patience = uint32((uint64(guid) * 214013ULL + 2531011ULL) % 101ULL);
        uint32 risk = uint32((uint64(guid) * 134775813ULL + 1ULL) % 101ULL);
        CharacterDatabase.PExecute(
            "INSERT IGNORE INTO organic_economy_profile "
            "(character_guid,account_id,career_participant,intended_profession_one,intended_profession_two,"
            "generosity,thrift,bargaining_patience,risk_tolerance,profession_plan_version) VALUES (%u,%u,%u,%u,%u,%u,%u,%u,%u,1)",
            guid, account, profile.career ? 1 : 0, profile.intendedOne, profile.intendedTwo,
            generosity, thrift, patience, risk);
        profiles[guid] = profile;
    }
    return profiles;
}

std::string PlayerbotOrganicEconomy::CurrentGoalType(uint32 characterGuid) const
{
    auto found = profiles.find(characterGuid);
    return found == profiles.end() ? "" : found->second.currentGoalType;
}

uint32 PlayerbotOrganicEconomy::RecipeMaterialQuantity(uint32 guid, uint32 entry) const
{
    auto found = profiles.find(guid);
    if (policy.mode != "active" || !policy.careers || found == profiles.end() ||
        found->second.currentGoalState != "active" || found->second.currentGoalType != "profession_skill_up") return 0;
    const std::string prefix = "profession:" + std::to_string(guid) + ":";
    const auto& id = found->second.currentGoalId;
    if (id.compare(0, prefix.size(), prefix)) return 0;
    const std::string suffix = id.substr(prefix.size());
    if (suffix.empty() || suffix.size() > 9 || suffix.find_first_not_of("0123456789") != std::string::npos) return 0;
    const auto* spell = sServerFacade.LookupSpellInfo(uint32(std::stoul(suffix)));
    if (!spell) return 0;
    uint32 count = 0;
    for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
        if (spell->Reagent[i] > 0 && uint32(spell->Reagent[i]) == entry) count += spell->ReagentCount[i];
    return count;
}

bool PlayerbotOrganicEconomy::SafeForEconomy(Player* bot) const
{
    if (!bot || !bot->IsInWorld() || !bot->IsAlive() || bot->IsInCombat() || bot->InBattleGround() ||
        bot->IsTaxiFlying() || bot->GetTransport() || bot->IsBeingTeleported() ||
        !bot->GetMap() || bot->GetMap()->IsDungeon())
        return false;
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    bool partyFreeTime = sPlayerbotRendezvousManager.IsPartyFreeTime(bot->GetGUIDLow());
    if (!ai || (sPlayerbotRendezvousManager.BlocksAutonomousPartyWork(bot->GetGUIDLow()) &&
        sPlayerbotRendezvousManager.GetPartyActivityOwner(bot->GetGUIDLow()) !=
            PlayerbotRendezvousManager::PartyActivityOwner::economy_service) ||
        (ai->GetMaster() && (ai->GetMaster()->isRealPlayer() || !ai->GetMaster()->GetPlayerbotAI()) && !partyFreeTime))
        return false;
    Group* group = bot->GetGroup();
    if (group)
    {
        if(!partyFreeTime) for(const auto& slot:group->GetMemberSlots())
            if(!sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(slot.guid))) return false;
        for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        {
            Player* member = reference->getSource();
            if (member && !member->GetPlayerbotAI() && !partyFreeTime)
                return false;
        }
    }
    return true;
}

bool PlayerbotOrganicEconomy::AllowsServiceAction(uint32 guid, const std::string& action) const
{
    const auto found=serviceTrips.find(guid);
    if(found==serviceTrips.end() || sPlayerbotRendezvousManager.GetPartyActivityOwner(guid)!=
        PlayerbotRendezvousManager::PartyActivityOwner::economy_service) return true;
    const auto& trip=found->second;
    if(LivingServiceExecution::DisruptiveMaintenance(action)) return false;
    if(action.find("request travel target")==0) return trip.requesting;
    if(action=="choose travel target") return !trip.local;
    if(action=="travel" || action=="move to travel target") {
        Player* bot=sRandomPlayerbotMgr.GetPlayerBot(guid);
        auto* target=bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
        return !trip.local && target && target->GetDestination() &&
            uint32(target->GetDestination()->GetPurpose())==trip.purpose;
    }
    // Only the owning job may choose/replace a route. Noncombat buff chasing
    // also replaces point movement; casting a nearby heal remains available.
    return action.find("travel")==std::string::npos && action.find("rpg")==std::string::npos &&
        action.find("follow")==std::string::npos && action.find("reach spell")==std::string::npos &&
        action.find("move random")==std::string::npos && action.find("grind")==std::string::npos && action!="go";
}

void PlayerbotOrganicEconomy::ReleaseRecipeService(uint32 guid, const std::string& reason)
{
    auto found=serviceTrips.find(guid);if(found==serviceTrips.end()) return;
    using Owner=PlayerbotRendezvousManager::PartyActivityOwner;
    using Phase=PlayerbotRendezvousManager::PartyActivityPhase;
    Player* bot=sRandomPlayerbotMgr.GetPlayerBot(guid);
    const auto lease = found->second.lease;
    if(bot && bot->IsInWorld() && bot->IsAlive() && !bot->IsInCombat() && !bot->IsBeingTeleported() &&
        sPlayerbotRendezvousManager.HasPartyActivityLease(lease) &&
        sPlayerbotRendezvousManager.GetPartyActivityOwner(guid)==Owner::economy_service) {
        auto* target=bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
        const bool preparing=target && target->GetStatus()==ai::TravelStatus::TRAVEL_STATUS_PREPARE &&
            bot->GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::string>("manual string","future travel purpose")->Get()==std::to_string(found->second.purpose);
        if(target && (preparing || (target->GetDestination() && uint32(target->GetDestination()->GetPurpose())==found->second.purpose)))
            target->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
        bot->StopMoving();bot->GetMotionMaster()->MoveIdle();
    }
    serviceTrips.erase(found);
    sPlayerbotRendezvousManager.ReleasePartyActivityLease(lease,Phase::deferred,reason);
}

void PlayerbotOrganicEconomy::PauseRecipeService(uint32 guid,const std::string& reason)
{
    auto found=serviceTrips.find(guid);if(found==serviceTrips.end()) return;
    const uint64 stamp=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    found->second.work.Observe(stamp,false);
    // Never clear movement on a safety pause or a newer owner's route. The
    // exact old handle may release authority, but not erase the accepted job.
    sPlayerbotRendezvousManager.ReleasePartyActivityLease(found->second.lease,
        PlayerbotRendezvousManager::PartyActivityPhase::deferred,reason);
    found->second.lease={};
    found->second.nextMove=0;
}

void PlayerbotOrganicEconomy::ReachRecipeService(Player* bot,uint32 purpose,const std::string& goal,std::string& blocker)
{
    using Owner=PlayerbotRendezvousManager::PartyActivityOwner;
    using Phase=PlayerbotRendezvousManager::PartyActivityPhase;
    const uint32 guid=bot->GetGUIDLow(), now=uint32(time(nullptr));
    if(!SafeForEconomy(bot)) {PauseRecipeService(guid,"recipe_service_safety_pause");blocker="recipe_service_safety_pause";return;}
    if(!LivingServiceExecution::Prepare(bot)) {PauseRecipeService(guid,"recipe_service_preparation_wait");blocker=LivingServiceExecution::Blocker(bot);return;}
    if(serviceRetry[guid]>now) {blocker="recipe_service_retry_wait";return;}
    auto* ai=bot->GetPlayerbotAI();auto* context=ai->GetAiObjectContext();
    auto* target=context->GetValue<ai::TravelTarget*>("travel target")->Get();
    auto old=serviceTrips.find(guid);
    if(old!=serviceTrips.end() && (old->second.goal!=goal || old->second.purpose!=purpose))
        ReleaseRecipeService(guid,"recipe_service_step_changed");
    if(!serviceTrips.count(guid)) {
        if(target && (target->IsForced() || target->IsGroupCopy())) {blocker="recipe_waiting_for_committed_route";return;}
        ServiceTrip trip;trip.goal=goal;trip.purpose=purpose;trip.started=trip.progress=now;
        serviceTrips.emplace(guid,trip);
    }
    auto& trip=serviceTrips.at(guid);
    const uint64 stamp=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    size_t running=0;for(const auto& entry:serviceTrips) if(entry.second.work.Running()) ++running;
    if(!trip.work.Running() && running>=4) {
        trip.work.Observe(stamp,false);blocker="recipe_service_queue_wait";return;
    }
    const auto acquisition=sPlayerbotRendezvousManager.AcquirePartyActivityLease(guid,Owner::economy_service,Phase::traveling,45,
        "recipe_service_trip",goal+":"+std::to_string(purpose),trip.lease);
    if(!acquisition.Permitted()) {
        trip.work.Observe(stamp,false);
        if(acquisition.Waiting()) {
            blocker=acquisition.blocker;
            return; // Admission waiting is not route failure or cancellation.
        }
        ReleaseRecipeService(guid,"recipe_service_preempted");blocker="recipe_service_preempted";return;
    }
    trip.work.Observe(stamp,true);
    if(trip.work.ActiveMs()>=600000) {
        ReleaseRecipeService(guid,"recipe_service_deadline");serviceRetry[guid]=now+300;
        blocker="recipe_service_deadline";return;
    }
    // Generic RPG destinations consider the neighbourhood an arrival. Finish
    // the last metres against an actual service, not the RPG work/idle loop.
    WorldObject* service=nullptr;float distance=1e30f;
    const bool focus=(purpose&FocusService)!=0;
    const bool mail=purpose==uint32(ai::TravelDestinationPurpose::Mail);
    const uint32 flag=purpose==uint32(ai::TravelDestinationPurpose::Bank)?UNIT_NPC_FLAG_BANKER:
        purpose==uint32(ai::TravelDestinationPurpose::Vendor)?UNIT_NPC_FLAG_VENDOR:UNIT_NPC_FLAG_AUCTIONEER;
    for(auto id:context->GetValue<std::list<ObjectGuid>>((mail||focus)?"nearest game objects no los":"nearest npcs no los")->Get()) {
        WorldObject* candidate=nullptr;
        if(mail||focus) {auto* go=ai->GetGameObject(id);if(go && (mail?go->GetGoType()==GAMEOBJECT_TYPE_MAILBOX:
            go->GetGoType()==GAMEOBJECT_TYPE_SPELL_FOCUS && go->GetGOInfo()->spellFocus.focusId==(purpose&~FocusService))) candidate=go;}
        else {auto* npc=ai->GetUnit(id);if(npc && npc->HasFlag(UNIT_NPC_FLAGS,flag) && !sServerFacade.IsHostileTo(npc,bot)) candidate=npc;}
        if(candidate && candidate->GetMap()==bot->GetMap() && bot->GetDistance(candidate)<distance) {
            service=candidate;distance=bot->GetDistance(candidate);
        }
    }
    trip.local=focus||service!=nullptr;
    if(service) {
        if(distance+1<trip.distance) {trip.distance=distance;trip.progress=now;trip.work.Progress();}
        if(now>=trip.nextMove) {
            trip.nextMove=now+5;
            float x=service->GetPositionX(),y=service->GetPositionY(),z=service->GetPositionZ();
            if(bot->GetMap()->GetReachableRandomPointOnGround(x,y,z,1.0f,false))
                bot->GetMotionMaster()->MovePoint(240,x,y,z);
        }
        blocker="recipe_approaching_service";
    } else if(focus) {
        const WorldPosition here(bot);const WorldPosition* closest=nullptr;
        for(const auto& point:CraftStations(purpose&~FocusService)) {
            if(point.getMapId()!=bot->GetMapId()) continue;
            const float remaining=here.distance(point);
            if(remaining<=600 && remaining<distance) {closest=&point;distance=remaining;}
        }
        if(!closest) {ReleaseRecipeService(guid,"recipe_station_unavailable_locally");serviceRetry[guid]=now+300;blocker="recipe_station_unavailable_locally";return;}
        if(distance+1<trip.distance) {trip.distance=distance;trip.progress=now;trip.work.Progress();}
        if(now>=trip.nextMove) {trip.nextMove=now+5;RecipeServiceMovement movement(ai);movement.To(*closest);}
        blocker="recipe_traveling_to_crafting_station";
    } else {
        bool same=target && target->GetDestination() && uint32(target->GetDestination()->GetPurpose())==purpose && target->IsActive();
        if(same && target->GetPosition() && target->GetPosition()->getMapId()==bot->GetMapId()) {
            const float remaining=target->Distance(bot);
            if(remaining+2<trip.distance) {trip.distance=remaining;trip.progress=now;trip.work.Progress();}
        }
        if(!same && target && target->GetStatus()!=ai::TravelStatus::TRAVEL_STATUS_PREPARE && now>=trip.nextMove) {
            trip.nextMove=now+15;
            target->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
            context->ClearValues("travel target active");context->ClearValues("no active travel destinations");
            trip.requesting=true;
            const bool requested=ai->DoSpecificAction("request travel target::"+std::to_string(purpose),Event("can move around","",bot),true);
            trip.requesting=false;
            blocker=requested?"recipe_service_route_requested":"recipe_service_route_pending";
        } else {
            blocker="recipe_traveling_to_service";
            if(same && now>=trip.nextMove) {
                trip.nextMove=now+5;
                // The normal travel action can sit below incidental RPG work
                // in the action queue. Execute its existing guarded movement
                // step for this exact owned route, without a new movement path.
                if(!ai->DoSpecificAction("move to travel target",Event("recipe_service","",bot),true))
                    blocker="recipe_service_movement_pending";
            }
        }
    }
    if(trip.work.NoProgressMs()>=90000) {
        if(++trip.attempts>=2) {
            ReleaseRecipeService(guid,"recipe_service_no_progress");serviceRetry[guid]=now+300;
            blocker="recipe_service_no_progress";return;
        }
        trip.progress=now;trip.work.Progress();trip.distance=1e30f;trip.nextMove=0;
        if(target && !target->IsForced()) target->SetStatus(ai::TravelStatus::TRAVEL_STATUS_EXPIRED);
    }
}

bool PlayerbotOrganicEconomy::PrepareRecipeMail(Player* bot,uint32 entry,const std::string& goal,std::string& blocker)
{
    for(auto it=bot->GetMailBegin();it!=bot->GetMailEnd();++it) {
        auto* mail=*it;
        if(!mail || mail->state==MAIL_STATE_DELETED || mail->COD || mail->expire_time<=time(nullptr) || mail->deliver_time>time(nullptr)) continue;
        for(const auto& attachment:mail->items) {
            if(attachment.item_template!=entry) continue;
            auto* item=bot->GetMItem(attachment.item_guid);if(!item) continue;
            ItemPosCountVec positions;
            if(bot->CanStoreNewItem(NULL_BAG,NULL_SLOT,positions,entry,item->GetCount())==EQUIP_ERR_OK) return true;
            const uint32 guid=bot->GetGUIDLow();
            if(serviceRetry[guid]>uint32(time(nullptr))) {blocker="recipe_service_retry_wait";return false;}
            const auto pressure=sPlayerbotInventoryPressure.Analyze(bot);
            if(mailPrepAttempts[guid]>=4 || (!pressure.vendorStacks && !pressure.HasBankableStorage())) {
                ReleaseRecipeService(guid,"recipe_bags_full_no_safe_storage");
                serviceRetry[guid]=uint32(time(nullptr))+300;mailPrepAttempts[guid]=0;
                blocker="recipe_bags_full_no_safe_storage";return false;
            }
            const bool vendor=pressure.vendorStacks && mailPrepAttempts[guid]<2;
            const uint32 flag=vendor?UNIT_NPC_FLAG_VENDOR:UNIT_NPC_FLAG_BANKER;
            auto* ai=bot->GetPlayerbotAI();bool nearby=false;
            for(auto id:ai->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>("nearest npcs no los")->Get())
                if(bot->GetNPCIfCanInteractWith(id,flag)) {nearby=true;break;}
            if(!nearby) ReachRecipeService(bot,uint32(vendor?ai::TravelDestinationPurpose::Vendor:ai::TravelDestinationPurpose::Bank),goal,blocker);
            else {
                ++mailPrepAttempts[guid];
                // Existing native safe maintenance preserves reserved recipe,
                // quest, trade and guild items. Recheck capacity next sweep.
                ai->DoSpecificAction(vendor?"sell":"bank",Event("rpg action",vendor?"living-wow-safe-vendor":"living-wow-safe-storage",nullptr),true);
                ReleaseRecipeService(guid,"recipe_mail_capacity_recheck");
                blocker="recipe_mail_capacity_recheck";
            }
            return false;
        }
    }
    return true;
}

bool PlayerbotOrganicEconomy::Submit(const Policy& currentPolicy)
{
    profiles = LoadProfiles();
    std::ostringstream events, plans;
    events << "{\"events\":[";
    plans << "{\"bots\":[";
    bool firstEvent = true, firstBot = true;
    uint32 reportedBots = 0;
    uint32 now = uint32(time(nullptr));
    // GetPlayers() contains only the legacy manager-owned subset after the
    // Linux migration. ChatBotGuids is the same live-bot registry used by
    // health telemetry and therefore includes the autonomous population.
    for (uint32 guid : sRandomPlayerbotMgr.GetChatBotGuids())
    {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        // The persistent profile table is the authoritative allow-list. It is
        // populated only for RNDBOT accounts by the migration, whereas the
        // legacy numeric random-account range can be stale after importing an
        // existing realm and would incorrectly exclude every live bot.
        if (!bot || !bot->IsInWorld() || !bot->GetPlayerbotAI())
            continue;
        auto profileIt = profiles.find(guid);
        if (profileIt == profiles.end())
            continue;
        ++reportedBots;
        Profile profile = profileIt->second;
        const uint32 professionIds[] = {164,165,171,182,186,197,202,333,393,755};
        uint32 professionOne = 0, professionTwo = 0, skillOne = 0, skillTwo = 0;
        for (uint32 skillId : professionIds)
        {
            uint32 value = bot->GetSkillValue(skillId);
            if (!value) continue;
            if (!professionOne) { professionOne = skillId; skillOne = value; }
            else if (!professionTwo) { professionTwo = skillId; skillTwo = value; break; }
        }
        std::vector<uint32> outputs = KnownCraftOutputs(bot);
        uint32 readyRecipe = profile.career && currentPolicy.careers ? PendingRecipeSpell(bot) : 0;
        if (!readyRecipe && profile.career && currentPolicy.careers) readyRecipe = ReadyCraftSpell(bot);
        if (!readyRecipe && profile.career && currentPolicy.careers) readyRecipe = BankCraftSpell(bot);
        if (!readyRecipe && profile.career && currentPolicy.careers && currentPolicy.buying) readyRecipe = MarketCraftSpell(bot);
        bool surplus = HasAuctionSurplus(bot);
        if (!firstEvent) events << ',';
        firstEvent = false;
        events << "{\"event_id\":\"profile-" << guid << '-' << now
            << "\",\"type\":\"profile_snapshot\",\"character_guid\":" << guid
            << ",\"character_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(bot->GetName())
            << "\",\"account_id\":" << bot->GetSession()->GetAccountId()
            << ",\"career_participant\":" << (profile.career ? "true" : "false")
            << ",\"intended_profession_one\":" << profile.intendedOne
            << ",\"intended_profession_two\":" << profile.intendedTwo
            << ",\"profession_one\":" << professionOne << ",\"profession_two\":" << professionTwo
            << ",\"profession_one_skill\":" << skillOne << ",\"profession_two_skill\":" << skillTwo
            << ",\"current_goal_id\":\"" << PlayerbotLLMInterface::SanitizeForJson(profile.currentGoalId)
            << "\",\"current_goal_type\":\"" << PlayerbotLLMInterface::SanitizeForJson(profile.currentGoalType)
            << "\",\"current_goal_state\":\"" << PlayerbotLLMInterface::SanitizeForJson(profile.currentGoalState)
            << "\""
            << ",\"known_recipe_outputs\":[";
        for (size_t i = 0; i < outputs.size(); ++i) { if (i) events << ','; events << outputs[i]; }
        events << "],\"auction_surplus\":" << (surplus ? "true" : "false") << '}';
        const bool gearingNeed = sPlayerbotBuildProfiles.GearingGoalsEnabled() &&
            sPlayerbotBuildProfiles.HasGearingDeficiency(bot);
        if ((!profile.career && !gearingNeed) || currentPolicy.mode == "off")
            continue;
        if (!firstBot) plans << ',';
        firstBot = false;
        plans << "{\"character_guid\":" << guid << ",\"candidate_goals\":[";
        bool firstGoal = true;
        if (gearingNeed)
        {
            plans << "{\"goal_id\":\"gear:" << guid
                << "\",\"type\":\"equipment_upgrade\",\"utility\":"
                << sPlayerbotBuildProfiles.GearingGoalUtility(bot)
                << ",\"eligible\":true,\"duration_seconds\":3600}";
            firstGoal = false;
        }
        if (profile.career)
        {
            if (!firstGoal) plans << ',';
            plans << "{\"goal_id\":\"supplies:" << guid
                << "\",\"type\":\"maintain_supplies\",\"utility\":10,\"eligible\":true,\"duration_seconds\":3600}";
            firstGoal = false;
        }
        if (readyRecipe)
            plans << ",{\"goal_id\":\"profession:" << guid << ':' << readyRecipe
                << "\",\"type\":\"profession_skill_up\",\"utility\":70,\"eligible\":true,\"duration_seconds\":1800}";
        if (profile.career && profile.currentGoalType == "storage_pressure")
            plans << ",{\"goal_id\":\"storage:" << guid
                << "\",\"type\":\"storage_pressure\",\"utility\":100,\"eligible\":true,\"duration_seconds\":1800}";
        if (profile.career && surplus)
            plans << ",{\"goal_id\":\"auction:" << guid
                << "\",\"type\":\"list_surplus\",\"utility\":20,\"eligible\":true,\"duration_seconds\":3600}";
        if (profile.career && currentPolicy.advertising && IsCity(bot->GetZoneId()) && !outputs.empty())
            plans << ",{\"goal_id\":\"advertise:" << guid << ':' << outputs.front()
                << "\",\"type\":\"profession_advertisement\",\"utility\":4,\"eligible\":true,\"duration_seconds\":1800}";
        plans << "]}";
    }
    events << "]}"; plans << "]}";

    // Mirror the real character-owned auction table for Admin observability.
    // Keep this separate from the bot profile batch so population growth cannot
    // truncate listings or their authoritative reconciliation marker.
    std::ostringstream auctionEvents, activeAuctionIds;
    auctionEvents << "{\"events\":[";
    activeAuctionIds << '[';
    bool firstAuction = true, auctionSnapshotTruncated = false;
    uint32 emittedAuctions = 0;
    auto auctions = CharacterDatabase.PQuery(
        "SELECT a.id,a.houseid,a.itemowner,c.name,a.item_template,a.item_count,"
        "a.startbid,a.buyoutprice,a.time FROM auction a LEFT JOIN characters c "
        "ON c.guid=a.itemowner WHERE a.itemowner<>0 ORDER BY a.id LIMIT 2001");
    if (auctions)
    {
        do
        {
            if (emittedAuctions >= 2000)
            {
                auctionSnapshotTruncated = true;
                break;
            }
            Field* fields = auctions->Fetch();
            const uint32 auctionId = fields[0].GetUInt32();
            const uint32 sellerGuid = fields[2].GetUInt32();
            const uint32 itemEntry = fields[4].GetUInt32();
            ItemPrototype const* item = sObjectMgr.GetItemPrototype(itemEntry);
            if (!auctionId || !sellerGuid || !item)
                continue;
            if (!firstAuction) { auctionEvents << ','; activeAuctionIds << ','; }
            firstAuction = false;
            ++emittedAuctions;
            activeAuctionIds << auctionId;
            auctionEvents << "{\"event_id\":\"auction-" << auctionId << '-' << now
                << "\",\"type\":\"auction_snapshot\",\"auction_id\":" << auctionId
                << ",\"auction_house_id\":" << fields[1].GetUInt32()
                << ",\"seller_guid\":" << sellerGuid
                << ",\"seller_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(fields[3].GetString())
                << "\",\"seller_type\":\"" << (profiles.count(sellerGuid) ? "bot" : "human")
                << "\",\"item_entry\":" << itemEntry
                << ",\"item_name\":\"" << PlayerbotLLMInterface::SanitizeForJson(item->Name1)
                << "\",\"quantity\":" << fields[5].GetUInt32()
                << ",\"bid_copper\":" << fields[6].GetUInt32()
                << ",\"buyout_copper\":" << fields[7].GetUInt32()
                << ",\"state\":\"active\",\"expires_at\":" << fields[8].GetUInt64() << '}';
        } while (auctions->NextRow());
    }
    activeAuctionIds << ']';
    if (!firstAuction) auctionEvents << ',';
    auctionEvents << "{\"event_id\":\"auction-snapshot-complete-" << now
        << "\",\"type\":\"auction_snapshot_complete\",\"truncated\":"
        << (auctionSnapshotTruncated ? "true" : "false") << ",\"active_auction_ids\":"
        << activeAuctionIds.str() << "}]}";

    if (!reportedBots)
        return false;
    std::string eventBody = events.str(), auctionBody = auctionEvents.str(), planBody = plans.str();
    pendingPlans = std::async(std::launch::async, [eventBody, auctionBody, planBody]()
    {
        std::vector<std::string> debug;
        PlayerbotLLMInterface::Generate(eventBody, 9, sPlayerbotAIConfig.llmMaxSimultaniousGenerations,
            debug, true, "/v2/economy-events");
        PlayerbotLLMInterface::Generate(auctionBody, 9, sPlayerbotAIConfig.llmMaxSimultaniousGenerations,
            debug, true, "/v2/economy-events");
        return PlayerbotLLMInterface::Generate(planBody, 9, sPlayerbotAIConfig.llmMaxSimultaniousGenerations,
            debug, true, "/v2/economy-plans");
    });
    return true;
}

bool PlayerbotOrganicEconomy::Advertise(Player* bot, uint32 itemEntry, const Policy& currentPolicy)
{
    auto now = std::chrono::steady_clock::now();
    if (!currentPolicy.advertising || !IsCity(bot->GetZoneId()) ||
        (lastChannelAd.time_since_epoch().count() && now - lastChannelAd < std::chrono::seconds(currentPolicy.channelAdCooldownSeconds)) ||
        (adCooldowns[bot->GetGUIDLow()].time_since_epoch().count() && now - adCooldowns[bot->GetGUIDLow()] < std::chrono::seconds(currentPolicy.botAdCooldownSeconds)))
        return false;
    std::vector<uint32> known = KnownCraftOutputs(bot, 40);
    if (std::find(known.begin(), known.end(), itemEntry) == known.end())
        return false;
    ItemPrototype const* item = sObjectMgr.GetItemPrototype(itemEntry);
    if (!item)
        return false;
    std::ostringstream text;
    text << "Crafter available: " << item->Name1 << ". Whisper me if you need one made.";
    if (!bot->GetPlayerbotAI()->SayToTrade(text.str()))
        return false;
    lastChannelAd = now;
    adCooldowns[bot->GetGUIDLow()] = now;
    return true;
}

bool PlayerbotOrganicEconomy::ExecuteGoal(Player* bot, Profile& profile,
    const Policy& currentPolicy, std::string& failureReason)
{
    if (!bot || !bot->GetPlayerbotAI())
    {
        failureReason = "bot_unavailable";
        return false;
    }
    PlayerbotAI* ai = bot->GetPlayerbotAI();
    const std::string& goalType = profile.currentGoalType;
    const std::string& goalId = profile.currentGoalId;
    if (goalType == "equipment_upgrade" && sPlayerbotBuildProfiles.GearingGoalsEnabled())
    {
        if (!sPlayerbotBuildProfiles.HasGearingDeficiency(bot)) return true;
        if (ai->DoSpecificAction("equip upgrades", Event("organic economy gearing", "", bot), true))
        {
            failureReason = "equipping_owned_upgrade";
            return false;
        }
        if (currentPolicy.buying &&
            ai->DoSpecificAction("rpg ah buy", Event("organic economy gearing", "", bot), true))
        {
            failureReason = "purchased_character_owned_upgrade";
            return false;
        }
        if (currentPolicy.buying)
        {
            std::ostringstream action;
            action << "request travel target::" << (uint32)TravelDestinationPurpose::AH;
            if (ai->DoSpecificAction(action.str(), Event("organic economy gearing", "", bot), true))
                failureReason = "traveling_to_character_auction_upgrade";
            else
                failureReason = "awaiting_organic_upgrade_source";
        }
        else
        {
            ai->ChangeStrategy("nc +travel", BotState::BOT_STATE_NON_COMBAT);
            failureReason = "awaiting_quest_vendor_craft_or_loot_upgrade";
        }
        return false;
    }
    if (goalType == "profession_skill_up" && currentPolicy.careers)
    {
        const uint32 epoch = uint32(time(nullptr));
        auto pending = craftAttempts.find(bot->GetGUIDLow());
        if (pending != craftAttempts.end() && pending->second.goal != goalId)
        { craftAttempts.erase(pending); pending = craftAttempts.end(); }
        if (pending != craftAttempts.end())
        {
            const CraftAttempt attempt = pending->second;
            if (bot->GetSkillValuePure(attempt.skill) > attempt.beforeSkill)
            {
                craftAttempts.erase(pending);
                return true; // Actual skill gain, never a queued command.
            }
            if (bot->IsNonMeleeSpellCasted(true, false, true) || epoch < attempt.started + 30)
            { failureReason = "awaiting_profession_result"; return false; }
            failureReason = bot->GetItemCount(attempt.output, false) > attempt.beforeOutput ?
                "crafted_without_skill_gain" : "craft_interrupted_or_rejected";
            craftAttempts.erase(pending);
            return false;
        }
        if(!LivingServiceExecution::Prepare(bot))
        {failureReason=LivingServiceExecution::Blocker(bot);return false;}
        const uint32 recipe = GoalRecipe(bot->GetGUIDLow(),goalId);
        if (!recipe)
        { failureReason = "invalid_profession_recipe"; return false; }
        const auto* spell = sServerFacade.LookupSpellInfo(recipe);
        const uint32 skill = CraftSkill(bot, spell);
        if (!bot->HasSpell(recipe) || !spell)
        { failureReason = "recipe_unavailable_or_no_skill_gain"; return false; }
        // Collect an already-paid attachment before checking whether this
        // recipe is still useful for leveling or another reagent is available.
        for(uint32 i=0;i<MAX_SPELL_REAGENTS;++i) {
            if(spell->Reagent[i]<=0 || !ai::AhBidAction::HasPendingMaterial(bot,spell->Reagent[i])) continue;
            if(!LivingServiceExecution::Prepare(bot)) {failureReason=LivingServiceExecution::Blocker(bot);return false;}
            if(!PrepareRecipeMail(bot,spell->Reagent[i],goalId,failureReason)) return false;
            ai::AhBidAction market(ai);
            market.CollectRecipeMaterial(spell->Reagent[i],failureReason);
            if(failureReason=="recipe_mailbox_out_of_range")
                ReachRecipeService(bot,uint32(ai::TravelDestinationPurpose::Mail),goalId,failureReason);
            else ReleaseRecipeService(bot->GetGUIDLow(),failureReason);
            if(failureReason=="recipe_materials_received") mailPrepAttempts.erase(bot->GetGUIDLow());
            return false;
        }
        if(!skill) {ReleaseRecipeService(bot->GetGUIDLow(),"recipe_no_longer_useful");failureReason="recipe_unavailable_or_no_skill_gain";return false;}
        if (!SafeCraftReagents(bot, spell))
        {
            if (!SafeCraftReagents(bot, spell, true, false))
            { failureReason = "missing_or_reserved_recipe_materials"; return false; }
            std::map<uint32, uint32> needed;
            for (uint32 i = 0; i < MAX_SPELL_REAGENTS; ++i)
                if (spell->Reagent[i] > 0 && spell->ReagentCount[i]) needed[spell->Reagent[i]] += spell->ReagentCount[i];
            ai::BankAction bank(ai);
            // At most one real stack transfer per reagent per sweep; a rejected
            // transfer never grants stock or reports a profession completion.
            for (const auto& reagent : needed)
            {
                if (bot->GetItemCount(reagent.first, false) >= reagent.second) continue;
                if (bot->GetItemCount(reagent.first, true) < reagent.second)
                {
                    ai::AhBidAction market(ai);
                    if (ai::AhBidAction::HasPendingMaterial(bot, reagent.first))
                    {
                        market.CollectRecipeMaterial(reagent.first, failureReason);
                        if (failureReason == "recipe_mailbox_out_of_range")
                        {
                            ReachRecipeService(bot,uint32(TravelDestinationPurpose::Mail),goalId,failureReason);
                        }
                        return false;
                    }
                    if (!currentPolicy.buying) { failureReason = "recipe_purchasing_disabled"; return false; }
                    market.BuyRecipeMaterial(reagent.first, reagent.second, failureReason);
                    if (failureReason == "recipe_auctioneer_out_of_range" &&
                        ai::AhBidAction::HasMaterialOffer(bot, reagent.first, reagent.second - bot->GetItemCount(reagent.first,true)))
                    {
                        ReachRecipeService(bot,uint32(TravelDestinationPurpose::AH),goalId,failureReason);
                    }
                    else ReleaseRecipeService(bot->GetGUIDLow(),failureReason);
                    return false; // A paid order is not inventory or a completed craft.
                }
                bank.WithdrawForRecipe(reagent.first, reagent.second, failureReason);
                if (failureReason == "recipe_banker_out_of_range")
                {
                    ReachRecipeService(bot,uint32(TravelDestinationPurpose::Bank),goalId,failureReason);
                    return false;
                }
                if (bot->GetItemCount(reagent.first, false) < reagent.second) return false;
            }
            if (!SafeCraftReagents(bot, spell)) return false;
        }
        // Release only a completed material-service leg. Keep a station leg's
        // progress/deadline until native CheckCast actually accepts the recipe.
        auto trip=serviceTrips.find(bot->GetGUIDLow());
        if(trip!=serviceTrips.end() && !(trip->second.purpose&FocusService))
            ReleaseRecipeService(bot->GetGUIDLow(),"recipe_materials_prepared");
        SpellCastResult castResult=SPELL_CAST_OK;
        bool canCraft=ai->CanCastSpell(recipe, bot, 0, true, nullptr, false, false, false, &castResult);
        // Perform the cheap safe preparation and recheck in this same turn.
        // Returning for another 20-second sweep lets ordinary movement restart.
        if(!canCraft && (castResult==SPELL_FAILED_MOVING || castResult==SPELL_FAILED_NOT_STANDING)) {
            bot->StopMoving();bot->SetStandState(UNIT_STAND_STATE_STAND);
            canCraft=ai->CanCastSpell(recipe, bot, 0, true, nullptr, false, false, false, &castResult);
        }
        if (!canCraft) {
            if(castResult==SPELL_FAILED_REQUIRES_SPELL_FOCUS && spell->RequiresSpellFocus)
                ReachRecipeService(bot,FocusService|spell->RequiresSpellFocus,goalId,failureReason);
            else failureReason="recipe_cast_requirement_"+std::to_string(uint32(castResult));
            return false;
        }
        ReleaseRecipeService(bot->GetGUIDLow(),"recipe_station_ready");
        CraftAttempt attempt;
        attempt.goal = goalId; attempt.spell = recipe; attempt.skill = skill;
        attempt.beforeSkill = bot->GetSkillValuePure(skill);
        attempt.output = spell->EffectItemType[0]; attempt.beforeOutput = bot->GetItemCount(attempt.output, false);
        attempt.started = epoch;
        if (ai->CastSpell(recipe, bot)) craftAttempts[bot->GetGUIDLow()] = attempt;
        failureReason = craftAttempts.count(bot->GetGUIDLow()) ? "awaiting_profession_result" : "craft_rejected";
        return false;
    }
    if (goalType == "list_surplus" && currentPolicy.posting)
    {
        if (!HasAuctionSurplus(bot))
            return true;
        if (ai->DoSpecificAction("ah", Event("rpg action", "vendor", bot), true))
            return true;
        std::ostringstream action;
        action << "request travel target::" << (uint32)TravelDestinationPurpose::AH;
        if (ai->DoSpecificAction(action.str(), Event("organic economy auction", "", bot), true))
            failureReason = "traveling_to_auctioneer";
        else
            failureReason = "auctioneer_route_pending";
        return false;
    }
    if (goalType == "storage_pressure" && currentPolicy.careers)
    {
        LivingWowInventoryPressureSummary pressure = sPlayerbotInventoryPressure.Analyze(bot);
        if (pressure.bagUsage < 85 ||
            (!pressure.vendorStacks && !pressure.bankStacks && !pressure.craftStacks && !pressure.auctionStacks))
            return true;
        if (pressure.vendorStacks)
        {
            if (ai->DoSpecificAction("sell", Event("rpg action", "living-wow-safe-vendor", bot), true))
                return false;
            if (ai->DoSpecificAction("request progression vendor travel target",
                    Event("organic economy storage", "", bot), true))
                failureReason = "traveling_to_vendor";
            else
                failureReason = "vendor_route_pending";
            return false;
        }
        if (pressure.craftStacks &&
            ai->DoSpecificAction("craft random item", Event("organic economy storage", "", bot), true))
            return false;
        if (pressure.bankStacks && pressure.bankUsage < 95)
        {
            if (ai->DoSpecificAction("bank", Event("rpg action", "living-wow-safe-storage", bot), true))
                return false;
            std::ostringstream action;
            action << "request travel target::" << (uint32)TravelDestinationPurpose::Bank;
            if (ai->DoSpecificAction(action.str(), Event("organic economy storage", "", bot), true))
                failureReason = "traveling_to_bank";
            else
                failureReason = "bank_route_pending";
            return false;
        }
        if (pressure.auctionStacks && currentPolicy.posting)
        {
            if (ai->DoSpecificAction("ah", Event("rpg action", "vendor", bot), true))
                return false;
            std::ostringstream action;
            action << "request travel target::" << (uint32)TravelDestinationPurpose::AH;
            if (ai->DoSpecificAction(action.str(), Event("organic economy storage", "", bot), true))
                failureReason = "traveling_to_auctioneer";
            else
                failureReason = "auctioneer_route_pending";
            return false;
        }
        failureReason = "protected_inventory_only";
        return false;
    }
    if (goalType == "profession_advertisement" && currentPolicy.advertising)
    {
        size_t split = goalId.rfind(':');
        uint32 itemEntry = split == std::string::npos ? 0 : uint32(std::stoul(goalId.substr(split + 1)));
        if (Advertise(bot, itemEntry, currentPolicy))
            return true;
        failureReason = "advertisement_cooldown_or_location";
        return false;
    }
    if (goalType == "maintain_supplies")
    {
        // Existing Playerbots maintenance values own exact purchases. Keeping
        // the travel strategy active lets those validated needs select a real
        // vendor without inventing stock or granting supplies here.
        ai->ChangeStrategy("nc +travel", BotState::BOT_STATE_NON_COMBAT);
        return true;
    }
    failureReason = "unsupported_goal_type";
    return false;
}

void PlayerbotOrganicEconomy::ProcessActiveGoals(const Policy& currentPolicy,
    std::chrono::steady_clock::time_point now)
{
    if (currentPolicy.mode != "active" || profiles.empty())
        return;
    std::vector<uint32> guids;
    guids.reserve(profiles.size());
    std::set<uint32> verifying;
    for (const auto& entry : profiles)
    {
        if (entry.second.currentGoalState != "active" || entry.second.currentGoalId.empty()) continue;
        auto retry = retryCooldowns.find(entry.first);
        if (retry != retryCooldowns.end() && now < retry->second) continue;
        guids.push_back(entry.first);
        auto attempt = craftAttempts.find(entry.first);
        if (entry.second.currentGoalType == "profession_skill_up" && attempt != craftAttempts.end() &&
            attempt->second.goal == entry.second.currentGoalId) verifying.insert(entry.first);
    }
    auto priority=verifying;
    for(const auto& trip:serviceTrips) priority.insert(trip.first);
    const auto work = EconomyWorkOrder(guids, priority, executionCursor);
    for (uint32 guid : work)
    {
        Profile& profile = profiles[guid];
        if (profile.currentGoalState != "active" || profile.currentGoalId.empty()) continue;
        if (retryCooldowns[guid].time_since_epoch().count() && now < retryCooldowns[guid]) continue;
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
        // Result inspection doesn't move, respec, buy or cast. A human joining
        // after the cast must not prevent us observing its real outcome.
        if (!bot || !bot->IsInWorld() || (!verifying.count(guid) && !SafeForEconomy(bot)))
        { PauseRecipeService(guid,"recipe_service_safety_pause");retryCooldowns[guid] = now + std::chrono::seconds(30); continue; }
        std::string failureReason;
        bool completed = ExecuteGoal(bot, profile, currentPolicy, failureReason);
        if(!completed && failureReason=="recipe_materials_received") {
            // One real receipt buys a bounded execution window, not indefinite
            // immunity from replanning. The deadline survives realm restarts.
            const uint32 epoch=uint32(time(nullptr));
            profile.committedUntil=std::min(epoch+1800,profile.createdAt+10800);
            CharacterDatabase.PExecute("UPDATE organic_economy_goal SET authoritative_payload=JSON_SET(authoritative_payload,'$.paid_materials_until',%u),expires_at=GREATEST(expires_at,FROM_UNIXTIME(%u)) WHERE character_guid=%u AND capability_ref='%s' AND state='active'",profile.committedUntil,profile.committedUntil,guid,profile.currentGoalId.c_str());
        }
        retryCooldowns[guid] = now + std::chrono::seconds(completed ? 600 : serviceTrips.count(guid) ? 5 : 20);
        if (!completed)
        {
            if (lastBlockers[guid] != failureReason)
            {
                lastBlockers[guid] = failureReason;
                CharacterDatabase.PExecute("UPDATE organic_economy_goal SET failure_reason='%s' WHERE character_guid=%u AND capability_ref='%s' AND state='active'",
                    failureReason.c_str(), guid, profile.currentGoalId.c_str());
            }
            continue;
        }
        lastBlockers.erase(guid);
        actionCooldowns[guid] = now;
        CharacterDatabase.PExecute(
            "UPDATE organic_economy_goal SET state='completed',failure_reason='' WHERE character_guid='%u' AND capability_ref='%s' AND state='active'",
            guid, profile.currentGoalId.c_str());
        profile.currentGoalState = "completed";
    }
    if (!guids.empty()) executionCursor = (executionCursor + work.size()) % guids.size();
}

void PlayerbotOrganicEconomy::ApplyPlans(const std::string& response, const Policy& currentPolicy)
{
    if (response.empty()) return;
    boost::property_tree::ptree root;
    std::istringstream input(response);
    try { boost::property_tree::read_json(input, root); }
    catch (...) { return; }
    auto children = root.get_child_optional("plans");
    if (!children) return;
    for (const auto& child : *children)
    {
        uint32 guid = child.second.get<uint32>("character_guid", 0);
        std::string planId = child.second.get<std::string>("plan_id", "");
        std::string goalId = child.second.get<std::string>("goal_id", "");
        std::string goalType = child.second.get<std::string>("goal_type", "");
        std::string source = child.second.get<std::string>("source", "deterministic_fallback");
        if (!guid || planId.empty() || goalId.empty()) continue;
        auto attempt = craftAttempts.find(guid);
        auto active = profiles.find(guid);
        if(currentPolicy.mode=="active" && active!=profiles.end() && active->second.currentGoalState=="active" &&
            active->second.currentGoalType=="profession_skill_up") {
            Player* bot=sRandomPlayerbotMgr.GetPlayerBot(guid);
            if(active->second.committedUntil>uint32(time(nullptr))) continue;
            const uint32 pendingRecipe=bot && bot->IsInWorld()?PendingRecipeSpell(bot):0;
            const uint32 currentRecipe=GoalRecipe(guid,active->second.currentGoalId);
            const auto* currentSpell=sServerFacade.LookupSpellInfo(currentRecipe);
            // Receipt must not hand the job back to the planner before the
            // paid ingredients can be used. Existing goal expiry still bounds it.
            if(bot && bot->IsInWorld() && bot->HasSpell(currentRecipe) && CraftSkill(bot,currentSpell) &&
                SafeCraftReagents(bot,currentSpell,true)) continue;
            if(serviceTrips.count(guid) || (pendingRecipe && active->second.currentGoalId==
                "profession:"+std::to_string(guid)+":"+std::to_string(pendingRecipe))) continue;
        }
        if (currentPolicy.mode == "active" && attempt != craftAttempts.end() && active != profiles.end() &&
            active->second.currentGoalState == "active" &&
            PreserveCraftResult(attempt->second.goal, active->second.currentGoalId, attempt->second.started, uint32(time(nullptr))))
            continue; // Inspect the in-flight result before replacing its goal.
        CharacterDatabase.PExecute(
            "UPDATE organic_economy_goal SET state='expired' WHERE character_guid='%u' AND state IN ('candidate','proposed','active')", guid);
        CharacterDatabase.PExecute(
            "INSERT INTO organic_economy_goal (character_guid,goal_type,capability_ref,state,utility,source,authoritative_payload,expires_at) "
            "VALUES ('%u','%s','%s','%s',0,'%s','{}',DATE_ADD(NOW(),INTERVAL 1 HOUR))",
            guid, goalType.c_str(), goalId.c_str(), currentPolicy.mode == "active" ? "active" : "proposed", source.c_str());
        Profile& profile = profiles[guid];
        profile.currentGoalId = goalId;
        profile.currentGoalType = goalType;
        profile.currentGoalState = currentPolicy.mode == "active" ? "active" : "proposed";
        profile.createdAt=uint32(time(nullptr));profile.committedUntil=0;
        retryCooldowns.erase(guid);
        lastBlockers.erase(guid);
        craftAttempts.erase(guid);
    }
}

void PlayerbotOrganicEconomy::Update()
{
    auto now = std::chrono::steady_clock::now();
    if (!nextPolicyLoad.time_since_epoch().count() || now >= nextPolicyLoad)
    {
        policy = LoadPolicy();
        nextPolicyLoad = now + std::chrono::seconds(60);
    }
    if (pendingPlans.valid() && pendingPlans.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        ApplyPlans(pendingPlans.get(), policy);
    if (!nextExecutionSweep.time_since_epoch().count() || now >= nextExecutionSweep)
    {
        // Release execution on interruption; keep the accepted service step.
        // Actual domain invalidation still uses its explicit terminal path.
        std::vector<uint32> release,pause;
        for(const auto& trip:serviceTrips) {
            Player* bot=sRandomPlayerbotMgr.GetPlayerBot(trip.first);
            auto profile=profiles.find(trip.first);
            if(profile==profiles.end() || profile->second.currentGoalId!=trip.second.goal ||
                profile->second.currentGoalState!="active") release.push_back(trip.first);
            else if(policy.mode!="active" || !policy.careers || !SafeForEconomy(bot)) pause.push_back(trip.first);
        }
        for(uint32 guid:pause) PauseRecipeService(guid,"recipe_service_safety_pause");
        for(uint32 guid:release) ReleaseRecipeService(guid,"recipe_service_invalidated");
        ProcessActiveGoals(policy, now);
        nextExecutionSweep = now + std::chrono::seconds(5);
    }
    if (policy.mode == "off" || pendingPlans.valid() || (nextSubmit.time_since_epoch().count() && now < nextSubmit))
        return;
    // Random bots populate after the world update loop begins. An empty first
    // sample should retry quickly instead of hiding Observe telemetry for the
    // full planner cadence.
    bool submitted = Submit(policy);
    nextSubmit = now + std::chrono::seconds(submitted ? policy.cadenceSeconds : 30);
}
