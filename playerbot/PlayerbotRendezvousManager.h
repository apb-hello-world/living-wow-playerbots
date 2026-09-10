#ifndef _PLAYERBOT_RENDEZVOUS_MANAGER_H
#define _PLAYERBOT_RENDEZVOUS_MANAGER_H

#include <chrono>
#include "PartyLootWindow.h"
#include "PlayerbotErrandTravel.h"
#include "PlayerbotServiceCatchup.h"
#include "LivingActivity.h"
#include "LivingActivityAcquisition.h"
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

class Player;
class Map;
namespace ai { class TravelDestination; class WorldPosition; class TravelTarget; }

class PlayerbotRendezvousManager
{
public:
    enum class RequestResult { accepted, ordinary_travel, unavailable, unsafe };
    enum class PartyActivityOwner
    {
        none, party_follow, combat, death_recovery, transport, rendezvous,
        party_errand, guild_event, player_command, guild_supply, economy_service
    };
    enum class PartyActivityPhase
    {
        idle, preparing, departing, traveling, performing, returning,
        verifying, deferred, blocked, completed, failed
    };

    static PlayerbotRendezvousManager& instance();
    RequestResult Request(Player* bot, Player* player, const std::string& actionId, bool returnAfter);
    // Registers a temporary human-created party assist without moving the bot
    // inside the invitation handler. The world update performs any relocation
    // after the group opcode and Playerbots strategy reset have completed.
    bool RegisterPartyAssist(Player* bot, Player* inviter, bool recovered = false);
    // Re-arm an existing mixed-party assist after a scoped trip (for example,
    // vending) so the bot returns through the same catch-up relocation used
    // after an invitation instead of selecting ordinary long-distance travel.
    bool ResumePartyAssist(Player* bot, Player* player, const std::string& reason);
    // Temporarily release a human-led party bot from close follow so its
    // existing RPG/economy strategies can perform personal errands. The bot
    // remains in the party and can be recalled through ResumePartyAssist.
    bool BeginPartyFreeTime(Player* bot, Player* player, const std::string& reason);
    bool IsPartyFreeTime(uint32 botGuid) const;
    bool HasVerifiedErrandRoute(uint32 botGuid) const;
    bool FindClassTrainingDestination(Player* bot, ai::TravelDestination*& destination,
        ai::WorldPosition*& position) const;
    bool YieldPartyFollowToLoot(Player* bot);
    std::string PartyState(uint32 botGuid) const;
    std::string PartyReason(uint32 botGuid) const;
    uint32 PartyDeadRecoveryAttempts(uint32 botGuid) const;
    uint32 PartyDeadRecoverySeconds(uint32 botGuid) const;
    void BeginDeparture(uint32 botGuid, uint32 playerGuid, const std::string& reason);
    void Cancel(uint32 botGuid, uint32 playerGuid, const std::string& reason);
    bool CancelGuildEvent(uint32 botGuid, const std::string& eventId, const std::string& reason);
    void Update();
    bool IsActive(uint32 botGuid, uint32 playerGuid) const;
    bool WasRelocated(uint32 botGuid, uint32 playerGuid) const;
    std::string State(uint32 botGuid, uint32 playerGuid) const;
    // Guild-event assembly owns non-combat movement until the complete roster
    // reaches its organizer. The participant/organizer split lets PlayerbotAI
    // preserve manager-issued follow movement while holding the organizer.
    bool IsGuildEventAssemblyParticipant(uint32 botGuid) const;
    bool IsGuildEventAssemblyOrganizer(uint32 botGuid) const;
    PartyActivityOwner GetPartyActivityOwner(uint32 botGuid) const;
    PartyActivityPhase GetPartyActivityPhase(uint32 botGuid) const;
    static const char* PartyActivityOwnerName(PartyActivityOwner owner);
    static const char* PartyActivityPhaseName(PartyActivityPhase phase);
    // Stable for one authoritative roster lifecycle and shared by Chat v2,
    // pending offers, and party-activity telemetry.
    std::string GetPartySessionId(Player* participant) const;
    uint64 GetPartySessionRevision(Player* participant) const;
    std::string GetPartyActivityStateJson(uint32 botGuid) const;
    bool OwnsPartyMovement(uint32 botGuid) const;
    bool BlocksAutonomousPartyWork(uint32 botGuid) const;
    bool AllowsOwnedMovement(uint32 botGuid, const std::string& actionName);
    // One shared world-thread budget for every Living WoW relocation path.
    // Callers retain pending state until the next manager update if consumed.
    bool ClaimRelocationSlot();
    bool CanRelocateUnobserved(Player* bot, Map* destinationMap,
        float destinationX, float destinationY, float destinationZ) const;
    bool FindSafeStagingPoint(Player* bot, Player* player,
        float& x, float& y, float& z) const;
    LivingActivity::Acquisition AcquirePartyActivityLease(uint32 botGuid, PartyActivityOwner owner,
        PartyActivityPhase phase, uint32 ttlSeconds, const std::string& reason,
        const std::string& jobKey, LivingActivity::ActivityLease& handle);
    bool UpdatePartyActivityLease(const LivingActivity::ActivityLease& handle,
        PartyActivityPhase phase, uint32 ttlSeconds, const std::string& reason);
    void ReleasePartyActivityLease(const LivingActivity::ActivityLease& handle,
        PartyActivityPhase terminalPhase, const std::string& reason);
    bool HasPartyActivityLease(const LivingActivity::ActivityLease& handle) const;
    std::vector<std::string> DrainPartyActivityTelemetry(bool includeSnapshots,
        size_t* transitionCount = nullptr);
    void RequeuePartyActivityTelemetry(const std::vector<std::string>& transitions);
    // Patch 177 supplies the bounded operational-chat aggregation behind this
    // API and drains it through the same party-activity transport.
    void RecordSuppressedActivity(Player* bot, const std::string& origin,
        const std::string& suppressionClass, const std::string& actionClass,
        uint32 count = 1);

private:
    struct ErrandObservation
    {
        uint8 bagUsage = 0;
        uint8 durability = 100;
        uint32 vendorStacks = 0;
        uint32 bankStacks = 0;
        uint32 auctionStacks = 0;
        uint32 mailPayloads = 0;
        uint32 auctionCount = 0;
        uint32 professionSkill = 0;
        uint32 inventorySignature = 0;
        std::set<uint32> knownSpells;
    };

