#include "playerbot/playerbot.h"
#include "PlayerbotGuildSupplies.h"
#include "LivingServiceExecution.h"
#include "LivingActivityCoordinator.h"
#include "GuildSupplyPolicy.h"
#include "GuildGovernancePolicy.h"
#include "PlayerbotGuildGovernance.h"
#include "PlayerbotGuildEventExecutor.h"
#include "PlayerbotInventoryPressure.h"
#include "PlayerbotRendezvousManager.h"
#include "PlayerbotActionBroker.h"
#include "RandomPlayerbotMgr.h"
#include "Guilds/GuildMgr.h"
#include "Globals/ObjectAccessor.h"
#include "Maps/MapManager.h"
#include "Mails/Mail.h"
#include "Entities/Bag.h"
#include "strategy/values/ItemUsageValue.h"
#include "strategy/values/BudgetValues.h"
#include <algorithm>
#include <map>
#include <set>
#include <cmath>

using namespace ai;
using namespace livingguild;
namespace {
using Owner=PlayerbotRendezvousManager::PartyActivityOwner;
using Phase=PlayerbotRendezvousManager::PartyActivityPhase;
std::map<uint32,uint32> absentHumans;
bool HasOnlineHuman(Player* p) {
    if(p->GetGroup()) for(const auto& slot:p->GetGroup()->GetMemberSlots())
        if(!sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(slot.guid))&&sObjectAccessor.FindPlayer(slot.guid)) return true;
    return false;
}
bool HumanPartyBlocks(Player* p) {
    bool hasHuman=false;
    if(p->GetGroup()) for(const auto& slot:p->GetGroup()->GetMemberSlots()) {
        if(sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(slot.guid))) continue;
        hasHuman=true;
        // A loaded human may be zoning, so do not mistake map transfer for logout.
        if(Player* human=sObjectAccessor.FindPlayer(slot.guid)) {
            absentHumans.erase(p->GetGUIDLow());
            if(!human->IsInWorld()||!human->IsAlive()||human->IsInCombat()||human->IsTaxiFlying()||human->GetTransport()||
                !sPlayerbotRendezvousManager.IsPartyFreeTime(p->GetGUIDLow())||
                sPlayerbotRendezvousManager.HasVerifiedErrandRoute(p->GetGUIDLow())) return true;
        }
    }
    if(!hasHuman||HasOnlineHuman(p)) {absentHumans.erase(p->GetGUIDLow());return false;}
    const uint32 now=uint32(time(nullptr));auto inserted=absentHumans.emplace(p->GetGUIDLow(),now);
    const uint32 grace=std::max(60u,std::min(900u,uint32(sPlayerbotAIConfig.chatDirectorPartyDisconnectGraceSeconds)));
    return now-inserted.first->second<grace;
}
Player* Online(uint32 guid) {return sObjectAccessor.FindPlayer(ObjectGuid(HIGHGUID_PLAYER,guid));}
bool Bot(Player* p) {return p&&p->GetSession()&&!p->isRealPlayer()&&p->GetPlayerbotAI()&&
    sPlayerbotAIConfig.IsInRandomAccountList(p->GetSession()->GetAccountId());}
const char* SafetyBlocker(Player* p) {
    if(!sPlayerbotAIConfig.chatDirectorPartyActivityOwnership) return "activity_ownership_disabled";
    if(!Bot(p)||!p->IsInWorld()||!p->GetMap()) return "carrier_offline_or_loading";
    if(!p->IsAlive()) return "recovering_from_death";
    if(p->IsInCombat()) return "in_combat";
    if(p->IsBeingTeleported()) return "map_transfer_in_progress";
    if(p->IsTaxiFlying()||p->GetTransport()) return "aboard_transport";
    if(p->InBattleGround()||p->GetMap()->IsDungeon()) return "in_dungeon_or_battleground";
    if(LivingServiceExecution::Busy(p)) return LivingServiceExecution::Blocker(p);
    if(sGuildEventExecutor.Reserved(p->GetGUIDLow())) return "committed_to_guild_event";
    if(HumanPartyBlocks(p)) return "human_party_or_reconnect_grace";
    const auto owner=sPlayerbotRendezvousManager.GetPartyActivityOwner(p->GetGUIDLow());
    // Stale ordinary follow may yield once there is no human party. Explicit
    // actions, errands and guild commitments retain their higher priority.
    if(owner==Owner::none||owner==Owner::guild_supply||owner==Owner::party_follow) return "";
    if(owner==Owner::party_errand&&sPlayerbotRendezvousManager.IsPartyFreeTime(p->GetGUIDLow())&&
        !sPlayerbotRendezvousManager.HasVerifiedErrandRoute(p->GetGUIDLow())) return "";
    return "another_activity_owns_movement";
}
bool Safe(Player* p) {return !*SafetyBlocker(p);}
bool MayDeposit(Guild* guild,uint32 member) {
    for(uint8 tab=0;tab<guild->GetPurchasedTabs();++tab)
        if(guild->IsMemberHaveRights(member,tab,GUILD_BANK_RIGHT_DEPOSIT_ITEM)) return true;
    return false;
}
std::list<Item*> Inventory(Player* p) {return p->GetPlayerbotAI()->InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS);}
void InvalidateItems(Player* p) {
    if(!p||!p->GetPlayerbotAI()) return;
    p->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("item usage");
    p->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("inventory items");
}
uint32 DonationAllowance(Player* p,uint32 now) {
    if(CharacterDatabase.PQuery("SELECT delivery_id FROM guild_society_supply_delivery WHERE donor_guid=%u AND item_entry=0 AND deposited_quantity>0 AND updated_at>%u LIMIT 1",p->GetGUIDLow(),now-86400)) return 0;
    auto traits=CharacterDatabase.PQuery("SELECT p.generosity,p.thrift,e.joined_at FROM organic_economy_profile p JOIN guild_society_member_evidence e ON e.character_guid=p.character_guid AND e.guild_id=%u WHERE p.character_guid=%u",p->GetGuildId(),p->GetGUIDLow());
    if(!traits) return 0;
    auto* f=traits->Fetch();const uint32 joined=f[2].GetUInt32();
    if(joined>now) return 0;
    // Honor all existing personal budget categories, including professions and
    // mounts, plus a small cash floor. Refresh at execution, never trust a quote.
    uint64 protectedMoney=5000;
    for(uint32 category=1;category<=11;++category)
        protectedMoney+=p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<uint32>("money needed for",std::to_string(category))->Get();
    auto held=CharacterDatabase.PQuery("SELECT COALESCE(SUM(money_copper),0) FROM organic_economy_reservation WHERE character_guid=%u AND expires_at>NOW()",p->GetGUIDLow());
    if(held) protectedMoney+=held->Fetch()[0].GetUInt64();
    return SupplyDonation(p->GetMoney(),protectedMoney,f[0].GetUInt32(),f[1].GetUInt32(),(now-joined)/86400);
}
struct Delivery {
    LivingActivity::ActivityLease lease;
    uint64 id=0;uint32 guild=0,donor=0,carrier=0,item=0,entry=0,quantity=0,deposited=0,mail=0;
    std::string goal,phase,blocker;
    uint32 retry=0,active=0,last=0,nextMove=0,progress=0,attempts=0,operations=0,prepVendor=0,prepBank=0;
    float distance=1e30f;uint32 service=0;
};
struct Service {uint32 guid=0,entry=0,map=0,type=0,zone=0;float x=0,y=0,z=0;};
struct Goal {uint32 guild=0,required=0,reserved=0;bool money=false;};
uint16 EmptyBagSlot(Player* p) {
    for(uint8 slot=INVENTORY_SLOT_ITEM_START;slot<INVENTORY_SLOT_ITEM_END;++slot)
        if(!p->GetItemByPos(INVENTORY_SLOT_BAG_0,slot)) return (uint16(INVENTORY_SLOT_BAG_0)<<8)|slot;
    for(uint8 bag=INVENTORY_SLOT_BAG_START;bag<INVENTORY_SLOT_BAG_END;++bag) {
        Bag* container=dynamic_cast<Bag*>(p->GetItemByPos(INVENTORY_SLOT_BAG_0,bag));
        if(container) for(uint8 slot=0;slot<container->GetBagSize();++slot)
            if(!p->GetItemByPos(bag,slot)) return (uint16(bag)<<8)|slot;
    }
    return 0;
}
}
struct PlayerbotGuildSupplies::State {
    bool ready=false;uint32 check=0,next=0,load=0,cursor=0;
    std::map<uint64,Delivery> deliveries;
    std::map<uint32,uint64> moving;
    std::set<uint32> protectedItems;
    LivingActivity::LegacyResourcePublisher protection;
    std::vector<Service> services;
    std::map<uint32,bool> enabled;
    std::set<uint32> moneyEnabled;
    std::map<std::string,Goal> goals;
    uint64 workCursor=0;
    uint64 depositing=0,mailing=0;
    uint32 depositCount=0;
    bool mailRecorded=false;
    uint32 mailReceiver=0;

