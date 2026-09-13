#ifndef LIVING_GUILD_SUPPLIES_H
#define LIVING_GUILD_SUPPLIES_H
#include <cstdint>
#include <memory>
#include <string>
#include "LivingLegacyResourceView.h"
#include "LivingGuildDeposit.h"
class Item;
class Player;
// Single world-thread executor. Native transaction hooks persist delivery proof.
class PlayerbotGuildSupplies {
public:
    static PlayerbotGuildSupplies& instance();
    void Update();
    bool Reserved(uint32_t item) const;
    bool ReservedEntry(uint32_t player,uint32_t entry) const;
    std::shared_ptr<const LivingActivity::LegacyResourceView> ReservedItemsView() const;
    bool OwnsMovement(uint32_t player) const;
    bool AllowsMovement(uint32_t player,const std::string& action) const;
    uint32_t InTransit(uint32_t guild,const std::string& goal,std::string& status) const;
    void DeliveryCounts(uint32_t guild,const std::string& goal,uint32_t& reserved,uint32_t& mailed,uint32_t& collected) const;
    bool MoneyEnabled(uint32_t guild) const;
    void RecordDeposit(uint32_t guild,uint32_t actor,uint32_t entry,uint32_t count);
    void RecordMailed(uint32_t sender,uint32_t receiver,Item* item,uint32_t mail);
    void RecordCollected(uint32_t receiver,uint32_t mail,uint32_t item,uint32_t count);
    void RecordMoneyDeposit(uint32_t guild,uint32_t actor,uint32_t copper);
    bool ReadManagedDeposit(const LivingActivity::Task&,LivingActivity::GuildDepositQuote&,std::string&) const;
    bool ReadManagedMail(const LivingActivity::Task&,LivingActivity::ResourceClaim&,std::string&) const;
    bool ReadDeliveryJob(uint64_t delivery,uint32_t actor,LivingActivity::GuildDeliveryJob&,std::string&) const;
    bool AllowsManagedClaim(const LivingActivity::ResourceClaim&) const;
    bool BeginManagedDeposit(const LivingActivity::GuildDepositQuote&);
    void EndManagedDeposit();
private:
    PlayerbotGuildSupplies();
    ~PlayerbotGuildSupplies();
    struct State;
    std::unique_ptr<State> state_;
};
#define sGuildSupplies PlayerbotGuildSupplies::instance()
#endif
