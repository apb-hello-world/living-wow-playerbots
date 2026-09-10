#include "playerbot/playerbot.h"
#include "PartyCombatPositioning.h"
#include "PartyPositioningPolicy.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivityGameplay.h"
#include "LivingActivityScope.h"
#include "ServerFacade.h"
#include "Groups/Group.h"
#include "Entities/Pet.h"
#include "MotionGenerators/PathFinder.h"
#include "MotionGenerators/MotionMaster.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Grids/CellImpl.h"
#include "strategy/values/HazardsValue.h"
#include "strategy/values/PositionValue.h"

using namespace ai;
using namespace ai::party_positioning;

namespace
{
Point CombatPoint(const WorldObject* object)
{
    return {object->GetPositionX(), object->GetPositionY(), object->GetPositionZ()};
}

bool EngagedWith(Unit* enemy, Unit* member)
{
    return member && (enemy->GetVictim() == member ||
        member->getAttackers().count(enemy) ||
        enemy->getThreatManager().HasThreat(member, true));
}

bool EngagedWithParty(Unit* enemy, Group* group)
{
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->getSource();
        if (member && member->IsInWorld() && member->GetMap() == enemy->GetMap() &&
            (EngagedWith(enemy, member) || EngagedWith(enemy, member->GetPet())))
            return true;
    }
    return false;
}

struct Danger
{
    Unit* enemy;
    float radius;
};

void Report(PlayerbotAI* ai, int state, const char* result, bool retreat, unsigned pulls = 0,
    Unit* target = nullptr, unsigned pathChecks = 0, unsigned dangers = 0)
{
    AiObjectContext* context = ai->GetAiObjectContext();
    const char* key = retreat ? "party retreat outcome" : "party approach outcome";
    if (AI_VALUE2(int, "manual int", key) == state)
        return;
    SET_AI_VALUE2(int, "manual int", key, state);
    sLog.outString("LivingParty positioning bot=%u name=%s movement=%s result=%s additional_enemies=%u target=%u path_checks=%u nearby_dangers=%u",
        ai->GetBot()->GetGUIDLow(), ai->GetBot()->GetName(), retreat ? "retreat" : "approach", result, pulls,
        target ? target->GetGUIDLow() : 0, pathChecks, dangers);
}

bool MakePath(Player* bot, const Point& destination, std::vector<Point>& points)
{
    PathFinder path(bot);
    if (!path.calculate(destination.x, destination.y, destination.z, false))
        return false;
    if (!(path.getPathType() & PATHFIND_NORMAL) ||
        (path.getPathType() & (PATHFIND_NOPATH | PATHFIND_INCOMPLETE | PATHFIND_SHORTCUT |
            PATHFIND_NOT_USING_PATH | PATHFIND_SHORT)))
        return false;
    points.clear();
    points.push_back(CombatPoint(bot));
    for (const auto& p : path.getPath())
        points.push_back({p.x, p.y, p.z});
    return points.size() > 1 && DistanceSquared(points.back(), destination) <= 1.0f;
}

float Length(const std::vector<Point>& path)
{
    float length = 0;
    for (size_t i = 1; i < path.size(); ++i)
        length += std::sqrt(DistanceSquared(path[i-1], path[i]));
    return length;
}

bool RatePath(Player* bot, const std::vector<Point>& path,
    const std::vector<Danger>& dangers, const std::list<HazardPosition>& hazards,
    bool emergency, Score& score)
{
    for (const auto& hazard : hazards)
    {
        if (hazard.first.getMapId() != bot->GetMapId())
            continue;
        const Point center = {hazard.first.getX(), hazard.first.getY(), hazard.first.getZ()};
        if (CrossesBoundary(path, center, hazard.second, [](const Point&) { return true; }))
            return false;
        // Escape must actually leave the ground effect, not stop deeper inside.
        if (emergency && DistanceSquared(path.back(), center) < (hazard.second + 0.5f)*(hazard.second + 0.5f))
            return false;
    }
    score.extraPulls = 0;
    for (const Danger& danger : dangers)
    {
        if (CrossesBoundary(path, CombatPoint(danger.enemy), danger.radius,
            [&](const Point& p) { return danger.enemy->IsEnemyCheckIgnoresLos() || danger.enemy->IsWithinLOS(p.x, p.y,
                p.z + bot->GetCollisionHeight(), true); }))
        {
            ++score.extraPulls;
            if (!emergency)
                return false;
        }
    }
    return true;
}

} // namespace

