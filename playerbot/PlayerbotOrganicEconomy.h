#ifndef _PLAYERBOT_ORGANIC_ECONOMY_H
#define _PLAYERBOT_ORGANIC_ECONOMY_H

#include "Common.h"
#include "LivingActivity.h"
#include "LivingActivityWorkClock.h"
#include "LivingServiceTravel.h"

#include <chrono>
#include <future>
#include <map>
#include <string>

class Player;

class PlayerbotOrganicEconomy
{
public:
    static PlayerbotOrganicEconomy& instance();
    void Update();
    bool CanLearnProfessionSpell(Player* bot, uint32 learnedSpell) const;
    std::string CurrentGoalType(uint32 characterGuid) const;
    uint32 RecipeMaterialQuantity(uint32 characterGuid, uint32 itemEntry) const;
    bool AllowsServiceAction(uint32 guid, const std::string& action) const;
    bool HasOwnedServiceRoute(uint32 guid,uint32 purpose) const;
    bool IsAuctionPostingEnabled() const { return policy.mode == "active" && policy.posting; }
    // Trusted finite service step; no inventory operation or synthetic access.
    // Reads the acknowledged saved root and acquires its existing authority.
    LivingActivity::ServiceTravelResult ReachSavedService(uint32 actor,const std::string& task,
        uint64 revision,LivingActivity::ServiceDestination service);

private:
    struct Policy
    {
        std::string mode = "observe";
        bool careers = false;
        bool posting = false;
        bool buying = false;
        bool commissions = false;
        bool advertising = false;
        uint32 cadenceSeconds = 600;
        uint32 botAdCooldownSeconds = 3600;
        uint32 channelAdCooldownSeconds = 600;
    };

    struct Profile
    {
        bool career = false;
        uint32 planVersion = 0;
        uint32 race = 0;
        uint32 createdAt = 0, committedUntil = 0;
        uint64 goalRow = 0, goalLookupToken = 0;
        uint32 intendedOne = 0;
        uint32 intendedTwo = 0;
        std::string currentGoalId;
        std::string currentGoalType;
        std::string currentGoalState;
        std::string managedTask, managedPhase;
    };

    struct CraftAttempt
    {
        std::string goal;
        uint32 spell = 0, skill = 0, beforeSkill = 0;
        uint32 output = 0, beforeOutput = 0, started = 0;
    };
    std::map<uint32, CraftAttempt> craftAttempts;
    struct ServiceTrip
    {
        LivingActivity::ActivityLease lease;
        LivingActivity::WorkClock work;
        LivingActivity::Task managedTask;
        LivingActivity::ActionContext action;
        LivingActivity::ActivityLease searchLease;
        uint64 searchRevision=0,ticket=0,initialActiveMs=0,routeRevision=0;
        LivingActivity::ServicePathProgress pathProgress;
        std::string goal;
        uint32 purpose=0, purchaseItem=0, started=0, progress=0, nextMove=0, attempts=0;
        float distance=1e30f;
        bool requesting=false, local=false,ready=false,routeOwned=false,routeInitialized=false;
    };
    std::map<uint32, ServiceTrip> serviceTrips;
    uint64 serviceSequence=0;
    std::map<uint32, uint32> serviceRetry;
    std::map<uint32, uint32> mailPrepAttempts;
    bool PrepareRecipeMail(Player* bot, uint32 entry, const std::string& goal, std::string& blocker);
    void ReachRecipeService(Player* bot, uint32 purpose, const std::string& goal, std::string& blocker);
    LivingActivity::ServiceTravelResult DriveRecipeService(Player* bot,uint32 purpose,
        const std::string& goal,const LivingActivity::Task* saved,uint32 purchaseItem=0,uint32 purchaseQuantity=0);
    void ReleaseRecipeService(uint32 guid, const std::string& reason);
    void PauseRecipeService(uint32 guid, const std::string& reason);
    std::map<uint32, std::string> lastBlockers;

    PlayerbotOrganicEconomy() = default;
    Policy LoadPolicy();
    std::map<uint32, Profile> LoadProfiles();
    void LookupGoalRow(uint32 guid, Profile& profile);
    bool Submit(const Policy& policy);
    void ApplyPlans(const std::string& response, const Policy& policy);
    void ProcessActiveGoals(const Policy& policy, std::chrono::steady_clock::time_point now);
    bool ExecuteGoal(Player* bot, Profile& profile, const Policy& policy, std::string& failureReason);
    bool SafeForEconomy(Player* bot) const;
    bool Advertise(Player* bot, uint32 itemEntry, const Policy& policy);

    std::future<std::string> pendingPlans;
    std::chrono::steady_clock::time_point nextSubmit;
    std::chrono::steady_clock::time_point nextPolicyLoad;
    std::chrono::steady_clock::time_point lastChannelAd;
    std::chrono::steady_clock::time_point nextExecutionSweep;
    std::map<uint32, std::chrono::steady_clock::time_point> actionCooldowns;
    std::map<uint32, std::chrono::steady_clock::time_point> retryCooldowns;
    std::map<uint32, std::chrono::steady_clock::time_point> adCooldowns;
    uint32 executionCursor = 0;
    uint64 goalLookupSequence = 0;
    std::map<uint32, Profile> profiles;
    Policy policy;
};

#define sPlayerbotOrganicEconomy PlayerbotOrganicEconomy::instance()

#endif