    // One durable typed record per task in a party errand bundle. Legacy bit
    // masks below are retained only as an O(1) scheduler index; state exposed
    // to Chat v2 and telemetry comes from these records.
    struct PartySettlementErrand
    {
        uint32 type = 0;
        std::string taskId;
        PartyActivityPhase phase = PartyActivityPhase::preparing;
        uint32 routeAttempts = 0;
        uint32 operationAttempts = 0;
        std::string outcomeCode;
        ErrandObservation before;
        ErrandObservation after;
    };

    struct PartySession
    {
        uint32 botGuid = 0;
        uint32 playerGuid = 0;
        uint32 groupId = 0;
        uint32 partyRosterSignature = 0;
        std::string partySessionId;
        uint64 partySessionRevision = 0;
        uint32 originMapId = 0;
        uint32 originInstanceId = 0;
        float originX = 0.0f;
        float originY = 0.0f;
        float originZ = 0.0f;
        float originO = 0.0f;
        std::string previousActivity;
        std::string state;
        std::string reason;
        bool relocated = false;
        bool forceRelocation = false;
        bool approachIssued = false;
        bool freeTimeRecallRequested = false;
        uint32 approachAttempts = 0;
        uint32 deadRecoveryAttempts = 0;
        float lastHumanDistance = 0.0f;
        float followLastX = 0.0f, followLastY = 0.0f;
        bool followPositionKnown = false;
        ai::PartyLootWindow lootWindow;
        uint32 hearthStartMapId = 0;
        uint32 freeTimePlayerZoneId = 0;
        uint32 freeTimePlayerAreaId = 0;
        uint32 settlementKey = 0;
        uint32 automaticErrandMask = 0;
        uint32 automaticErrandScopeMask = 0;
        uint32 completedErrandMask = 0;
        uint32 deferredErrandMask = 0;
        uint32 currentErrand = 0;
        uint32 currentErrandCapability = 0;
        uint32 currentErrandOutput = 0;
        uint32 currentErrandOutputCountBefore = 0;
        bool currentErrandLocal = false;
        std::string currentErrandId;
        std::map<uint32, PartySettlementErrand> errands;
        uint32 errandRouteAttempts = 0;
        uint32 errandOperationAttempts = 0;
        bool errandOperationAccepted = false;
        bool errandFallbackUsed = false;
        bool errandCatchupUsed = false;
        std::chrono::steady_clock::time_point errandTravelStarted;
        std::chrono::steady_clock::time_point nextErrandCatchupAttempt;
        bool errandRelocationPending = false;
        bool errandSummarySent = false;
        float errandLastDistance = -1.0f;
        LivingWowErrandTravel errandTravel;
        std::chrono::steady_clock::time_point errandWorldportSince;
        ErrandObservation errandBefore;
        float hearthStartX = 0.0f;
        float hearthStartY = 0.0f;
        float hearthStartZ = 0.0f;
        float automaticErrandLastX = 0.0f;
        float automaticErrandLastY = 0.0f;
        std::chrono::steady_clock::time_point stateSince;
        std::chrono::steady_clock::time_point nextApproachAttempt;
        std::chrono::steady_clock::time_point humanAbsentSince;
        std::chrono::steady_clock::time_point lastFollowProgress;
        std::chrono::steady_clock::time_point nextFollowRepair;
        std::chrono::steady_clock::time_point staleCombatSince;
        std::chrono::steady_clock::time_point deadRecoveryStarted;
        std::chrono::steady_clock::time_point nextDeadRecoveryAttempt;
        std::chrono::steady_clock::time_point freeTimeUntil;
        std::chrono::steady_clock::time_point automaticErrandReadyAt;
        std::chrono::steady_clock::time_point postArrivalErrandGraceUntil;
        std::map<uint32, std::chrono::steady_clock::time_point> automaticErrandCooldowns;
        std::chrono::steady_clock::time_point automaticErrandHardDeadline;
        std::chrono::steady_clock::time_point automaticErrandActiveDeadline;
        std::chrono::steady_clock::time_point currentErrandDeadline;
        std::chrono::steady_clock::time_point currentErrandNoProgressDeadline;
        std::chrono::steady_clock::time_point errandBlockedSince;
        std::chrono::steady_clock::time_point nextErrandStep;
        std::chrono::steady_clock::time_point nextSettlementCheck;
        std::chrono::steady_clock::time_point nextAutomaticErrandCheck;
        std::chrono::steady_clock::time_point nextAutomaticErrandProgressLog;
        std::chrono::steady_clock::time_point errandSummaryReadyAt;
        std::chrono::steady_clock::time_point hearthStarted;
    };