    void Release(Delivery& delivery) {
        const uint32 guid = delivery.lease.actor;
        auto entry = moving.find(guid);
        if(entry != moving.end() && entry->second == delivery.id) moving.erase(entry);
        Player* p=Online(guid);
        if(p&&p->IsInWorld()&&p->GetMap()&&p->IsAlive()&&!p->IsInCombat()&&!p->IsBeingTeleported()&&
            sPlayerbotRendezvousManager.HasPartyActivityLease(delivery.lease)&&
            sPlayerbotRendezvousManager.GetPartyActivityOwner(guid)==Owner::guild_supply) {
            p->StopMoving();p->GetMotionMaster()->MoveIdle();
        }
        sPlayerbotRendezvousManager.ReleasePartyActivityLease(delivery.lease,Phase::deferred,"supply_delivery_released");
        delivery.lease = {};
    }
    void Block(Delivery& d,const std::string& reason,uint32 now,bool cooldown=false) {
        Release(d);
        if(d.blocker!=reason) {
            d.blocker=reason;
            CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET blocker='%s',updated_at=%u WHERE delivery_id=%llu",reason.c_str(),now,(unsigned long long)d.id);
        }
        if(cooldown) {d.retry=now+600;d.active=0;d.last=0;d.operations=0;d.attempts=0;d.service=0;d.prepVendor=0;d.prepBank=0;}
    }
    void Finish(Delivery& d,const char* phase,const char* reason,uint32 now) {
        Release(d);d.phase=phase;d.blocker=reason;
        CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET phase='%s',blocker='%s',updated_at=%u WHERE delivery_id=%llu",phase,reason,now,(unsigned long long)d.id);
        InvalidateItems(Online(d.carrier));
        PublishProtection();
    }
    void PublishProtection() {
        LivingActivity::LegacyResourceView view;
        view.items=protectedItems;
        view.items.erase(0);
        for (const auto& pair:deliveries) {
            const auto& d=pair.second;
            if (!SupplyTerminal(d.phase) && d.carrier && d.entry) view.entries.emplace(d.carrier,d.entry);
        }
        protection.Replace(std::move(view));
    }
    void Reload(uint32 now) {
        load=now+15;enabled.clear();
        goals.clear();
        auto goalRows=CharacterDatabase.PQuery("SELECT DISTINCT g.goal_id,g.guild_id,g.required_quantity,g.reserved_quantity,g.request_kind FROM guild_society_supply_goal g JOIN guild_society_supply_delivery d ON d.goal_id=g.goal_id AND d.guild_id=g.guild_id WHERE g.state='active' AND d.phase NOT IN ('completed','cancelled','failed') ORDER BY g.guild_id,g.goal_id LIMIT 256");
        if(goalRows) do {auto* f=goalRows->Fetch();if(Id(f[0].GetString())) goals[f[0].GetString()]={f[1].GetUInt32(),f[2].GetUInt32(),f[3].GetUInt32(),f[4].GetCppString()=="money"};} while(goalRows->NextRow());
        moneyEnabled.clear();
        auto settings=CharacterDatabase.PQuery("SELECT guild_id,money_enabled FROM guild_society_supply_execution WHERE enabled=1");
        if(settings) do {auto* f=settings->Fetch();enabled[f[0].GetUInt32()]=true;if(f[1].GetBool()) moneyEnabled.insert(f[0].GetUInt32());} while(settings->NextRow());
        auto rows=CharacterDatabase.PQuery("SELECT delivery_id,guild_id,goal_id,donor_guid,carrier_guid,item_guid,item_entry,quantity,deposited_quantity,mail_id,phase,blocker FROM guild_society_supply_delivery WHERE phase NOT IN ('completed','cancelled','failed') ORDER BY delivery_id LIMIT 256");
        std::map<uint64,Delivery> fresh;
        if(rows) do {
            Field* f=rows->Fetch();Delivery d;
            auto old=deliveries.find(f[0].GetUInt64());
            if(old!=deliveries.end()) {
                if(old->second.carrier!=f[4].GetUInt32()) Release(old->second);
                d=old->second;
            }
            d.id=f[0].GetUInt64();d.guild=f[1].GetUInt32();d.goal=f[2].GetString();d.donor=f[3].GetUInt32();d.carrier=f[4].GetUInt32();
            if(d.phase!=f[10].GetString()||d.deposited!=f[8].GetUInt32()) {d.operations=0;d.service=0;d.active=0;}
            d.item=f[5].GetUInt32();d.entry=f[6].GetUInt32();d.quantity=f[7].GetUInt32();d.deposited=f[8].GetUInt32();d.mail=f[9].GetUInt32();d.phase=f[10].GetString();d.blocker=f[11].GetString();
            fresh[d.id]=d;
        } while(rows->NextRow());
        for(auto& old:deliveries) if(!fresh.count(old.first)) {Release(old.second);InvalidateItems(Online(old.second.carrier));}
        deliveries.swap(fresh);
        protectedItems.clear();
        for(const auto& pair:deliveries) {
            const auto& d=pair.second;protectedItems.insert(d.item);
            Player* carrier=Online(d.carrier);
            if(d.phase=="carried"&&d.mail&&Bot(carrier)&&carrier->IsInWorld())
                for(auto* item:Inventory(carrier)) if(item&&item->GetEntry()==d.entry) protectedItems.insert(item->GetGUIDLow());
        }
        PublishProtection();
    }
    bool Busy(uint32 guid) const {for(const auto& d:deliveries) if(!SupplyTerminal(d.second.phase)&&(d.second.carrier==guid||d.second.donor==guid)) return true;return false;}
    const Service* Destination(Player* p,bool mail,uint32 npcFlag=0) const {
        const Service* best=nullptr;float score=1e30f;
        for(const auto& s:services) {
            if(s.type!=(npcFlag?100000+npcFlag:uint32(mail?GAMEOBJECT_TYPE_MAILBOX:GAMEOBJECT_TYPE_GUILD_BANK))||!SupplyCity(s.zone,p->GetTeam()==ALLIANCE)) continue;
            const float distance=s.map==p->GetMapId()?p->GetDistance(s.x,s.y,s.z):100000.0f+std::abs(s.x-p->GetPositionX());
            if(distance<score) {best=&s;score=distance;}
        }
        return best;
    }
    bool ClaimService(Player* p,Delivery& d,uint32 now) {
        const auto acquisition=sPlayerbotRendezvousManager.AcquirePartyActivityLease(p->GetGUIDLow(),Owner::guild_supply,Phase::traveling,90,
            "guild_supply_delivery",std::to_string(d.id),d.lease);
        if(!acquisition.Permitted()) {
            Block(d,acquisition.blocker,now);
            return false; // Neither proximity nor an old route handle grants ownership.
        }
        moving[p->GetGUIDLow()]=d.id;
        return true;
    }
    WorldObject* Reach(Player* p,Delivery& d,bool mail,uint32 now,uint32 npcFlag=0) {
        const uint32 type=npcFlag?100000+npcFlag:uint32(mail?GAMEOBJECT_TYPE_MAILBOX:GAMEOBJECT_TYPE_GUILD_BANK);
        const std::string workingReason=npcFlag?(npcFlag==UNIT_NPC_FLAG_VENDOR?"clearing_bags_at_vendor":"clearing_bags_at_personal_bank"):"";
        auto working=[&]() {if(d.blocker!=workingReason) {d.blocker=workingReason;CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET blocker='%s',updated_at=%u WHERE delivery_id=%llu",workingReason.c_str(),now,(unsigned long long)d.id);}};
        const auto nearby=p->GetPlayerbotAI()->GetAiObjectContext()->GetValue<std::list<ObjectGuid>>(npcFlag?"nearest npcs no los":"nearest game objects no los")->Get();
        for(auto guid:nearby) {
            if(npcFlag) {if(auto* npc=p->GetNPCIfCanInteractWith(guid,npcFlag)) {
                if(!ClaimService(p,d,now)) return nullptr;
                working();return npc;
            }}
            else if(auto* go=p->GetGameObjectIfCanInteractWith(guid,GameobjectTypes(type))) {
                if(!ClaimService(p,d,now)) return nullptr;
                working();return go;
            }
        }
        // Online town breaks may use a service already reached by normal
        // errands, but must not install a competing route or teleport.
        if(HasOnlineHuman(p)) {Block(d,"waiting_for_local_town_service",now);return nullptr;}
        const Service* service=nullptr;
        for(const auto& s:services) if(s.guid==d.service&&s.type==type) {service=&s;break;}
        if(!service) {service=Destination(p,mail,npcFlag);if(service) {d.service=service->guid;d.distance=1e30f;d.progress=now;d.attempts=0;}}
        if(!service) {Block(d,"no_accessible_service",now,true);return nullptr;}
        if(!ClaimService(p,d,now)) return nullptr;
        working();
        const float distance=p->GetMapId()==service->map?p->GetDistance(service->x,service->y,service->z):1e20f;
        if(distance+2<d.distance) {d.distance=distance;d.progress=now;}
        if(distance<=600&&now<d.progress+30&&now>=d.nextMove) {
            d.nextMove=now+10;
            float x=service->x,y=service->y,z=service->z;
            if(p->GetMap()->GetReachableRandomPointOnGround(x,y,z,2.0f,false))
                p->GetMotionMaster()->MovePoint(201,x,y,z);
        }
        if(distance>600||now>=d.progress+30||d.active>=300) {
            Map* map=sMapMgr.FindMap(service->map,0);
            float x=service->x,y=service->y,z=service->z;
            if(sPlayerbotAIConfig.chatDirectorPartyFallbackTravel&&map&&map->GetReachableRandomPointOnGround(x,y,z,2.0f,false)&&
                sPlayerbotRendezvousManager.CanRelocateUnobserved(p,map,x,y,z)&&
                sPlayerbotRendezvousManager.ClaimRelocationSlot()) {
                if(++d.attempts>2) {Block(d,"service_route_failed",now,true);return nullptr;}
                p->TeleportTo(service->map,x,y,z,0);
                d.progress=now;d.nextMove=now+5;d.distance=1e30f;
                return nullptr;
            }
            if(d.active>=300) Block(d,"safe_travel_deferred",now,true);
        }
        return nullptr;
    }
    bool MakeCollectionRoom(Player* p,Delivery& d,uint32 now,bool requireEmptySlot=false) {
        if(requireEmptySlot && EmptyBagSlot(p)) return true;
        ItemPosCountVec dest;
        if(!requireEmptySlot && p->CanStoreNewItem(NULL_BAG,NULL_SLOT,dest,d.entry,d.quantity-d.deposited)==EQUIP_ERR_OK) return true;
        const auto summary=sPlayerbotInventoryPressure.Analyze(p);
        const bool vendor=summary.vendorStacks&&d.prepVendor<2;
        const bool bank=summary.HasBankableStorage()&&d.prepBank<2;
        if(!vendor&&!bank) {Block(d,"bags_full_no_safe_storage",now,true);return false;}
        const uint32 flag=vendor?UNIT_NPC_FLAG_VENDOR:UNIT_NPC_FLAG_BANKER;
        if(Reach(p,d,false,now,flag)) {
            auto* ai=p->GetPlayerbotAI();const size_t before=Inventory(p).size();
            if(vendor) ++d.prepVendor;else ++d.prepBank;
            ai->DoSpecificAction(vendor?"sell":"bank",Event("rpg action",vendor?"living-wow-safe-vendor":"living-wow-safe-storage",nullptr),true);
            InvalidateItems(p);ai->GetAiObjectContext()->ClearValues("bag space");ai->GetAiObjectContext()->ClearValues("bank space");
            // Native operations must produce actual capacity, never trust their
            // return value. Persist changed possessions before collecting mail.
            if(Inventory(p).size()<before) p->SaveToDB();
            d.service=0;d.nextMove=0;Release(d);
        }
        return false;
    }
};
PlayerbotGuildSupplies::PlayerbotGuildSupplies():state_(new State) {}
PlayerbotGuildSupplies::~PlayerbotGuildSupplies()=default;
PlayerbotGuildSupplies& PlayerbotGuildSupplies::instance(){static PlayerbotGuildSupplies value;return value;}
bool PlayerbotGuildSupplies::ReservedEntry(uint32 player,uint32 entry) const {
    return ReservedItemsView()->Entry(player,entry);
}
bool PlayerbotGuildSupplies::Reserved(uint32 item) const {
    return ReservedItemsView()->Item(item);
}
std::shared_ptr<const LivingActivity::LegacyResourceView> PlayerbotGuildSupplies::ReservedItemsView() const {
    return state_->protection.Inspect();
}
bool PlayerbotGuildSupplies::OwnsMovement(uint32 guid) const {
    auto moving=state_->moving.find(guid);
    if(moving==state_->moving.end()) return false;
    auto delivery=state_->deliveries.find(moving->second);
    return delivery!=state_->deliveries.end()&&
        sPlayerbotRendezvousManager.HasPartyActivityLease(delivery->second.lease)&&
        sPlayerbotRendezvousManager.GetPartyActivityOwner(guid)==Owner::guild_supply;
}
bool PlayerbotGuildSupplies::MoneyEnabled(uint32 guild) const {return state_->ready&&state_->moneyEnabled.count(guild)!=0;}
uint32 PlayerbotGuildSupplies::InTransit(uint32 guild,const std::string& goal,std::string& status) const {
    status="searching_for_spare_items";uint32 count=0;
    auto enabled=state_->enabled.find(guild);
    if(!state_->ready||enabled==state_->enabled.end()||!enabled->second) status="delivery_automation_off";
    for(const auto& pair:state_->deliveries) {const auto& d=pair.second;
        if(d.guild!=guild||d.goal!=goal||SupplyTerminal(d.phase)) continue;
        count+=d.quantity-d.deposited;
        status=!d.blocker.empty()?d.blocker:d.phase=="mailed"?"in_mail":"being_delivered";
    }
    return count;
}
bool PlayerbotGuildSupplies::AllowsMovement(uint32 guid,const std::string& action) const {
    if(!OwnsMovement(guid)) return true;
    if(LivingServiceExecution::DisruptiveMaintenance(action)) return false;
    // The executor installs its own point movement. Do not suppress combat,
    // healing, rolls or validated nearby loot; only alternate route owners.
    return action.find("travel")==std::string::npos&&action.find("rpg")==std::string::npos&&
        action.find("reach spell")==std::string::npos&&
        action.find("follow")==std::string::npos&&action.find("move random")==std::string::npos&&
        action!="go"&&action.find("grind")==std::string::npos;
}
void PlayerbotGuildSupplies::DeliveryCounts(uint32 guild,const std::string& goal,uint32& reserved,uint32& mailed,uint32& collected) const {
    reserved=mailed=collected=0;
    for(const auto& pair:state_->deliveries) {const auto& d=pair.second;
        if(d.guild!=guild||d.goal!=goal||SupplyTerminal(d.phase)) continue;
        const uint32 amount=d.quantity-d.deposited;
        if(d.phase=="mailed") mailed+=amount;
        else if(d.mail) collected+=amount;
        else reserved+=amount;
    }
}
void PlayerbotGuildSupplies::RecordDeposit(uint32 guild,uint32 actor,uint32 entry,uint32 count) {
    auto found=state_->deliveries.find(state_->depositing);if(found==state_->deliveries.end()) return;
    auto& d=found->second;
    if(!count||d.phase!="carried"||d.guild!=guild||d.carrier!=actor||d.entry!=entry||count!=state_->depositCount||count>d.quantity-d.deposited) return;
    // Called INSIDE native bank transaction. Restart cannot lose the proof or
    // credit an attempt that did not commit. Donor remains distinct from courier.
    CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET phase=IF(deposited_quantity+%u>=quantity,'completed','carried'),deposited_quantity=deposited_quantity+%u,blocker='',updated_at=%u WHERE delivery_id=%llu AND phase='carried' AND deposited_quantity=%u",count,count,uint32(time(nullptr)),(unsigned long long)d.id,d.deposited);
}
bool PlayerbotGuildSupplies::ReadDeliveryJob(uint64_t id,uint32_t actor,LivingActivity::GuildDeliveryJob& job,std::string& blocker) const {
    job={};blocker="guild_delivery_projection_pending";
    if(!sLivingActivityCoordinator.OnWorldThread() || !state_->ready)return false;
    const auto found=state_->deliveries.find(id);if(found==state_->deliveries.end())return false;
    const auto& d=found->second;
    if(d.carrier!=actor || (d.phase!="carried" && d.phase!="mailed") || !d.entry) {
        blocker="guild_delivery_native_item_leg_required";return false;
    }
    job={d.id,d.guild,d.donor,d.entry,d.quantity,d.mail,d.goal,false};
    if(!LivingActivity::ValidGuildDeliveryJob(job)){blocker="guild_delivery_native_identity_invalid";job={};return false;}
    blocker.clear();return true;
}
bool PlayerbotGuildSupplies::ReadManagedDeposit(const LivingActivity::Task& task,LivingActivity::GuildDepositQuote& q,
    std::string& blocker) const {
    using namespace LivingActivity;
    q={};auto reject=[&](const char* why){blocker=why;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !state_->ready || !ValidateGuildDeliveryTask(task,blocker) ||
        !IsManagedGuildDelivery(task) || !DecodeGuildDeliveryJob(task.checkpoint.data,q.job,blocker))
        return reject("guild_delivery_saved_task_unavailable");
    const auto found=state_->deliveries.find(q.job.delivery);
    if(found==state_->deliveries.end())return reject("guild_delivery_projection_pending");
    const auto& d=found->second;
    if(d.guild!=q.job.guild || d.goal!=q.job.goal || d.donor!=q.job.donor || d.carrier!=task.actor ||
        d.entry!=q.job.entry || d.quantity!=q.job.quantity || d.mail!=q.job.incomingMail || q.job.money)
        return reject("guild_delivery_native_identity_changed");
    if(d.phase!="carried" || d.deposited>=d.quantity)return reject("guild_delivery_not_carried");
    auto* guild=sGuildMgr.GetGuildById(d.guild);
    const auto enabled=state_->enabled.find(d.guild);
    if(!guild || enabled==state_->enabled.end() || !enabled->second || !sGuildGovernance.Allows(guild,"supplies"))
        return reject("guild_delivery_automation_paused");
    if(!guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,task.actor)))return reject("guild_delivery_member_departed");
    if(!MayDeposit(guild,task.actor))return reject("guild_delivery_deposit_permission_revoked");
    const auto goal=state_->goals.find(d.goal);
    if(goal==state_->goals.end() || goal->second.guild!=d.guild || goal->second.money)
        return reject("guild_delivery_goal_cancelled");
    q.actor=task.actor;q.item=d.item;q.deposited=d.deposited;
    q.goalTarget=goal->second.required;q.goalReserved=goal->second.reserved;
    const auto counts=guild->GetBankItemCounts();const auto entry=counts.find(d.entry);
    q.bankCount=entry==counts.end()?0:entry->second;
    q.amount=std::min(255u,std::min(d.quantity-d.deposited,
        SupplyOutstanding(q.goalTarget,q.bankCount,q.goalReserved,0)));
    if(!q.amount)return reject("guild_delivery_target_already_satisfied");
    blocker.clear();return true;
}
bool PlayerbotGuildSupplies::AllowsManagedClaim(const LivingActivity::ResourceClaim& claim) const {
    using namespace LivingActivity;
    if(!sLivingActivityCoordinator.OnWorldThread() || claim.copper || claim.state!="held" || !claim.quantity ||
        (claim.location!="bags" && claim.location!="mail"))return false;
    const auto task=sLivingActivityCoordinator.ReadSavedTask(claim.task);GuildDeliveryJob job;std::string why;
    if(!task || task->actor!=claim.actor || !IsManagedGuildDelivery(*task) || !ValidateGuildDeliveryTask(*task,why) ||
        !DecodeGuildDeliveryJob(task->checkpoint.data,job,why) || job.money || job.entry!=claim.itemEntry)return false;
    const auto found=state_->deliveries.find(job.delivery);
    if(found==state_->deliveries.end())return false;
    const auto& d=found->second;
    if(d.carrier!=claim.actor || d.guild!=job.guild || d.goal!=job.goal ||
        d.donor!=job.donor || d.mail!=job.incomingMail || d.quantity!=job.quantity || d.deposited>=d.quantity ||
        claim.quantity>d.quantity-d.deposited)return false;
    if(claim.location=="mail") {
        if(d.phase!="mailed" || !d.mail || claim.nativeReference!=d.mail || d.item!=claim.itemGuid ||
            d.deposited || claim.quantity!=d.quantity)return false;
    } else if(d.phase!="carried" || claim.nativeReference || (!d.mail && d.item!=claim.itemGuid))return false;
    // The legacy entry-wide reservation is waived only for its exact saved
    // owner. Another delivery of the same entry remains conservatively held.
    for(const auto& other:state_->deliveries) if(other.first!=d.id && !SupplyTerminal(other.second.phase) &&
        other.second.carrier==claim.actor && other.second.entry==claim.itemEntry)return false;
    return true;
}
bool PlayerbotGuildSupplies::ReadManagedMail(const LivingActivity::Task& task,LivingActivity::ResourceClaim& claim,
    std::string& blocker) const {
    using namespace LivingActivity;
    claim={};GuildDeliveryJob job,native;auto reject=[&](const char* why){blocker=why;return false;};
    if(!sLivingActivityCoordinator.OnWorldThread() || !state_->ready || !IsManagedGuildDelivery(task) ||
        !ValidateGuildDeliveryTask(task,blocker) || !DecodeGuildDeliveryJob(task.checkpoint.data,job,blocker) ||
        !ReadDeliveryJob(job.delivery,task.actor,native,blocker))return false;
    if(job.money || !job.incomingMail || EncodeGuildDeliveryJob(job)!=EncodeGuildDeliveryJob(native))
        return reject("guild_delivery_mail_identity_changed");
    const auto& d=state_->deliveries.at(job.delivery);
    if(d.phase!="mailed")return reject("guild_delivery_mail_already_collected");
    if(d.deposited || !d.item)return reject("guild_delivery_mail_credit_requires_reconciliation");
    // Collection is personal custody of an already accepted attachment, not
    // permission to deposit. A revoked guild duty must not erase owned mail.
    auto* actor=Online(task.actor);auto* mail=actor?actor->GetMail(d.mail):nullptr;
    if(!mail || mail->state==MAIL_STATE_DELETED || mail->sender!=d.donor ||
        mail->receiverGuid!=actor->GetObjectGuid() || mail->COD || mail->expire_time<=time(nullptr))
        return reject("guild_delivery_native_mail_changed");
    claim.task=task.id;claim.actor=task.actor;claim.itemGuid=d.item;claim.itemEntry=d.entry;
    claim.quantity=d.quantity;claim.state="held";claim.location="mail";claim.nativeReference=d.mail;
    blocker.clear();return true;
}
bool PlayerbotGuildSupplies::BeginManagedDeposit(const LivingActivity::GuildDepositQuote& q) {
    if(!sLivingActivityCoordinator.OnWorldThread() || !LivingActivity::ValidGuildDepositQuote(q) ||
        state_->depositing || state_->mailing)return false;
    const auto found=state_->deliveries.find(q.job.delivery);
    if(found==state_->deliveries.end())return false;
    const auto& d=found->second;
    if(d.carrier!=q.actor || d.guild!=q.job.guild || d.goal!=q.job.goal || d.donor!=q.job.donor ||
        d.entry!=q.job.entry || d.quantity!=q.job.quantity || d.deposited!=q.deposited ||
        d.mail!=q.job.incomingMail || d.phase!="carried")return false;
    state_->depositing=d.id;state_->depositCount=q.amount;return true;
}
void PlayerbotGuildSupplies::EndManagedDeposit() {
    state_->depositing=0;state_->depositCount=0;state_->load=0;
}
void PlayerbotGuildSupplies::RecordMoneyDeposit(uint32 guild,uint32 actor,uint32 copper) {
    auto found=state_->deliveries.find(state_->depositing);if(found==state_->deliveries.end()) return;
    const auto& d=found->second;
    if(!copper||d.entry||d.phase!="carried"||d.guild!=guild||d.donor!=actor||d.carrier!=actor||
        copper!=state_->depositCount||copper>d.quantity||d.deposited) return;
    CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET phase='completed',deposited_quantity=%u,blocker='',updated_at=%u WHERE delivery_id=%llu AND phase='carried' AND deposited_quantity=0",copper,uint32(time(nullptr)),(unsigned long long)d.id);
}
void PlayerbotGuildSupplies::RecordMailed(uint32 sender,uint32 receiver,Item* item,uint32 mail) {
    auto found=state_->deliveries.find(state_->mailing);if(found==state_->deliveries.end()||!item) return;
    auto& d=found->second;Player* source=Online(sender);
    if(!source||state_->mailReceiver!=receiver||d.donor!=sender||d.carrier!=sender||d.item!=item->GetGUIDLow()||d.quantity!=item->GetCount()||d.phase!="carried") return;
    // Mail row, removed inventory, real postage, attachment and journal share
    // this native mail transaction (no extra synthetic mail/inventory path).
    item->DeleteFromInventoryDB();item->SetOwnerGuid(ObjectGuid(HIGHGUID_PLAYER,receiver));item->SaveToDB();
    source->SaveInventoryAndGoldToDB();
    CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET carrier_guid=%u,mail_id=%u,phase='mailed',blocker='mail_delivery_delay',updated_at=%u WHERE delivery_id=%llu AND phase='carried'",receiver,mail,uint32(time(nullptr)),(unsigned long long)d.id);
    state_->mailRecorded=true;
    d.carrier=receiver;d.mail=mail;d.phase="mailed";d.operations=0;d.service=0;
    state_->PublishProtection();
    InvalidateItems(source);InvalidateItems(Online(receiver));
}
void PlayerbotGuildSupplies::RecordCollected(uint32 receiver,uint32 mail,uint32 item,uint32 count) {
    for(auto& pair:state_->deliveries) {auto& d=pair.second;
        if(d.carrier!=receiver||d.mail!=mail||d.item!=item||d.phase!="mailed"||count!=d.quantity-d.deposited) continue;
        CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET phase='carried',blocker='',updated_at=%u WHERE delivery_id=%llu AND phase='mailed'",uint32(time(nullptr)),(unsigned long long)d.id);
        Player* carrier=Online(receiver);InvalidateItems(carrier);
        if(Bot(carrier)) for(auto* held:Inventory(carrier))
            if(held&&held->GetEntry()==d.entry) state_->protectedItems.insert(held->GetGUIDLow());
        state_->PublishProtection();
        state_->load=0;break;
    }
}
void PlayerbotGuildSupplies::Update() {
    auto& s=*state_;const uint32 now=uint32(time(nullptr));
    if(now<s.next) return;s.next=now+2;
    if(!s.ready) {
        if(now<s.check) return;s.check=now+60;
        s.ready=bool(CharacterDatabase.PQuery("SHOW TABLES LIKE 'guild_society_supply_delivery'"))&&
            bool(CharacterDatabase.PQuery("SHOW COLUMNS FROM guild_society_supply_execution LIKE 'money_enabled'"));
        if(!s.ready) return;
        auto services=WorldDatabase.PQuery("SELECT g.guid,g.id,g.map,g.position_x,g.position_y,g.position_z,t.type FROM gameobject g JOIN gameobject_template t ON t.entry=g.id WHERE g.map IN (0,1,530) AND (t.type=%u OR (t.type=%u AND EXISTS (SELECT 1 FROM gameobject b JOIN gameobject_template bt ON bt.entry=b.id WHERE bt.type=%u AND b.map=g.map AND ABS(b.position_x-g.position_x)<600 AND ABS(b.position_y-g.position_y)<600))) ORDER BY t.type DESC,g.guid",uint32(GAMEOBJECT_TYPE_GUILD_BANK),uint32(GAMEOBJECT_TYPE_MAILBOX),uint32(GAMEOBJECT_TYPE_GUILD_BANK));
        if(services) do {Field* f=services->Fetch();Service p;p.guid=f[0].GetUInt32();p.entry=f[1].GetUInt32();p.map=f[2].GetUInt32();p.x=f[3].GetFloat();p.y=f[4].GetFloat();p.z=f[5].GetFloat();p.type=f[6].GetUInt32();s.services.push_back(p);} while(services->NextRow());
        // Cache only vendors/bankers around real guild-bank hubs, once at load.
        auto maintenance=WorldDatabase.PQuery("SELECT c.guid,c.id,c.map,c.position_x,c.position_y,c.position_z,t.NpcFlags FROM creature c JOIN creature_template t ON t.Entry=c.id WHERE c.map IN (0,1) AND (t.NpcFlags & %u)<>0 AND EXISTS (SELECT 1 FROM gameobject g JOIN gameobject_template gt ON gt.entry=g.id WHERE gt.type=%u AND g.map=c.map AND ABS(g.position_x-c.position_x)<600 AND ABS(g.position_y-c.position_y)<600)",uint32(UNIT_NPC_FLAG_VENDOR|UNIT_NPC_FLAG_BANKER),uint32(GAMEOBJECT_TYPE_GUILD_BANK));
        if(maintenance) do {Field* f=maintenance->Fetch();for(uint32 flag:{uint32(UNIT_NPC_FLAG_VENDOR),uint32(UNIT_NPC_FLAG_BANKER)}) {
            if(!(f[6].GetUInt32()&flag)) continue;
            Service p;p.guid=f[0].GetUInt32();p.entry=f[1].GetUInt32();p.map=f[2].GetUInt32();p.x=f[3].GetFloat();p.y=f[4].GetFloat();p.z=f[5].GetFloat();p.type=100000+flag;s.services.push_back(p);
        }} while(maintenance->NextRow());
    }
    // Terrain metadata is populated incrementally, never a world-tick scan of
    // every mailbox or bot. No raw pointers are kept beyond the update.
    static size_t serviceCursor=0;
    for(uint32 n=0;n<8&&serviceCursor<s.services.size();++n,++serviceCursor) {
        auto& p=s.services[serviceCursor];p.zone=sTerrainMgr.GetZoneId(p.map,p.x,p.y,p.z);
    }
    if(now>=s.load) s.Reload(now);
    auto next=s.deliveries.upper_bound(s.workCursor);if(next==s.deliveries.end()) next=s.deliveries.begin();
    for(uint32 visits=0;visits<std::min(size_t(8),s.deliveries.size());++visits) {
        auto& d=(next++)->second;if(next==s.deliveries.end()) next=s.deliveries.begin();s.workCursor=d.id;
        if(SupplyTerminal(d.phase)) continue;
        const LivingActivity::GuildDeliveryJob managed{d.id,d.guild,d.donor,d.entry,d.quantity,d.mail,d.goal,!d.entry};
        if(sLivingActivityCoordinator.OwnsGuildDelivery(d.carrier,managed)) {
            // Admission retires the legacy executor, including during a pause,
            // restart, cancellation or delayed native acknowledgement.
            // Releasing only the OLD lease cannot clear the new owner's route.
            s.Release(d);continue;
        }
#ifdef LIVING_ISOLATED_NATIVE_TESTS
        if(sLivingActivityCoordinator.IsolatedGameplayActor(d.carrier))continue;
#endif
        Player* p=Online(d.carrier);Guild* guild=sGuildMgr.GetGuildById(d.guild);
        if(!guild||!s.enabled[d.guild]||!sGuildGovernance.Allows(guild,"supplies")) {s.Block(d,"supply_automation_paused",now);continue;}
        if(!guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,d.carrier))) {s.Block(d,"recipient_no_longer_member",now);continue;}
        const char* safety=SafetyBlocker(p);
        if(*safety) {
            d.last=0;
            // A short cast pauses the trip; releasing ownership here allowed
            // unrelated town/buff movement to repeatedly steal its route.
            // All other safety blockers still release immediately.
            if(std::string(safety)=="active_spell_or_channel" &&
                sPlayerbotRendezvousManager.HasPartyActivityLease(d.lease)) {
                if(d.blocker!=safety) {d.blocker=safety;CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET blocker='%s',updated_at=%u WHERE delivery_id=%llu",safety,now,(unsigned long long)d.id);}
            } else s.Block(d,safety,now);
            continue;
        }
        if(now<d.retry) continue;
        if(!LivingServiceExecution::Prepare(p)) continue;
        d.active+=d.last?std::min(now-d.last,32u):0;d.last=now;
        auto goal=s.goals.find(d.goal);
        if(goal==s.goals.end()||goal->second.guild!=d.guild) {s.Finish(d,"cancelled","goal_cancelled_items_preserved",now);continue;}
        if(goal->second.money) {
            if(d.entry||d.mail||d.phase!="carried") {s.Block(d,"currency_record_requires_review",now,true);continue;}
            if(!s.moneyEnabled.count(d.guild)) {s.Block(d,"money_donations_disabled",now);continue;}
            if(!guild->GetPurchasedTabs()) {s.Block(d,"no_guild_bank_tabs",now,true);continue;}
            if(guild->GetGuildBankMoney()>=goal->second.required) {
                CharacterDatabase.PExecute("UPDATE guild_society_supply_goal SET state='completed',updated_at=%u WHERE goal_id='%s' AND guild_id=%u AND request_kind='money' AND state='active'",now,d.goal.c_str(),d.guild);
                s.Finish(d,"cancelled","bank_funding_target_met",now);continue;
            }
            if(auto* bank=s.Reach(p,d,false,now)) {
                auto current=CharacterDatabase.PQuery("SELECT required_quantity FROM guild_society_supply_goal WHERE goal_id='%s' AND guild_id=%u AND request_kind='money' AND state='active'",d.goal.c_str(),d.guild);
                if(!current) {s.Finish(d,"cancelled","fundraiser_no_longer_active",now);continue;}
                const uint32 target=current->Fetch()[0].GetUInt32();
                const uint32 needed=guild->GetGuildBankMoney()>=target?0:target-uint32(guild->GetGuildBankMoney());
                const uint32 amount=std::min(needed,std::min(d.quantity,DonationAllowance(p,now)));
                if(!amount) {s.Finish(d,"cancelled","personal_budget_or_target_changed",now);continue;}
                if(++d.operations>2) {s.Block(d,"bank_money_operation_blocked",now,true);continue;}
                s.depositing=d.id;s.depositCount=amount;
                WorldPacket packet(CMSG_GUILD_BANK_DEPOSIT_MONEY);packet<<bank->GetObjectGuid()<<amount;
                p->GetSession()->HandleGuildBankDepositMoney(packet);
                s.depositing=0;s.depositCount=0;s.Release(d);s.load=0;
                if(guild->GetGuildBankMoney()>=target)
                    CharacterDatabase.PExecute("UPDATE guild_society_supply_goal SET state='completed',updated_at=%u WHERE goal_id='%s' AND guild_id=%u AND request_kind='money' AND state='active'",now,d.goal.c_str(),d.guild);
            }
            continue;
        }
        if(d.phase=="mailed") {
            Mail* mail=p->GetMail(d.mail);
            if(!mail||mail->state==MAIL_STATE_DELETED) {s.Block(d,"mail_unavailable_review",now);continue;}
            if(mail->COD||mail->sender!=d.donor) {s.Block(d,"mail_identity_mismatch",now);continue;}
            if(mail->deliver_time>time(nullptr)) {d.active=0;s.Block(d,"mail_delivery_delay",now);continue;}
            if(!p->GetMItem(d.item)) {s.Block(d,"attachment_unavailable_review",now,true);continue;}
            if(!s.MakeCollectionRoom(p,d,now)) continue;
            if(auto* mailbox=s.Reach(p,d,true,now)) {
                if(!p->GetMItem(d.item)) {s.Block(d,"attachment_unavailable_review",now,true);continue;}
                const auto claims=sLivingActivityCoordinator.ResourceReservations().Inspect();
                const auto* attachment=p->GetMItem(d.item);
                if(!claims || claims->UnreservedItem(p->GetGUIDLow(),d.item,attachment->GetEntry(),attachment->GetCount())!=attachment->GetCount())
                {s.Block(d,"mail_owned_by_saved_task",now);continue;}
                if(++d.operations>2) {s.Block(d,"mail_collection_blocked",now,true);continue;}
                WorldPacket packet(CMSG_MAIL_TAKE_ITEM);packet<<mailbox->GetObjectGuid()<<d.mail<<d.item;
                p->GetSession()->HandleMailTakeItem(packet);s.Release(d);s.load=0;
            }
            continue;
        }
        if(d.phase!="carried") continue;
        Item* item=nullptr;
        for(auto* candidate:Inventory(p)) if(candidate&&candidate->GetEntry()==d.entry&&
            (d.mail||candidate->GetGUIDLow()==d.item)) {item=candidate;break;}
        if(!item) {s.Block(d,"reserved_item_missing_review",now,true);continue;}
        const auto bank=guild->GetBankItemCounts();auto stock=bank.find(d.entry);
        const uint32 need=SupplyOutstanding(goal->second.required,stock==bank.end()?0:stock->second,goal->second.reserved,0);
        if(!need) {s.Finish(d,"cancelled","stock_already_satisfied_items_preserved",now);continue;}
        const uint32 amount=std::min(std::min(item->GetCount(),d.quantity-d.deposited),need);
        if(MayDeposit(guild,d.carrier)) {
            const int32 tab=guild->FindSupplyDepositTab(d.carrier,item,amount);
            if(tab<0) {s.Block(d,"bank_full_or_item_ineligible",now,true);continue;}
            if(s.Reach(p,d,false,now)) {
                auto current=CharacterDatabase.PQuery("SELECT required_quantity,reserved_quantity FROM guild_society_supply_goal WHERE goal_id='%s' AND guild_id=%u AND state='active'",d.goal.c_str(),d.guild);
                if(!current) {s.Finish(d,"cancelled","goal_cancelled_items_preserved",now);continue;}
                const uint32 finalAmount=std::min(255u,std::min(amount,SupplyOutstanding(current->Fetch()[0].GetUInt32(),stock==bank.end()?0:stock->second,current->Fetch()[1].GetUInt32(),0)));
                if(!finalAmount) continue;
                if(++d.operations>2) {s.Block(d,"bank_operation_blocked",now,true);continue;}
                s.depositing=d.id;s.depositCount=finalAmount;
                guild->MoveFromCharToBank(p,item->GetBagSlot(),item->GetSlot(),uint8(tab),255,finalAmount);
                s.depositing=0;s.depositCount=0;s.Release(d);s.load=0;
            }
        } else if(d.mail) s.Block(d,"deposit_permission_revoked",now,true);
        else {
            Player* recipient=nullptr;float distance=1e30f;
            for(uint32 guid:sRandomPlayerbotMgr.GetChatBotGuids()) {
                Player* candidate=Online(guid);
                // Receiving ordinary mail does not interrupt the recipient's
                // current party/combat/event. Safe() still gates collection and
                // deposit after receipt; keep the one-delivery courier bound.
                if(!Bot(candidate)||!candidate->IsInWorld()||!candidate->GetMap()||candidate==p||candidate->GetGuildId()!=d.guild||s.Busy(guid)||
                    !MayDeposit(guild,guid)||guild->FindSupplyDepositTab(guid,item,amount)<0||candidate->GetMailSize()>=50) continue;
                const Service* bankService=s.Destination(candidate,false);if(!bankService) continue;
                float score=candidate->GetMapId()==bankService->map?candidate->GetDistance(bankService->x,bankService->y,bankService->z):100000;
                if(score<distance) {recipient=candidate;distance=score;}
            }
            if(!recipient) {
                s.Block(d,"no_available_deposit_recipient",now);
                d.retry=now+30;d.last=0; // Recheck capacity/availability, not a failed route.
                continue;
            }
            if(amount<d.quantity) {
                if(!CharacterDatabase.DirectPExecute("UPDATE guild_society_supply_delivery SET quantity=%u,updated_at=%u WHERE delivery_id=%llu AND phase='carried'",amount,now,(unsigned long long)d.id)) continue;
                d.quantity=amount;
            }
            if(p->GetMoney()<30) {s.Block(d,"insufficient_postage",now,true);continue;}
            // Preparation owns its service leg until actual capacity exists.
            // Never return to the mailbox midway through a storage trip.
            if(LivingServiceExecution::NeedsSplitPreparation(item->GetCount(),amount,EmptyBagSlot(p)!=0)) {
                s.MakeCollectionRoom(p,d,now,true);continue;
            }
            if(s.Reach(p,d,true,now)) {
                if(!CharacterDatabase.PQuery("SELECT goal_id FROM guild_society_supply_goal WHERE goal_id='%s' AND guild_id=%u AND state='active'",d.goal.c_str(),d.guild)) {s.Finish(d,"cancelled","goal_cancelled_items_preserved",now);continue;}
                if(d.operations>=2) {s.Block(d,"mail_send_blocked",now,true);continue;}
                if(item->GetCount()>amount) {
                    // Native stack splitting, saved with the new attachment
                    // reference before sending. Never create extra quantities.
                    uint16 empty=EmptyBagSlot(p);
                    if(!empty) {s.MakeCollectionRoom(p,d,now,true);continue;}
                    const uint32 before=item->GetCount();p->SplitItem(item->GetPos(),empty,amount);
                    Item* split=p->GetItemByPos(empty);
                    if(!split||split->GetEntry()!=d.entry||split->GetCount()!=amount||item->GetCount()!=before-amount) {s.Block(d,"stack_split_unavailable",now,true);continue;}
                    if(!CharacterDatabase.BeginTransaction()) {s.Block(d,"split_persistence_requires_review",now,true);continue;}
                    p->SaveInventoryAndGoldToDB();
                    CharacterDatabase.PExecute("UPDATE guild_society_supply_delivery SET item_guid=%u,updated_at=%u WHERE delivery_id=%llu AND phase='carried'",split->GetGUIDLow(),now,(unsigned long long)d.id);
                    if(!CharacterDatabase.CommitTransactionDirect()) {s.Block(d,"split_persistence_requires_review",now,true);continue;}
                    d.item=split->GetGUIDLow();item=split;
                }
                ++d.operations;
                s.mailing=d.id;s.mailReceiver=recipient->GetGUIDLow();s.mailRecorded=false;
                p->MoveItemFromInventory(item->GetBagSlot(),item->GetSlot(),true);p->ModifyMoney(-30);
                MailDraft draft("Guild supply delivery", "For the guild's "+std::string(item->GetProto()->Name1)+" supply request ("+d.goal+"). Please deposit these items in the guild bank.");
                draft.AddItem(item);
                draft.SendMailTo(MailReceiver(recipient),MailSender(p),MAIL_CHECK_MASK_HAS_BODY,
                    p->GetSession()->GetAccountId()==recipient->GetSession()->GetAccountId()?0:sWorld.getConfig(CONFIG_UINT32_MAIL_DELIVERY_DELAY));
                s.mailing=0;s.mailReceiver=0;s.Release(d);s.load=0;
                if(!s.mailRecorded) s.Block(d,"mail_transaction_requires_review",now,true);
            }
        }
    }
    // One candidate per sweep, maximum one held delivery per bot. Claim only
    // genuinely spare stacks; gathering/crafting/purchases are a later phase.
    const auto bots=sRandomPlayerbotMgr.GetChatBotGuids();if(bots.empty()||s.deliveries.size()>=128) return;
    uint32 guid=0;Player* bot=nullptr;
    for(uint32 n=0;n<10;++n) {
        guid=*std::next(bots.begin(),s.cursor++%bots.size());Player* candidate=Online(guid);
        if(candidate&&s.enabled[candidate->GetGuildId()]&&!s.Busy(guid)&&Safe(candidate)) {bot=candidate;break;}
    }
    if(!bot) return;
    Guild* guild=sGuildMgr.GetGuildById(bot->GetGuildId());if(!guild||!guild->GetPurchasedTabs()||!sGuildGovernance.Allows(guild,"supplies")) return;
    auto goals=CharacterDatabase.PQuery("SELECT goal_id,item_entry,required_quantity,reserved_quantity,request_kind FROM guild_society_supply_goal WHERE guild_id=%u AND state='active' AND provenance IN ('human_request','event_requirement','profession_requirement','equipment_requirement') ORDER BY priority DESC,created_at LIMIT 16",guild->GetId());
    if(!goals) return;
    const auto bank=guild->GetBankItemCounts();
    do {
        Field* f=goals->Fetch();const uint32 entry=f[1].GetUInt32();uint32 transit=0;
        if(f[4].GetCppString()=="money") {
            if(!s.moneyEnabled.count(guild->GetId())||entry) continue;
            std::string goalId=f[0].GetString();if(!Id(goalId)) continue;
            const uint32 target=f[2].GetUInt32();
            if(guild->GetGuildBankMoney()>=target) {
                CharacterDatabase.PExecute("UPDATE guild_society_supply_goal SET state='completed',updated_at=%u WHERE goal_id='%s' AND guild_id=%u AND request_kind='money' AND state='active'",now,goalId.c_str(),guild->GetId());
                continue;
            }
            for(const auto& pair:s.deliveries) {const auto& d=pair.second;if(d.guild==guild->GetId()&&!d.entry&&!SupplyTerminal(d.phase)) transit+=d.quantity-d.deposited;}
            const uint32 need=SupplyOutstanding(target,uint32(guild->GetGuildBankMoney()),0,transit);
            if(!need) continue;
            const uint32 amount=std::min(need,DonationAllowance(bot,now));if(!amount) continue;
            if(!CharacterDatabase.DirectPExecute("INSERT INTO guild_society_supply_delivery (guild_id,goal_id,donor_guid,carrier_guid,item_guid,item_entry,quantity,created_at,updated_at) VALUES (%u,'%s',%u,%u,0,0,%u,%u,%u)",guild->GetId(),goalId.c_str(),guid,guid,amount,now,now)) return;
            s.Reload(now);return;
        }
        for(const auto& pair:s.deliveries) {const auto& d=pair.second;if(d.guild==guild->GetId()&&d.entry==entry&&!SupplyTerminal(d.phase)) transit+=d.quantity-d.deposited;}
        auto stock=bank.find(entry);const uint32 need=SupplyOutstanding(f[2].GetUInt32(),stock==bank.end()?0:stock->second,f[3].GetUInt32(),transit);
        if(!need) continue;
        for(auto* item:Inventory(bot)) {
            if(!item||item->GetEntry()!=entry) continue;
            bool reserved=false;const auto disposition=sPlayerbotInventoryPressure.Classify(bot,item,&reserved);
            const bool spare=disposition==LivingWowItemDisposition::Vendor||disposition==LivingWowItemDisposition::Auction;
            if(!SupplyClaimable(spare,item->CanBeTraded()&&!item->IsConjuredConsumable(),reserved,item->GetCount(),need)) continue;
            if(CharacterDatabase.PQuery("SELECT reservation_id FROM organic_economy_reservation WHERE character_guid=%u AND (item_guid=%u OR (item_guid IS NULL AND item_entry=%u AND quantity>0)) AND expires_at>NOW() LIMIT 1",guid,item->GetGUIDLow(),entry)) continue;
            std::string goalId=f[0].GetString();if(!Id(goalId)) continue;
            if(!CharacterDatabase.DirectPExecute("INSERT INTO guild_society_supply_delivery (guild_id,goal_id,donor_guid,carrier_guid,item_guid,item_entry,quantity,created_at,updated_at) VALUES (%u,'%s',%u,%u,%u,%u,%u,%u,%u)",guild->GetId(),goalId.c_str(),guid,guid,item->GetGUIDLow(),entry,std::min(item->GetCount(),need),now,now)) return;
            s.Reload(now);InvalidateItems(bot);return;
        }
    } while(goals->NextRow());
}
