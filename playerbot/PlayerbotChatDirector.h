#ifndef _PLAYERBOT_CHAT_DIRECTOR_H
#define _PLAYERBOT_CHAT_DIRECTOR_H

#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>
#include "LivingProgressionRecovery.h"

class Player;
struct ChatDirectorActionProposal;

struct ChatDirectorCapability
{
    std::string capabilityRef;
    std::string type;
    std::string itemName;
    std::string itemKind;
    std::string itemUsage;
    std::string demandReason;
    uint32 economicVersion = 0;
    uint32 demandScore = 0;
    uint32 currentQuantity = 0;
    uint32 desiredQuantity = 0;
    uint32 itemId = 0;
    uint32 quality = 0;
    uint32 quantity = 0;
    uint32 minQuantity = 0;
    uint32 maxQuantity = 0;
    uint32 totalQuantity = 0;
    uint32 reserveQuantity = 0;
    uint32 disposableQuantity = 0;
    uint32 priceCopper = 0;
    uint32 valueCopper = 0;
    uint32 vendorSellCopper = 0;
    uint32 playerbotSellCopper = 0;
    uint32 playerbotBuyCopper = 0;
    uint32 marketUnitCopper = 0;
    uint32 marketSamples = 0;
    uint32 minimumUnitPriceCopper = 0;
    uint32 maximumUnitPriceCopper = 0;
    bool giftEligible = false;
    uint32 questId = 0;
    uint32 groupId = 0;
    uint32 actorGuid = 0;
    std::string description;
    std::vector<std::string> deliveries;
};

struct ChatDirectorQuest
{
    struct Objective
    {
        std::string type;
        std::string name;
        uint32 current = 0;
        uint32 required = 0;
    };

    struct SourceItem
    {
        uint32 itemId = 0;
        std::string name;
        uint32 current = 0;
        uint32 required = 0;
        uint32 useSpellId = 0;
        bool usableNow = false;
        std::string blocker;
    };

    uint32 questId = 0;
    std::string title;
    std::string status;
    bool shareable = false;
    std::vector<Objective> objectives;
    std::vector<SourceItem> sourceItems;
};

struct ChatDirectorGroupState
{
    uint32 groupId = 0;
    uint32 leaderGuid = 0;
    std::string leaderName;
    uint32 memberCount = 0;
    uint32 capacity = 5;
    bool raid = false;
    bool isLeader = false;
    bool isAssistant = false;
    bool full = false;
    bool pendingInvite = false;
    uint32 pendingInviteLeaderGuid = 0;
    std::string pendingInviteKind;
    std::vector<std::string> humanMembers;
};

struct ChatDirectorCandidate
{
    uint32 guid = 0;
    std::string name;
    uint8 race = 0;
    uint8 cls = 0;
    uint8 level = 1;
    uint32 zone = 0;
    uint32 subzone = 0;
    std::string zoneName;
    std::string subzoneName;
    float distanceToSpeaker = -1.0f;
    std::string role;
    std::string currentActivity;
    std::string questLog;
    bool questLogTruncated = false;
    ChatDirectorGroupState groupState;
    std::vector<ChatDirectorQuest> quests;
    bool grouped = false;
    bool inCombat = false;
    bool alive = true;
    bool ghost = false;
    bool available = true;
    std::string partyAssistState;
    std::string partyAssistReason;
    std::string partyActivityStateJson;
    uint32 deadRecoveryAttempts = 0;
    uint32 deadRecoverySeconds = 0;
    bool hasPetition = false;
    std::string petitionName;
    uint32 petitionSignatures = 0;
    uint32 petitionRequired = 0;
    std::vector<std::string> signedPartyMembers;
    std::vector<std::string> eligiblePetitionPartyMembers;
    uint32 volunteerPetitionOwnerGuid = 0;
    std::string volunteerPetitionOwnerName;
    uint32 volunteerPetitionGuid = 0;
    std::string volunteerPetitionName;
    uint32 volunteerPetitionSignatures = 0;
    uint32 volunteerPetitionRequired = 0;
    uint32 guildId = 0;
    std::string guildName;
    std::string guildLeaderName;
    uint32 guildLeaderGuid = 0;
    uint32 guildRank = 0;
    uint32 guildMemberCount = 0;
    bool isGuildLeader = false;
    std::vector<ChatDirectorCapability> actionCapabilities;
};