bool PartyCombatPositioning::Enabled(PlayerbotAI* ai)
{
    if (!ai) return false;
    Player* bot = ai->GetBot();
    return bot && bot->IsInWorld() && !bot->IsBeingTeleported() &&
        ai->HasRealPlayerMaster() && bot->GetGroup() && bot->IsAlive() &&
        (bot->IsInCombat() || ai->IsStateActive(BotState::BOT_STATE_COMBAT)) &&
        !bot->InBattleGround() && !bot->GetTransport() && !bot->IsTaxiFlying() &&
        !bot->IsInWater() && !bot->IsFreeFlying();
}

bool PartyCombatPositioning::SpellRanges(PlayerbotAI* ai, Unit* target,
    const std::string& spell, float& minimum, float& maximum)
{
    if (!ai->GetSpellRange(spell, &maximum, &minimum))
        return false;
    Player* bot = ai->GetBot();
    const uint32 spellId = ai->GetAiObjectContext()->GetValue<uint32>("spell id", spell)->Get();
    const SpellEntry* info = sServerFacade.LookupSpellInfo(spellId);
    const SpellRangeEntry* range = info ? sServerFacade.LookupSpellRangeEntry(info->rangeIndex) : nullptr;
    if (!range)
        return false;
    // GetSpellRange accounts for equipment/talents but uses the bot itself as
    // target. Match Spell::GetMinMaxRange's stationary, center-distance rules.
    const float reach = bot->GetCombatReach() + target->GetCombatReach();
    if (range->Flags & SPELL_RANGE_FLAG_MELEE)
        maximum += bot->GetCombinedCombatReach(target, true, 0.0f) -
            bot->GetCombinedCombatReach(bot, true, 0.0f);
    else
    {
        maximum += reach;
        if (range->Flags & SPELL_RANGE_FLAG_RANGED)
            minimum += bot->GetCombinedCombatReach(target, true, 0.0f) -
                bot->GetCombinedCombatReach(bot, true, 0.0f);
        else if (minimum > 0)
            minimum += reach;
    }
    return true;
}

