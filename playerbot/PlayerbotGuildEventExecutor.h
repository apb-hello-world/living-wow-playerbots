#ifndef LIVING_PLAYERBOT_GUILD_EVENT_EXECUTOR_H
#define LIVING_PLAYERBOT_GUILD_EVENT_EXECUTOR_H
#include <cstdint>
#include <memory>
#include <string>
class Player;
namespace LivingActivity {struct Task;struct ActionContext;}
// World-thread coordinator. The worker receives copied travel information only.
// It never stores Player/Group/Map pointers across updates.
class PlayerbotGuildEventExecutor {
public:
    static PlayerbotGuildEventExecutor& instance();
    void Update();
    // Only the shared due queue calls this with an acknowledged task grant.
    // It performs one finite participant step, never schedules a second owner.
    std::string ExecuteParticipant(const LivingActivity::Task& task,const LivingActivity::ActionContext& action);
    bool Reserved(uint32_t guid) const;
    bool OwnsMovement(uint32_t guid) const;
    bool CanRendezvous(uint32_t guid,uint32_t coordinator,const std::string& event) const;
    bool CanGroupWith(uint32_t first,uint32_t second) const;
    bool AllowsMovement(uint32_t guid,const std::string& action) const;
    static bool DungeonSupported(uint32_t map);
    static bool DungeonParticipantReady(Player* player,uint32_t map);
    // kind 1: native PvE group kill credit; kind 2: successful quest reward.
    void RecordCredit(uint32_t actor,uint32_t guild,uint32_t group,uint32_t kind,
        uint32_t entry,uint64_t source,uint32_t map,uint32_t instance,uint32_t occurred);
private:
    PlayerbotGuildEventExecutor();
    ~PlayerbotGuildEventExecutor();
    struct State;
    std::unique_ptr<State> state_;
};
#define sGuildEventExecutor PlayerbotGuildEventExecutor::instance()
#endif
