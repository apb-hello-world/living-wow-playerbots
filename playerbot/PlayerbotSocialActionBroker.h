#ifndef _PLAYERBOT_SOCIAL_ACTION_BROKER_H
#define _PLAYERBOT_SOCIAL_ACTION_BROKER_H

#include <chrono>
#include <map>
#include <set>
#include <string>
#include "LivingActivity.h"

class Player;
class ObjectGuid;
struct ChatDirectorActionProposal;
struct ChatDirectorCandidate;
struct ChatDirectorEvent;

class PlayerbotSocialActionBroker
{
public:
    static PlayerbotSocialActionBroker& instance();
    bool Supports(const std::string& type) const;
    bool Create(const ChatDirectorActionProposal& proposal, const ChatDirectorEvent& event);
    bool CanUseSharedObject(Player* bot, Player* player, ObjectGuid guid);
    void AddSharedObjectCapabilities(Player* bot, Player* player, ChatDirectorCandidate& candidate);
    uint32 PreferredQuest(uint32 botGuid) const;
    // A bot that explicitly left an autonomous party for a human must not be
    // reclaimed by bot-only grouping before that human can invite it.  The
    // reservation is intentionally short lived and is cleared on acceptance.
    uint32 ReservedForPlayer(uint32 botGuid);
    bool CanReleasePendingInvite(Player* bot) const;
    void CompleteGroupReservation(uint32 botGuid, uint32 playerGuid);
    void ReserveForPlayer(uint32 botGuid, uint32 playerGuid);
    // Used while grounding chat capabilities so a leader can queue broader
    // personal free time behind an already-authorized maintenance trip.
    bool HasActiveVendorTrip(uint32 botGuid) const;
    uint32 PendingPetitionVolunteers(uint32 petitionGuid) const;
    void Update();

private:
    struct Action
    {
        LivingActivity::ActivityLease lease;
        std::string actionId;
        std::string eventId;
        std::string proposalId;
        std::string type;
        std::string capabilityRef;
        uint32 botGuid = 0;
        uint32 playerGuid = 0;
        uint32 subjectGuid = 0;
        uint32 groupId = 0;
        uint32 questId = 0;
        uint8 initialBagUsage = 0;
        uint8 bestBagUsage = 100;
        uint8 targetBagUsage = 75;
        uint8 sellAttempts = 0;
        std::string maintenanceType;
        bool outboundRelocated = false;
        bool restoreFollow = false;
        bool repairAttempted = false;
        bool maintenanceSucceeded = false;
        bool announceDeparture = false;
        std::string state;
        std::string failureReason;
        std::chrono::steady_clock::time_point expires;
        std::chrono::steady_clock::time_point serviceReadyAt;
        std::chrono::steady_clock::time_point stateSince;
        std::chrono::steady_clock::time_point lastSellAttempt;
        std::chrono::steady_clock::time_point lastActionAttempt;
        std::chrono::steady_clock::time_point completedAt;
    };

    struct SharedObjectOffer
    {
        uint32 botGuid = 0;
        uint32 playerGuid = 0;
        uint32 groupId = 0;
        uint64 objectGuid = 0;
        uint32 objectEntry = 0;
        uint32 skillId = 0;
        uint32 requiredSkill = 0;
        std::string nodeName;
        std::string objectKind;
        std::string state;
        std::chrono::steady_clock::time_point expires;
    };

    struct GatheringPolicy
    {
        uint32 playerGuid = 0;
        uint32 groupId = 0;
        uint32 skillId = 0;
        std::string mode;
    };

    struct GroupReservation
    {
        uint32 playerGuid = 0;
        std::chrono::steady_clock::time_point expires;
    };

    bool ValidateCommon(Player* bot, Player* player, bool requireBotAlive = true) const;
    bool StartVendorTrip(Player* bot, Player* player, const std::string& actionId,
        const std::string& eventId, const std::string& proposalId, bool announce);
    bool SetMaintenanceTarget(Player* bot, const std::string& maintenanceType) const;
    bool ContinueAtBank(Action& action, Player* bot);
    void QueuePartyReturn(Action& action, Player* bot, Player* player,
        const std::string& reason, bool success);
    void Report(const Action& action) const;
    std::map<std::string, Action> actions;
    std::map<uint32, std::pair<uint32, std::chrono::steady_clock::time_point>> preferredQuests;
    std::map<uint32, std::chrono::steady_clock::time_point> vendorCooldowns;
    std::set<uint32> vendorPressureNotified;
    std::map<uint32, SharedObjectOffer> sharedObjectOffers;
    std::map<std::string, std::chrono::steady_clock::time_point> sharedObjectCooldowns;
    std::map<std::string, std::chrono::steady_clock::time_point> sharedObjectPartyCooldowns;
    std::map<std::string, GatheringPolicy> gatheringPolicies;
    std::map<uint32, GroupReservation> groupReservations;
    std::chrono::steady_clock::time_point nextVendorScan;
};

#define sPlayerbotSocialActionBroker PlayerbotSocialActionBroker::instance()

#endif