    struct Session
    {
        uint32 botGuid = 0;
        uint32 playerGuid = 0;
        uint32 mapId = 0;
        float originX = 0.0f;
        float originY = 0.0f;
        float originZ = 0.0f;
        float originO = 0.0f;
        std::string actionId;
        std::string previousActivity;
        std::string state;
        std::string reason;
        bool returnAfter = true;
        bool relocated = false;
        bool combatPaused = false;
        uint32 pendingMapId = 0;
        float pendingX = 0.0f;
        float pendingY = 0.0f;
        float pendingZ = 0.0f;
        float pendingO = 0.0f;
        std::chrono::steady_clock::time_point started;
        std::chrono::steady_clock::time_point stateSince;
    };

    struct ExternalLease
    {
        LivingActivity::ActivityLease handle;
        PartyActivityOwner owner = PartyActivityOwner::none;
        PartyActivityPhase phase = PartyActivityPhase::idle;
        std::string reason;
        std::chrono::steady_clock::time_point expires;
    };

    struct GroupLifecycle
    {
        uint32 signature = 0;
        uint64 generation = 0;
    };

    struct SuppressedActivityAggregate
    {
        uint32 botGuid = 0;
        uint32 count = 0;
        std::string origin;
        std::string suppressionClass;
        std::string actionClass;
    };