bool PartyCombatPositioning::Move(PlayerbotAI* ai, Unit* target,
    float minRange, float maxRange, bool retreat)
{
    if (!ai) return false;
    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld() || !bot->GetGroup() || bot->IsBeingTeleported() ||
        !target || !target->IsInWorld() || target->GetMapId() != bot->GetMapId() ||
        target->GetInstanceId() != bot->GetInstanceId() || !ai->CanMove())
        return false;
    const auto nativePermit = LivingActivity::NativeCombatMovementPermit(*ai, target);
    std::unique_ptr<LivingActivity::ExecutionScope> nativeScope;
    if (nativePermit.validated) nativeScope.reset(new LivingActivity::ExecutionScope(nativePermit));
    const LivingActivity::Effects effects{LivingActivity::Mask(LivingActivity::Effect::Movement),
        nativePermit.validated ? LivingActivity::Lane::Combat : LivingActivity::Lane::Managed, true};
    auto permitted = [&] { return sLivingActivityCoordinator.PermitEffects(*ai, effects, "native party combat positioning"); };
    if (!permitted()) return false;
    AiObjectContext* context = ai->GetAiObjectContext();
    const Point start = CombatPoint(bot), focus = CombatPoint(target);
    const float currentRange = std::sqrt(DistanceSquared(start, focus));
    const auto hazards = AI_VALUE(std::list<HazardPosition>, "hazards");
    bool emergency = false;
    for (const auto& hazard : hazards)
        if (hazard.first.getMapId() == bot->GetMapId() &&
            hazard.first.sqDistance(WorldPosition(bot)) < (hazard.second + 0.5f)*(hazard.second + 0.5f))
            emergency = true;

    const int now = int(time(nullptr));
    const char* retryKey = retreat ? "party retreat retry" : "party approach retry";
    if (!emergency && AI_VALUE2(int, "manual int", retryKey) > now)
        return false; // Let other combat actions run while positioning is blocked.

    minRange = std::max(0.0f, minRange);
    if (maxRange <= minRange)
    {
        if (!retreat || emergency)
            ai->StopMoving();
        Report(ai, 2, "no_usable_range", retreat);
        return false;
    }
    const float preferredRange = maxRange;
    if (!retreat && !emergency && currentRange >= minRange && currentRange <= maxRange &&
        target->IsWithinLOSInMap(bot, true))
    {
        ai->StopMoving();
        Report(ai, 1, "already_in_range", retreat);
        return false;
    }

    // This is a spatial query, not the attack shortlist: include high-level
    // creatures, unseen corners and enemies fighting a different party.
    std::list<Unit*> nearby;
    MaNGOS::AnyUnitInObjectRangeCheck check(bot, 120.0f);
    MaNGOS::UnitListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitAllObjects(bot, searcher, 120.0f);
    std::vector<Danger> dangers;
    for (Unit* unit : nearby)
    {
        if (!unit || !unit->IsCreature() || !unit->IsAlive() ||
            !unit->CanAttackOnSight(bot) || !static_cast<Creature*>(unit)->CanInitiateAttack() ||
            EngagedWithParty(unit, bot->GetGroup()))
            continue;
        const float range = unit->GetAttackDistance(bot);
        if (range > 0)
            dangers.push_back({unit, range + 3.0f + bot->GetObjectBoundingRadius() + unit->GetObjectBoundingRadius()});
    }

    std::vector<Point> candidates;
    const WorldPosition previous = AI_VALUE2(WorldPosition, "custom position", "party combat destination");
    const WorldPosition previousFocus = AI_VALUE2(WorldPosition, "custom position", "party combat focus");
    const bool committed = AI_VALUE2(int, "manual int", "party combat commit until") > now &&
        AI_VALUE2(int, "manual int", "party combat retreat") == int(retreat) &&
        previous.getMapId() == bot->GetMapId() && previousFocus.fDist(target) < 2.0f;
    if (committed)
        candidates.push_back({previous.getX(), previous.getY(), previous.getZ()});

    // Angles on the party's side of the target first; shorter retreats can
    // still improve casting distance without demanding the ideal full range.
    const float baseAngle = std::atan2(start.y - focus.y, start.x - focus.x);
    const float angles[] = {0, 0.6f, -0.6f, 1.2f, -1.2f, 2.0f, -2.0f, 3.141593f};
    const float shortRange = retreat ? std::min(preferredRange, std::max(minRange + 0.5f, currentRange + 2.0f)) :
        std::max(minRange + 0.5f, preferredRange * 0.65f);
    for (float radius : {preferredRange, shortRange, std::min(preferredRange, minRange + 1.0f)})
        for (float angle : angles)
            candidates.push_back({focus.x + std::cos(baseAngle + angle)*radius,
                focus.y + std::sin(baseAngle + angle)*radius, focus.z});
    if (emergency)
        for (float radius : {4.0f, 8.0f, 12.0f})
            for (float angle : angles)
                candidates.push_back({start.x + std::cos(baseAngle + angle)*radius,
                    start.y + std::sin(baseAngle + angle)*radius, start.z});

    const Player* master = ai->GetMaster();
    const Point party = master && master->GetMap() == bot->GetMap() ? CombatPoint(master) : start;
    const auto preference = [&](const Point& p, float length)
    {
        return length + std::abs(std::sqrt(DistanceSquared(p, focus)) - preferredRange) * 0.4f +
            std::sqrt(DistanceSquared(p, party)) * 0.1f;
    };
    // Cheap ordering before bounded navmesh work. Retain a committed point at
    // the front, but never bypass current safety checks to keep a commitment.
    std::stable_sort(candidates.begin() + (committed ? 1 : 0), candidates.end(),
        [&](const Point& a, const Point& b)
        { return preference(a, std::sqrt(DistanceSquared(a, start))) <
                 preference(b, std::sqrt(DistanceSquared(b, start))); });

    // If no complete firing position is reachable, allow local progress.
    // These waypoints are still checked against the navmesh, ground effects
    // and every unengaged enemy. Never fall back to an unchecked chase.
    const size_t completeCandidates = candidates.size();
    if (!retreat && !emergency && currentRange > maxRange)
        for (float distance : {4.0f, 8.0f, 12.0f})
            for (float angle : {0.0f, 0.6f, -0.6f, 1.2f, -1.2f})
                candidates.push_back({start.x - std::cos(baseAngle + angle)*distance,
                    start.y - std::sin(baseAngle + angle)*distance, start.z});

    std::vector<Point> best;
    Score bestScore;
    Point destination = start;
    unsigned pathChecks = 0, phaseChecks = 0, safePaths = 0;
    bool partial = false;
    for (size_t index = 0; index < candidates.size(); ++index)
    {
        if (index == completeCandidates)
        {
            if (!best.empty())
                break;
            phaseChecks = 0;
        }
        const bool progress = index >= completeCandidates;
        Point candidate = candidates[index];
        bot->UpdateAllowedPositionZ(candidate.x, candidate.y, candidate.z);
        const float range = std::sqrt(DistanceSquared(candidate, focus));
        const float travel = std::sqrt(DistanceSquared(candidate, start));
        if (travel < 0.1f || travel > 40.0f || !std::isfinite(candidate.z))
            continue;
        if (progress ? !UsefulApproach(range, minRange, currentRange) :
            (!emergency && !FitsRange(range, minRange, maxRange, currentRange, retreat)))
            continue;
        if (!progress && !emergency && !target->IsWithinLOS(candidate.x, candidate.y,
            candidate.z + bot->GetCollisionHeight(), true))
            continue;
        if (!emergency)
        {
            bool unsafeEndpoint = false;
            for (const Danger& danger : dangers)
            {
                const float radius = danger.radius + 0.5f;
                const Point center = CombatPoint(danger.enemy);
                const float distance = DistanceSquared(candidate, center);
                if (distance < radius * radius &&
                    (danger.enemy->IsEnemyCheckIgnoresLos() || danger.enemy->IsWithinLOS(candidate.x, candidate.y,
                        candidate.z + bot->GetCollisionHeight(), true)) &&
                    distance + 0.01f < DistanceSquared(start, center))
                {
                    unsafeEndpoint = true;
                    break;
                }
            }
            if (unsafeEndpoint)
                continue;
        }
        if (phaseChecks >= (emergency ? 40u : 8u))
            continue;
        ++phaseChecks;
        ++pathChecks;
        std::vector<Point> path;
        if (!MakePath(bot, candidate, path) || Length(path) > 60.0f)
            continue;
        Score score;
        if (!RatePath(bot, path, dangers, hazards, emergency, score))
            continue;
        score.preference = preference(candidate, Length(path));
        if (best.empty() || score.BetterThan(bestScore))
        {
            best = path;
            bestScore = score;
            destination = candidate;
            partial = progress;
        }
        // Recheck a committed route against current patrol positions every time.
        if (committed && index == 0 && score.extraPulls == 0)
            break;
        if (!emergency && ++safePaths == 3)
            break;
    }

    if (best.empty())
    {
        // A failed optional retreat must not cancel another action's approach.
        // Existing checked steps are only four yards and finish on their own.
        if (!retreat || emergency)
        {
            ai->StopMoving();
            SET_AI_VALUE2(int, "manual int", "party combat commit until", 0);
        }
        SET_AI_VALUE2(int, "manual int", retryKey, now + 1);
        Report(ai, 2, "no_safe_route", retreat, 0, target, pathChecks, dangers.size());
        return false;
    }

    // Execute only a short portion of the checked navmesh route. No chase
    // generator may subsequently bend this path into an unchecked enemy pack.
    Movement::PointsArray step;
    step.push_back(G3D::Vector3(start.x, start.y, start.z));
    float remaining = 4.0f;
    for (size_t i = 1; i < best.size() && remaining > 0.01f; ++i)
    {
        const float distance = std::sqrt(DistanceSquared(best[i-1], best[i]));
        if (distance < 0.01f)
            continue;
        const Point end = Between(best[i-1], best[i], std::min(1.0f, remaining / distance));
        step.push_back(G3D::Vector3(end.x, end.y, end.z));
        remaining -= distance;
    }
    if (step.size() < 2)
        return false;
    if (!permitted()) return false; // Recheck current authority after path evaluation.
    ai->StopMoving();
    bot->GetMotionMaster()->Clear(false, true);
#ifndef MANGOSBOT_TWO
    bot->GetMotionMaster()->MovePath(step, FORCED_MOVEMENT_RUN, false, false);
#else
    bot->GetMotionMaster()->MovePath(step, FORCED_MOVEMENT_RUN, false);
#endif
    SET_AI_VALUE2(WorldPosition, "custom position", "party combat destination",
        WorldPosition(bot->GetMapId(), destination.x, destination.y, destination.z));
    SET_AI_VALUE2(WorldPosition, "custom position", "party combat focus", WorldPosition(target));
    SET_AI_VALUE2(int, "manual int", "party combat retreat", int(retreat));
    if (!committed)
        SET_AI_VALUE2(int, "manual int", "party combat commit until", now + 2);
    Report(ai, bestScore.extraPulls ? 4 : (partial ? 5 : 3),
        bestScore.extraPulls ? "emergency_escape" : (partial ? "safe_progress" : "safe_route"),
        retreat, bestScore.extraPulls, target, pathChecks, dangers.size());
    return true;
}