struct ChatDirectorEvent
{
    std::string key;
    std::string eventId;
    std::string channelType;
    std::string channelName;
    std::string speakerName;
    uint32 speakerGuid = 0;
    uint8 speakerLevel = 1;
    std::string speakerQuestLog;
    bool speakerQuestLogTruncated = false;
    std::vector<ChatDirectorQuest> speakerQuests;
    uint32 zone = 0;
    uint32 team = 0;
    std::string message;
    bool ambient = false;
    bool factualGrounding = false;
    std::string groundingType;
    std::map<uint32, ChatDirectorCandidate> candidates;
    std::chrono::steady_clock::time_point firstSeen;
};

struct ChatDirectorReply
{
    uint32 botGuid = 0;
    std::string text;
    uint32 delayMs = 2000;
    std::string requiresActionId;
    std::string replyChannel;
};

class PlayerbotChatDirector
{
public:
    static PlayerbotChatDirector& instance();
    void Observe(Player* bot, uint32 msgType, uint32 speakerGuid, const std::string& speakerName,
        const std::string& message, const std::string& channelName);
    void ObservePartyQuestPlan(Player* bot, uint32 questId, const std::string& questName,
        const std::string& objective, const std::string& areaName, uint32 distanceYards);
    void ObserveGroupInviteConflict(Player* bot, Player* initiator);
    void ObservePartyJoin(Player* bot, Player* inviter);
    bool HandleGuildAddonMessage(Player* receiverBot, Player* sender, const std::string& message);
    void Update();
    // Implemented only in the isolated fixture binary; never a chat capability.
    std::string IsolatedRecoveryProbe(Player* bot, bool disabled = false);

private:
    struct ActiveRequest
    {
        ChatDirectorEvent event;
        std::future<std::string> response;
    };

    struct ScheduledReply
    {
        ChatDirectorEvent event;
        ChatDirectorReply reply;
        std::chrono::steady_clock::time_point due;
    };

    std::string ChannelType(uint32 msgType, const std::string& channelName) const;
    std::string BuildJson(const ChatDirectorEvent& event) const;
    std::vector<ChatDirectorReply> ParseReplies(const std::string& response) const;
    std::vector<ChatDirectorActionProposal> ParseActionProposals(const std::string& response) const;
    void Dispatch(const ScheduledReply& scheduled);
    void MaybeCreateAmbientEvent(std::chrono::steady_clock::time_point now);
    void MaybeAdvertiseGuilds(std::chrono::steady_clock::time_point now);
    void MaybeCreateProactiveGroupEvent(std::chrono::steady_clock::time_point now);
    void MaybeReportBotHealth(std::chrono::steady_clock::time_point now);
    void MaybeReportProgressionTrace(std::chrono::steady_clock::time_point now);
    void MaybeReportPartyActivity(std::chrono::steady_clock::time_point now);
    void MaybeReportOrganicEconomy(std::chrono::steady_clock::time_point now);
    void MaybeReportGuildSocieties(std::chrono::steady_clock::time_point now);
    void ApplyGuildPlans(const std::string& response, std::chrono::steady_clock::time_point now);
    void UpdateGuildEventLifecycle(std::chrono::steady_clock::time_point now);
    void ReloadGuildPolicy(std::chrono::steady_clock::time_point now);
    void SendGuildAddonSnapshot(Player* source, Player* receiver);