    Session* Find(uint32 botGuid, uint32 playerGuid);
    const Session* Find(uint32 botGuid, uint32 playerGuid) const;
    bool IsPointUnobserved(Player* bot, float x, float y, float z) const;
    bool IsPointUnobservedOnMap(Map* map, Player* bot, float x, float y, float z) const;
    bool FindStagingPoint(Player* bot, Player* player, float& x, float& y, float& z) const;
    bool FindPartyRecoveryPoint(Player* bot, Player* player, float& x, float& y, float& z) const;
    bool ValidPath(Player* bot, float sx, float sy, float sz, Player* player) const;
    bool ReturnToActivity(Session& session, Player* bot);
    void UpdatePartyAssists();
    Player* FindPartyHuman(Player* bot) const;
    bool PartyHasHuman(Player* bot) const;
    bool PartyInstanceBoundarySafe(Player* bot, Player* player) const;
    bool PartySafeToRelease(Player* bot) const;
    bool StartPartyApproach(PartySession& session, Player* bot, Player* player);
    void RecoverStalePartyCombat(PartySession& session, Player* bot, Player* human);
    void BeginPartyHandoff(PartySession& session, Player* bot, Player* player,
        const std::string& reason);
    void ClearMovementState(Player* bot, Player* master, bool restoreFollow);
    bool ReturnPartyToActivity(PartySession& session, Player* bot);
    ErrandObservation ObserveErrandState(Player* bot) const;
    bool StartNextVerifiedErrand(PartySession& session, Player* bot);
    void UpdateVerifiedErrand(PartySession& session, Player* bot, Player* player,
        std::chrono::steady_clock::time_point now);
    bool TryErrandServiceCatchup(PartySession& session, Player* bot, ai::TravelTarget* target,
        std::chrono::steady_clock::time_point now);
    bool ExecuteVerifiedErrand(PartySession& session, Player* bot);
    bool VerifyErrand(const PartySession& session, const ErrandObservation& after) const;
    void FinishCurrentErrand(PartySession& session, Player* bot, bool completed,
        const std::string& reason);
    void QueueActivityTelemetry(uint32 botGuid, uint32 playerGuid, uint32 groupId,
        PartyActivityOwner owner, PartyActivityPhase phase, const std::string& event,
        const std::string& reason, uint32 task = 0,
        const ErrandObservation* before = nullptr, const ErrandObservation* after = nullptr);
    std::string BuildActivityTelemetry(uint32 botGuid, uint32 playerGuid, uint32 groupId,
        PartyActivityOwner owner, PartyActivityPhase phase, const std::string& event,
        const std::string& reason, uint32 task = 0,
        const ErrandObservation* before = nullptr, const ErrandObservation* after = nullptr,
        uint32 aggregateCount = 0);
    void LogPartyEvent(const PartySession& session, const char* event) const;
    void LogAutomaticErrandEvent(const PartySession& session, Player* bot, const char* event,
        uint32 previousErrands, uint32 remainingErrands) const;
    void LogEvent(const Session& session, const char* event);
    uint32 GetPartyRosterSignature(Player* participant) const;
    void PersistPartySession(const PartySession& session);
    bool RestorePersistedPartySession(Player* bot, Player* inviter, PartySession& session);
    void ClearPersistedPartySession(uint32 botGuid);
    void PrunePersistedPartySessions();

    std::map<uint32, Session> sessions;
    std::map<uint32, PartySession> partySessions;
    std::map<uint32, ExternalLease> externalLeases;
    uint64 externalLeaseGeneration = 0;
    std::map<std::string, SuppressedActivityAggregate> suppressedActivityAggregates;
    std::chrono::steady_clock::time_point nextSuppressionTelemetryFlush;
    std::deque<std::string> activityTelemetry;
    uint32 activityTelemetryDropped = 0;
    uint32 activityTelemetryRetried = 0;
    uint32 activityTelemetryReporter = 0;
    std::map<std::string, std::chrono::steady_clock::time_point> movementConflictCooldowns;
    mutable uint64 activitySequence = 0;
    uint64 errandSequence = 0;
    bool relocationAvailableThisUpdate = true;
    mutable uint64 partyProcessEpoch = 0;
    mutable uint64 partyGenerationSequence = 0;
    mutable std::map<uint32, GroupLifecycle> groupLifecycles;
    // A fresh invite that crossed an instance boundary is intentionally
    // rejected. Periodic restart discovery must not reinterpret that same
    // live roster as a persisted session and bypass the restriction.
    std::map<uint32, uint64> freshRestrictedPartyRevisions;
    std::chrono::steady_clock::time_point nextPartyDiscovery;
    bool partyPersistencePruned = false;
};

#define sPlayerbotRendezvousManager PlayerbotRendezvousManager::instance()

#endif