    struct BotHealthState
    {
        LivingActivity::RecoveryPauseClock recoveryPause;
        std::chrono::steady_clock::time_point recoveryAvailableSince;
        float x = 0.0f;
        float y = 0.0f;
        std::chrono::steady_clock::time_point lastMoved;
        std::chrono::steady_clock::time_point lastMeaningfulProgress;
        std::chrono::steady_clock::time_point lastGameplayProgress;
        std::chrono::steady_clock::time_point lastTravelAdvance;
        std::string travelTargetPosition;
        float lastTravelDistance = -1.0f;
        bool travelAdvancedSinceReport = false;
        std::map<uint32, std::chrono::steady_clock::time_point> completedQuestSince;
        std::chrono::steady_clock::time_point heightFaultSince;
        std::chrono::steady_clock::time_point lastRecovery;
        std::chrono::steady_clock::time_point recoveryBackoffUntil;
        std::vector<std::chrono::steady_clock::time_point> recoveryAttempts;
        std::string recoveryResult;
        uint8 lastLevel = 0;
        uint32 lastXp = 0;
        std::string questProgressSignature;
        uint32 lastRecoveryQuestId = 0;
        std::map<uint32, uint32> turninRouteFailures;
        std::map<uint32, std::chrono::steady_clock::time_point> turninDeferredUntil;
        uint32 recoveryQuestId = 0;
        uint32 recoveryStep = 0;
        std::chrono::steady_clock::time_point recoveryStartedAt;
        uint32 objectiveRouteFailures = 0;
        uint32 recoveryRouteRefreshes = 0;
        uint32 recoveryFailureStreak = 0;
        std::string recoveryTerminalReason;
        std::string nearbyRerouteResult;
        bool questItemFollowup = false;
        uint32 recoveryInteractionAttempts = 0;
        std::chrono::steady_clock::time_point lastRecoveryInteraction;
    };

    std::mutex mutex;
    std::map<std::string, ChatDirectorEvent> pending;
    std::vector<ActiveRequest> active;
    std::vector<ScheduledReply> scheduled;
    uint64 sequence = 0;
    std::chrono::steady_clock::time_point nextAmbient;
    std::chrono::steady_clock::time_point nextGuildAdvertisement;
    std::chrono::steady_clock::time_point lastConversation;
    std::chrono::steady_clock::time_point nextEconomySample;
    std::chrono::steady_clock::time_point nextGuildSample;
    std::chrono::steady_clock::time_point nextGuildPolicyReload;
    std::chrono::steady_clock::time_point nextGuildLifecycleUpdate;
    std::string guildPolicyMode = "observe";
    std::string guildRolloutScope = "canary";
    std::set<uint32> guildCanaryIds;
    std::future<std::string> pendingGuildPlans;
    std::set<uint32> guildAddonClients;
    std::chrono::steady_clock::time_point nextHealthSample;
    std::chrono::steady_clock::time_point nextProgressionTraceSample;
    std::chrono::steady_clock::time_point nextPartyActivitySample;
    std::chrono::steady_clock::time_point nextPartyActivitySnapshot;
    std::future<std::string> pendingPartyActivityTelemetry;
    std::vector<std::string> pendingPartyActivityTransitions;
    std::map<uint32, BotHealthState> botHealth;
    std::map<std::string, std::chrono::steady_clock::time_point> questPlanCooldowns;
    struct SharedActivityState
    {
        std::chrono::steady_clock::time_point firstSeen;
        std::chrono::steady_clock::time_point lastSeen;
        std::chrono::steady_clock::time_point lastOffer;
    };
    std::map<std::string, SharedActivityState> sharedActivity;
    std::map<uint32, std::chrono::steady_clock::time_point> proactivePlayerCooldowns;
    std::map<std::string, std::chrono::steady_clock::time_point> guildAdvertisementCooldowns;
};

#define sPlayerbotChatDirector PlayerbotChatDirector::instance()

#endif
