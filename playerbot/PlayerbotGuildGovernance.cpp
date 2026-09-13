#include "playerbot/playerbot.h"
#include "PlayerbotGuildGovernance.h"
#include "PlayerbotGuildSupplies.h"
#include "LivingActivityCoordinator.h"
#include "LivingGuildProcurementProjection.h"
#include "PlayerbotGuildEventExecutor.h"
#include "GuildEventPolicy.h"
#include "Guilds/Guild.h"
#include "Guilds/GuildMgr.h"
#include "Chat/Chat.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "Globals/ObjectAccessor.h"
#include <openssl/sha.h>
#include <iomanip>
#include <sstream>

using namespace livingguild;
namespace {
std::string Esc(std::string value) { CharacterDatabase.escape_string(value); return value; }
std::string Wire(std::string value,size_t limit=100) {
    for(char& c:value) if(static_cast<unsigned char>(c)<32||c=='|') c=' ';
    return value.substr(0,limit);
}
std::string Hash(const std::string& value) {
    unsigned char bytes[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(value.data()),value.size(),bytes);
    std::ostringstream out; for(auto c:bytes) out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c);
    return out.str();
}
}
PlayerbotGuildGovernance& PlayerbotGuildGovernance::instance() { static PlayerbotGuildGovernance value;return value; }
bool PlayerbotGuildGovernance::Ready() {
    if(ready_) return true;
    const uint32 now=uint32(time(nullptr));
    if(now<readyCheck_) return false;
    readyCheck_=now+60;
    // The migration writes its sentinel last, after every table/column exists.
    // A partial migration cannot enable the service just by creating policy.
    ready_=bool(CharacterDatabase.PQuery("SELECT version FROM guild_society_schema WHERE version=5"));
    return ready_;
}
Policy* PlayerbotGuildGovernance::Load(Guild* guild) {
    if(!guild||!Ready()) return nullptr;
    const uint32 id=guild->GetId(),leader=guild->GetLeaderGuid().GetCounter(),now=uint32(time(nullptr));
    auto found=policies_.find(id);
    if(found==policies_.end()) {
        auto row=CharacterDatabase.PQuery("SELECT leader_guid,revision,recruitment,events,supplies,promotions,member_hours,veteran_days,member_credits,veteran_credits,promotion_cooldown_hours,daily_promotion_limit FROM guild_society_policy WHERE guild_id=%u",id);
        Policy p;
        if(row) {
            Field* f=row->Fetch(); p.leader=f[0].GetUInt32();p.revision=f[1].GetUInt32();
            p.recruitment=f[2].GetBool();p.events=f[3].GetBool();p.supplies=f[4].GetBool();p.promotions=f[5].GetBool();
            p.memberHours=f[6].GetUInt32();p.veteranDays=f[7].GetUInt32();p.memberCredits=f[8].GetUInt32();
            p.veteranCredits=f[9].GetUInt32();p.cooldownHours=f[10].GetUInt32();p.dailyLimit=f[11].GetUInt32();
        } else {
            p.leader=leader;
            const bool bot=sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid()));
            p.recruitment=p.events=p.supplies=bot;
            if(!CharacterDatabase.DirectPExecute("INSERT IGNORE INTO guild_society_policy (guild_id,leader_guid,recruitment,events,supplies,updated_at) VALUES (%u,%u,%u,%u,%u,%u)",id,leader,bot,bot,bot,now)) return nullptr;
        }
        found=policies_.emplace(id,p).first;
    }
    Policy& p=found->second;
    if(p.leader!=leader) {
        // Human ownership never inherits a previous leader's delegation.
        const bool bot=sPlayerbotAIConfig.IsInRandomAccountList(sObjectMgr.GetPlayerAccountIdByGUID(guild->GetLeaderGuid()));
        if(!CharacterDatabase.DirectPExecute("UPDATE guild_society_policy SET leader_guid=%u,revision=revision+1,recruitment=%u,events=%u,supplies=%u,promotions=0,updated_at=%u WHERE guild_id=%u",leader,bot,bot,bot,now,id)) return nullptr;
        p.leader=leader;++p.revision;p.recruitment=p.events=p.supplies=bot;p.promotions=false;
    }
    return &p;
}
uint32 PlayerbotGuildGovernance::Permissions(Player* actor,Guild* guild) const {
    if(!actor||!guild||actor->GetGuildId()!=guild->GetId()) return 0;
    if(actor->GetObjectGuid()==guild->GetLeaderGuid()) return Recruit|Promote|Demote|Configure|Delegate;
    MemberSlot* member=guild->GetMemberSlot(actor->GetObjectGuid());
    if(!member) return 0;
    return (guild->HasRankRight(member->RankId,GR_RIGHT_INVITE)?uint32(Recruit):0u)|
        (guild->HasRankRight(member->RankId,GR_RIGHT_PROMOTE)?uint32(Promote):0u)|
        (guild->HasRankRight(member->RankId,GR_RIGHT_DEMOTE)?uint32(Demote):0u)|
        (guild->HasRankRight(member->RankId,GR_RIGHT_MODIFY_GUILD_INFO)?uint32(Configure):0u);
}
Policy PlayerbotGuildGovernance::ReadPolicy(Guild* guild) {
    if(Policy* policy=Load(guild)) return *policy;
    return Policy();
}
void PlayerbotGuildGovernance::ConfigureServer(bool global,const std::set<uint32>& canaries,bool recruitment,bool events,bool supplies) {
    global_=global;canaries_=canaries;recruitment_=recruitment;events_=events;supplies_=supplies;
}
bool PlayerbotGuildGovernance::Allows(Guild* guild,const std::string& duty,Player* actor) {
    if(!guild||!active_||(!global_&&!canaries_.count(guild->GetId()))) return false;
    if((duty=="recruitment"&&!recruitment_)||(duty=="events"&&!events_)||
        (duty=="supplies"&&!supplies_)||duty=="promotions") return false;
    Policy* p=Load(guild);
    if(!p||!Delegated(*p,duty)) return false;
    // Supply participation is permission-based, not a named officer duty.
    // Native per-tab bank rights are checked again by the delivery executor.
    if(duty=="supplies") return !actor||guild->GetMemberSlot(actor->GetObjectGuid());
    const uint32 permission=duty=="recruitment"?Recruit:duty=="promotions"?Promote:Configure;
    if(actor && !(Permissions(actor,guild)&permission)) return false;
    if(actor && actor->GetObjectGuid()==guild->GetLeaderGuid()) return true;
    const std::string namedDuty=duty=="recruitment"?"recruiter":duty=="supplies"?"quartermaster":"event_organizer";
    Duties& duties=officers_[guild->GetId()];const uint32 now=uint32(time(nullptr));
    if(now>=duties.refreshed) {
        duties.refreshed=now+60;duties.actors.clear();
        auto officers=CharacterDatabase.PQuery("SELECT duty,character_guid FROM guild_society_officer WHERE guild_id=%u AND state='active'",guild->GetId());
        if(officers) do {Field* f=officers->Fetch();duties.actors[f[0].GetString()]=f[1].GetUInt32();} while(officers->NextRow());
    }
    auto found=duties.actors.find(namedDuty);
    if(found==duties.actors.end()||(actor&&found->second!=actor->GetGUIDLow())) return false;
    MemberSlot* member=guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,found->second));
    // Revalidate native membership/rank even when the duty cache is fresh.
    return member&&guild->HasRankRight(member->RankId,permission==Recruit?GR_RIGHT_INVITE:GR_RIGHT_MODIFY_GUILD_INFO);
}
void PlayerbotGuildGovernance::Send(Player* actor,const std::string& text) {
    if(!actor||!actor->GetSession()) return;
    std::string payload="LWOWG2\t"+text;
    if(legacyActor_==actor->GetGUIDLow()) {
        const auto fields=Split(text);
        if(fields.size()<4||fields[0]!="RESULT") return;
        legacySucceeded_=fields[2]=="completed";
        const std::string reason=fields[3]=="saved"?"Guild event updated.":fields[3]+". Refresh the calendar and retry.";
        payload="LWOWG1\t"+std::string(legacySucceeded_?"ACK":"ERR")+"\t0\t"+Wire(reason);
    }
    if(payload.size()>254) return; // Every record is independently bounded.
    WorldPacket packet;
    ChatHandler::BuildChatPacket(packet,CHAT_MSG_WHISPER,payload.c_str(),LANG_ADDON,
        CHAT_TAG_NONE,actor->GetObjectGuid(),actor->GetName());
    actor->GetSession()->SendPacket(packet);
}
void PlayerbotGuildGovernance::RememberLegacySnapshot(Player* actor,uint32 token) {
    if(!actor||!actor->isRealPlayer()) return;
    Guild* guild=sGuildMgr.GetGuildById(actor->GetGuildId());
    if(Policy* p=Load(guild)) legacySnapshots_[actor->GetGUIDLow()]={guild->GetId(),token,p->revision,uint32(time(nullptr))+90};
}
bool PlayerbotGuildGovernance::HandleLegacy(Player* actor,const std::string& message) {
    if(!actor||!actor->isRealPlayer()) return false;
    legacyActor_=actor->GetGUIDLow();legacySucceeded_=false;
    const auto f=Split(message);uint32 token=0;
    auto found=legacySnapshots_.find(legacyActor_);
    auto reject=[&](const std::string& reason){Send(actor,"RESULT\tlegacy\trejected\t"+reason);legacyActor_=0;return false;};
    if(f.size()<3||message.size()>254||!Number(f[2],token)||found==legacySnapshots_.end()||
        found->second.guild!=actor->GetGuildId()||found->second.token!=token||found->second.expires<uint32(time(nullptr)))
        return reject("calendar_snapshot_expired");
    const std::string op="legacy:"+Hash(message).substr(0,24);
    const std::string revision=std::to_string(found->second.revision);
    std::string request;
    if(f.size()==5&&f[1]=="RSVP"&&Id(f[3])) {
        auto row=CharacterDatabase.PQuery("SELECT revision FROM guild_society_event WHERE guild_id=%u AND event_id='%s'",actor->GetGuildId(),f[3].c_str());
        if(!row) return reject("event_not_found");
        request="LWOWG2\tEVENT_RSVP\t"+op+"\t"+revision+"\t"+f[3]+"\t"+std::to_string(row->Fetch()[0].GetUInt32())+"\t"+f[4];
    } else if(f.size()==8&&f[1]=="CREATE") {
        // V1 cannot supply an objective. Only social/leveling definitions pass
        // the same V2 validator; quests/dungeons/supplies require an upgrade.
        request="LWOWG2\tEVENT_SAVE\t"+op+"\t"+revision+"\tcalendar:"+Hash(message).substr(0,24)+"\t0\t"+
            f[4]+"\t"+f[5]+"\t"+f[6]+"\t0\t"+f[3]+"\t"+f[7];
    } else return reject("malformed_request");
    if(request.size()>254) return reject("event_text_too_long");
    Handle(actor,request);
    legacyActor_=0;return legacySucceeded_;
}
void PlayerbotGuildGovernance::Snapshot(Player* actor,Guild* guild,Policy& p,const std::string& op,
    const std::string& section,uint32 offset,uint32 starts,uint32 ends,bool history) {
    const uint32 id=guild->GetId(),permissions=Permissions(actor,guild);
    // Read on request, not on the world tick. Native membership/rights decide
    // what is effective; a saved responsibility is never itself authority.
    auto leadership=[&]() {
        Send(actor,"LEADERS\t"+op);
        auto members=CharacterDatabase.PQuery("SELECT guid FROM guild_member WHERE guildid=%u ORDER BY `rank`,guid LIMIT 100",id);
        if(members) do {
            const uint32 guid=members->Fetch()[0].GetUInt32();
            MemberSlot* member=guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,guid));
            if(!member) continue;
            if(guid!=guild->GetLeaderGuid().GetCounter() &&
                !guild->HasRankRight(member->RankId,GR_RIGHT_PROMOTE) &&
                !guild->HasRankRight(member->RankId,GR_RIGHT_DEMOTE) &&
                !guild->HasRankRight(member->RankId,GR_RIGHT_MODIFY_GUILD_INFO)) continue;
            Send(actor,"LEADER\t"+op+"\t"+Wire(member->Name,24)+"\t"+Wire(guild->GetRankName(member->RankId),32));
        } while(members->NextRow());
        Send(actor,"DUTIES\t"+op);
        auto duties=CharacterDatabase.PQuery("SELECT duty,character_guid,state FROM guild_society_officer WHERE guild_id=%u ORDER BY duty LIMIT 3",id);
        if(duties) do {
            Field* f=duties->Fetch();const std::string duty=f[0].GetString();
            MemberSlot* member=guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,f[1].GetUInt32()));
            const uint32 right=duty=="recruiter"?GR_RIGHT_INVITE:GR_RIGHT_MODIFY_GUILD_INFO;
            const bool valid=member && guild->HasRankRight(member->RankId,right);
            const std::string state=valid?f[2].GetString():"ineligible";
            Send(actor,"DUTY\t"+op+"\t"+Wire(duty,32)+"\t"+std::to_string(f[1].GetUInt32())+"\t"+
                Wire(state,16)+"\t"+Wire(member?member->Name:"Former member",24));
        } while(duties->NextRow());
    };
    Send(actor,"STATE\t"+op+"\t"+std::to_string(p.revision)+"\t"+std::to_string(id)+"\t"+
        std::to_string(permissions)+"\t"+std::to_string(p.recruitment)+"\t"+std::to_string(p.events)+"\t"+
        std::to_string(p.supplies)+"\t"+std::to_string(p.promotions)+"\t"+Wire(guild->GetName(),48));
    Send(actor,"SUPPLYFEATURES\t"+op+"\t1");
    if(section.find("items:")==0) {
        const std::string query=section.substr(6);
        if(!(permissions&Configure)||query.size()<2||query.size()>48) {
            Send(actor,"RESULT\t"+op+"\trejected\titem_search_not_authorized_or_too_short");return;
        }
        // Literal substring, not SQL/LIKE syntax. Explicit UI searches only;
        // existing per-player request throttling and pagination remain in force.
        const char* digits="0123456789abcdef";std::string hex;
        for(unsigned char ch:query) {hex+=digits[ch>>4];hex+=digits[ch&15];}
        auto rows=WorldDatabase.PQuery("SELECT entry FROM item_template WHERE INSTR(LOWER(CONVERT(name USING utf8)) COLLATE utf8_general_ci,LOWER(CONVERT(0x%s USING utf8)) COLLATE utf8_general_ci)>0 ORDER BY name,entry LIMIT 6 OFFSET %u",hex.c_str(),offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            const uint32 entry=rows->Fetch()[0].GetUInt32();const auto* item=sObjectMgr.GetItemPrototype(entry);
            if(item) Send(actor,"ITEM\t"+op+"\t"+std::to_string(entry)+"\t"+Wire(item->Name1,64));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\t"+section+"\t"+std::to_string(count>5?offset+5:0));
    } else if(section.find("event_targets:")==0) {
        const std::string kind=section.substr(14);uint32 count=0;
        if(kind=="dungeon") {
            // Native five-player instance maps only; no raid or invented map.
            std::vector<const MapEntry*> maps;
            for(uint32 mapId=0;mapId<sMapStore.GetNumRows();++mapId) {
                const MapEntry* map=sMapStore.LookupEntry(mapId);
                if(map&&map->IsDungeon()&&!map->IsRaid()&&PlayerbotGuildEventExecutor::DungeonSupported(mapId)) maps.push_back(map);
            }
            for(size_t index=offset;index<maps.size()&&index<offset+6;++index) {
                if(++count>5) break;
                Send(actor,"OBJECTIVE\t"+op+"\tdungeon\t"+std::to_string(maps[index]->MapID)+"\t"+Wire(maps[index]->name[0],64));
            }
        } else if(kind=="quest") {
            auto rows=CharacterDatabase.PQuery("SELECT DISTINCT q.quest FROM character_queststatus q JOIN guild_member m ON m.guid=q.guid WHERE m.guildid=%u AND q.status IN (%u,%u) AND q.rewarded=0 ORDER BY q.quest LIMIT 6 OFFSET %u",id,uint32(QUEST_STATUS_COMPLETE),uint32(QUEST_STATUS_INCOMPLETE),offset);
            if(rows) do {
                if(++count>5) break;
                const uint32 questId=rows->Fetch()[0].GetUInt32();const Quest* quest=sObjectMgr.GetQuestTemplate(questId);
                if(quest) Send(actor,"OBJECTIVE\t"+op+"\tquest\t"+std::to_string(questId)+"\t"+Wire(quest->GetTitle(),64));
            } while(rows->NextRow());
        } else if(kind=="supply") {
            auto rows=CharacterDatabase.PQuery("SELECT DISTINCT item_entry FROM guild_society_supply_goal WHERE guild_id=%u AND state='active' AND provenance IN ('human_request','event_requirement','profession_requirement','equipment_requirement') ORDER BY item_entry LIMIT 6 OFFSET %u",id,offset);
            if(rows) do {
                if(++count>5) break;
                const uint32 itemId=rows->Fetch()[0].GetUInt32();const auto* item=sObjectMgr.GetItemPrototype(itemId);
                if(item) Send(actor,"OBJECTIVE\t"+op+"\tsupply\t"+std::to_string(itemId)+"\t"+Wire(item->Name1,64));
            } while(rows->NextRow());
        } else {Send(actor,"RESULT\t"+op+"\trejected\tunsupported_event_type");return;}
        Send(actor,"END\t"+op+"\t"+section+"\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="events") {
        const std::string filter=history?"":"AND state NOT IN ('completed','failed','cancelled') ";
        auto rows=CharacterDatabase.PQuery("SELECT event_id,revision,scheduled_at,COALESCE(ends_at,scheduled_at+3600),event_type,state,title,target_id,organizer_guid,details FROM guild_society_event WHERE guild_id=%u AND scheduled_at<%u AND COALESCE(ends_at,scheduled_at+3600)>%u %sORDER BY scheduled_at,event_id LIMIT 6 OFFSET %u",id,ends,starts,filter.c_str(),offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();
            const std::string eventId=f[0].GetString();
            if(!Id(eventId)) continue;
            std::ostringstream out;out<<"EVENT\t"<<op<<'\t'<<eventId<<'\t'<<f[1].GetUInt32()<<'\t'
                <<f[2].GetUInt32()<<'\t'<<f[3].GetUInt32()<<'\t'<<Wire(f[4].GetString(),16)<<'\t'
                <<Wire(f[5].GetString(),16)<<'\t'<<Wire(f[6].GetString(),48);
            Send(actor,out.str());
            MemberSlot* organizer=guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,f[8].GetUInt32()));
            Send(actor,"EVENTMETA\t"+op+"\t"+eventId+"\t"+std::to_string(f[7].GetUInt32())+"\t"+
                Wire(organizer?organizer->Name:"Unavailable",24)+"\t"+Wire(f[9].GetString(),72));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\tevents\t"+std::to_string(count>5?offset+5:0));
    } else if(section.find("roster:")==0 && Id(section.substr(7))) {
        const std::string eventId=section.substr(7);
        if(!CharacterDatabase.PQuery("SELECT event_id FROM guild_society_event WHERE guild_id=%u AND event_id='%s'",id,eventId.c_str())) {
            Send(actor,"RESULT\t"+op+"\trejected\tevent_not_found");return;
        }
        auto rows=CharacterDatabase.PQuery("SELECT r.character_guid,c.name,r.response,r.role,r.accepted_revision,e.revision FROM guild_society_rsvp r JOIN guild_society_event e ON e.event_id=r.event_id JOIN characters c ON c.guid=r.character_guid JOIN guild_member m ON m.guid=r.character_guid AND m.guildid=e.guild_id WHERE e.guild_id=%u AND e.event_id='%s' ORDER BY r.character_guid LIMIT 6 OFFSET %u",id,eventId.c_str(),offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();
            std::string response=f[2].GetString();
            if(response=="accepted"&&f[4].GetUInt32()!=f[5].GetUInt32()) response="renewal_required";
            Send(actor,"ATTENDEE\t"+op+"\t"+eventId+"\t"+std::to_string(f[0].GetUInt32())+"\t"+
                Wire(f[1].GetString(),24)+"\t"+Wire(response,24)+"\t"+Wire(f[3].GetString(),16));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\t"+section+"\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="goals") {
        const auto bank=guild->GetBankItemCounts();
        auto rows=CharacterDatabase.PQuery("SELECT goal_id,item_entry,required_quantity,available_quantity,reserved_quantity,state,priority,provenance,updated_at,request_kind,purpose FROM guild_society_supply_goal WHERE guild_id=%u AND state NOT IN ('cancelled','expired') ORDER BY priority DESC,updated_at DESC,goal_id LIMIT 6 OFFSET %u",id,offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();const auto* item=sObjectMgr.GetItemPrototype(f[1].GetUInt32());
            auto found=bank.find(f[1].GetUInt32());
            const bool money=f[9].GetCppString()=="money";
            const uint32 available=money?uint32(std::min(uint64(0xFFFFFFFF),guild->GetGuildBankMoney())):found==bank.end()?0:found->second;
            const std::string goalState=f[7].GetCppString()=="legacy_needs_review"?"needs_review":f[5].GetString();
            std::ostringstream out;out<<"GOAL\t"<<op<<'\t'<<f[0].GetString()<<'\t'<<f[1].GetUInt32()<<'\t'
                <<Wire(money?"Gold (copper units)":item?item->Name1:"Unknown item",32)<<'\t'<<f[2].GetUInt32()<<'\t'<<available<<'\t'
                <<f[4].GetUInt32()<<'\t'<<Wire(goalState,20)<<'\t'<<f[6].GetUInt32()<<'\t'<<(money?"money":"item");
            Send(actor,out.str());
            Send(actor,"GOALKIND\t"+op+"\t"+f[0].GetString()+"\t"+(money?"money":"item")+"\t"+Wire(f[10].GetString(),64));
            Send(actor,"GOALMETA\t"+op+"\t"+f[0].GetString()+"\t"+Wire(f[7].GetString(),32)+"\t"+std::to_string(uint32(time(nullptr))));
            std::string deliveryStatus;const uint32 transit=sGuildSupplies.InTransit(id,f[0].GetString(),deliveryStatus);
            if(!guild->GetPurchasedTabs()) deliveryStatus="no_guild_bank_tabs";
            else if(!Allows(guild,"supplies")) deliveryStatus="supply_automation_paused";
            else if(money&&!sGuildSupplies.MoneyEnabled(id)) deliveryStatus="money_donations_disabled";
            else if(uint64(available)>=uint64(f[2].GetUInt32())+f[4].GetUInt32()) deliveryStatus="stock_target_met";
            if(money&&f[5].GetCppString()=="completed") deliveryStatus="fundraiser_completed";
            else if(money&&!transit&&deliveryStatus=="searching_for_spare_items") deliveryStatus="waiting_for_willing_donors_with_spare_gold";
            Send(actor,"GOALDELIVERY\t"+op+"\t"+f[0].GetString()+"\t"+std::to_string(transit)+"\t"+Wire(deliveryStatus,48));
            uint32 held=0,mailed=0,collected=0;sGuildSupplies.DeliveryCounts(id,f[0].GetString(),held,mailed,collected);
            Send(actor,"GOALFLOW\t"+op+"\t"+f[0].GetString()+"\t"+std::to_string(held)+"\t"+std::to_string(mailed)+"\t"+std::to_string(collected));
            if(!money) {
                const auto work=sLivingActivityCoordinator.ReadGuildProcurementStatus(id,f[1].GetUInt32(),f[0].GetCppString());
                // Optional G2 records: old addons ignore these. Never add
                // assignments to native GOAL stock, GOALDELIVERY or GOALFLOW.
                const std::string key=op+"\t"+f[0].GetCppString()+"\t";
                Send(actor,"GOALWORK\t"+key+std::to_string(work.assigned)+"\t"+std::to_string(work.carried)+"\t"+
                    std::to_string(work.banked)+"\t"+std::to_string(work.mail)+"\t"+std::to_string(work.updatedAtMs/1000));
                Send(actor,"GOALWORKSTATUS\t"+key+(work.ready?"1":"0")+"\t"+Wire(work.phase,20)+"\t"+Wire(work.blocker,48));
            }
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\tgoals\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="history" && permissions) {
        auto rows=CharacterDatabase.PQuery("SELECT operation_type,outcome,reason,created_at,actor_guid FROM guild_society_operation WHERE guild_id=%u ORDER BY created_at DESC,operation_id LIMIT 6 OFFSET %u",id,offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();
            Send(actor,"HISTORY\t"+op+"\t"+f[0].GetString()+"\t"+f[1].GetString()+"\t"+
                Wire(f[2].GetString(),100)+"\t"+std::to_string(f[3].GetUInt32())+"\t"+std::to_string(f[4].GetUInt32()));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\thistory\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="evidence" && (permissions&(Promote|Demote))) {
        // Only mapped recruit/member ranks are candidates. Existing rank names
        // and promoted members are untouched, including legacy promotions.
        auto rows=CharacterDatabase.PQuery("SELECT m.guid,c.name,e.joined_at,e.last_promotion_at,COALESCE(cr.credits,0),COALESCE(cr.days,0),r.semantic_role,COALESCE(e.tenure_source,'unknown') FROM guild_member m JOIN characters c ON c.guid=m.guid JOIN guild_society_rank_role r ON r.guild_id=m.guildid AND r.rank_id=m.rank LEFT JOIN guild_society_member_evidence e ON e.guild_id=m.guildid AND e.character_guid=m.guid LEFT JOIN (SELECT c.character_guid,COUNT(*) credits,COUNT(DISTINCT FLOOR(c.earned_at/86400)) days FROM guild_society_credit c JOIN guild_society_member_evidence e2 ON e2.guild_id=c.guild_id AND e2.character_guid=c.character_guid AND c.earned_at>=e2.joined_at WHERE c.guild_id=%u AND c.source_type IN ('verified_event','approved_supply') GROUP BY c.character_guid) cr ON cr.character_guid=m.guid WHERE m.guildid=%u AND r.semantic_role IN ('recruit','member') ORDER BY m.guid LIMIT 6 OFFSET %u",id,id,offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();
            const auto reason=PromotionBlocker(p,uint32(time(nullptr)),f[2].GetUInt32(),f[3].GetUInt32(),
                f[4].GetUInt32(),f[5].GetUInt32(),f[6].GetCppString()=="member",0);
            Send(actor,"EVIDENCE\t"+op+"\t"+std::to_string(f[0].GetUInt32())+"\t"+Wire(f[1].GetString(),24)+"\t"+
                std::to_string(f[2].GetUInt32())+"\t"+std::to_string(f[3].GetUInt32())+"\t"+
                std::to_string(f[4].GetUInt32())+"\t"+std::to_string(f[5].GetUInt32())+"\t"+reason+"\t"+Wire(f[7].GetString(),24));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\tevidence\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="officers" && (permissions&Delegate)) {
        leadership();
        auto rows=CharacterDatabase.PQuery("SELECT m.guid,c.name FROM guild_member m JOIN characters c ON c.guid=m.guid WHERE m.guildid=%u ORDER BY m.`rank`,c.name,m.guid LIMIT 6 OFFSET %u",id,offset);
        uint32 count=0;
        if(rows) do {
            if(++count>5) break;
            Field* f=rows->Fetch();const uint32 guid=f[0].GetUInt32();
            MemberSlot* member=guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,guid));
            if(!member) continue;
            const uint32 rights=(guild->HasRankRight(member->RankId,GR_RIGHT_INVITE)?uint32(Recruit):0u)|
                (guild->HasRankRight(member->RankId,GR_RIGHT_MODIFY_GUILD_INFO)?uint32(Configure):0u);
            Send(actor,"MEMBER\t"+op+"\t"+std::to_string(guid)+"\t"+Wire(member->Name,24)+"\t"+std::to_string(rights)+"\t"+Wire(guild->GetRankName(member->RankId),32));
        } while(rows->NextRow());
        Send(actor,"END\t"+op+"\tofficers\t"+std::to_string(count>5?offset+5:0));
    } else if(section=="policy") {
        Send(actor,"POLICY\t"+op+"\t"+std::to_string(p.memberHours)+"\t"+std::to_string(p.veteranDays)+"\t"+
            std::to_string(p.memberCredits)+"\t"+std::to_string(p.veteranCredits)+"\t"+
            std::to_string(p.cooldownHours)+"\t"+std::to_string(p.dailyLimit));
        leadership();
        Send(actor,"END\t"+op+"\tpolicy\t0");
    } else Send(actor,"RESULT\t"+op+"\trejected\tsection_not_authorized");
}
bool PlayerbotGuildGovernance::Handle(Player* actor,const std::string& message) {
    if(message.find("LWOWG2\t")!=0) return false;
    if(!actor||!actor->isRealPlayer()||!actor->GetGuildId()||message.size()>254) return true;
    auto f=Split(message);if(f.size()<3||!Id(f[2])||f[2].size()>40) return true;
    const std::string op=f[2];const uint32 now=uint32(time(nullptr)),guid=actor->GetGUIDLow();
    // Bound client-triggered DB work. A rejected request is never replayed later.
    if(requests_[guid]>now) {Send(actor,"RESULT\t"+op+"\trejected\tcooldown");return true;}
    requests_[guid]=now+1;
    Guild* guild=sGuildMgr.GetGuildById(actor->GetGuildId());
    Policy* loaded=Load(guild);
    if(!loaded) {Send(actor,"RESULT\t"+op+"\trejected\tservice_unavailable");return true;}
    Policy p=*loaded;const uint32 id=guild->GetId(),permissions=Permissions(actor,guild);
    if(f[1]=="GET") {
        uint32 offset=0,starts=0,ends=0,history=0;
        const bool events=f.size()==8&&f[3]=="events";
        if((f.size()!=5&&!events)||!Number(f[4],offset,10000)||
            (events&&(!Number(f[5],starts)||!Number(f[6],ends)||!Number(f[7],history,1)||
                ends<=starts||uint64_t(ends)-starts>371*86400))) {
            Send(actor,"RESULT\t"+op+"\trejected\tmalformed_request");return true;
        }
        if(f[3]=="events"&&!events) {Send(actor,"RESULT\t"+op+"\trejected\tevent_window_required");return true;}
        Snapshot(actor,guild,p,op,f[3],offset,starts,ends,history!=0);return true;
    }
    if(f.size()<5||f[1].size()>32||!Id(f[1])) {Send(actor,"RESULT\t"+op+"\trejected\tmalformed_request");return true;}
    const std::string digest=Hash(message);
    auto prior=CharacterDatabase.PQuery("SELECT request_hash,outcome,reason FROM guild_society_operation WHERE guild_id=%u AND actor_guid=%u AND operation_id='%s'",id,guid,op.c_str());
    if(prior) {
        Field* row=prior->Fetch();
        if(row[0].GetString()!=digest) Send(actor,"RESULT\t"+op+"\trejected\toperation_id_reused");
        else {
            Send(actor,"RESULT\t"+op+"\t"+row[1].GetString()+"\t"+Wire(row[2].GetString()));
            if(row[1].GetCppString()=="completed") Snapshot(actor,guild,p,op,"policy",0);
        }
        return true;
    }
    uint32 revision=0;std::string reason,outcome="rejected",sql;std::vector<std::string> afterSql;bool bumpPolicy=true;
    if(!Number(f[3],revision)||revision!=p.revision) reason="stale_revision";
    else if(f[1]=="EVENT_SAVE") {
        EventDefinition e;
        if(!(permissions&Configure)) reason="guild_information_permission_required";
        else if(!active_||!events_) reason="events_disabled_by_server";
        else if(f.size()!=12||!Number(f[5],e.revision)||!Number(f[7],e.starts)||!Number(f[8],e.ends)||!Number(f[9],e.target)) reason="invalid_event";
        else {
            e.id=f[4];e.kind=f[6];e.title=f[10];e.details=f[11];reason=ValidateEvent(e,now);
            if(reason.empty()&&e.kind=="quest"&&!sObjectMgr.GetQuestTemplate(e.target)) reason="quest_not_found";
            if(reason.empty()&&e.kind=="dungeon") {
                const MapEntry* map=sMapStore.LookupEntry(e.target);
                if(!map||!map->IsDungeon()||map->IsRaid()||!PlayerbotGuildEventExecutor::DungeonSupported(e.target)) reason="unsupported_dungeon";
            }
            if(reason.empty()&&e.kind=="supply"&&!CharacterDatabase.PQuery("SELECT goal_id FROM guild_society_supply_goal WHERE guild_id=%u AND item_entry=%u AND state='active' AND provenance IN ('human_request','event_requirement','profession_requirement','equipment_requirement') LIMIT 1",id,e.target)) reason="approved_supply_goal_required";
            if(reason.empty()) {
                auto existing=CharacterDatabase.PQuery("SELECT guild_id,revision,state,scheduled_at,COALESCE(ends_at,0),event_type,target_id FROM guild_society_event WHERE event_id='%s'",e.id.c_str());
                bool renew=false;
                if(existing) {
                    Field* row=existing->Fetch();
                    if(row[0].GetUInt32()!=id) reason="event_scope";
                    else if(row[1].GetUInt32()!=e.revision) reason="stale_event_revision";
                    else if(row[2].GetCppString()!="draft"&&row[2].GetCppString()!="announced") reason="event_already_committed";
                    else {
                        EventDefinition before;before.starts=row[3].GetUInt32();before.ends=row[4].GetUInt32();before.kind=row[5].GetString();before.target=row[6].GetUInt32();
                        renew=NeedsRenewedAcceptance(before,e);
                    }
                } else if(e.revision) reason="event_not_found";
                const uint32 week=UtcWeekStart(e.starts);
                if(reason.empty()) {
                    auto count=CharacterDatabase.PQuery("SELECT COUNT(*) FROM guild_society_event WHERE guild_id=%u AND origin='scheduled' AND scheduled_at>=%u AND scheduled_at<%u AND event_id<>'%s' AND state<>'cancelled'",id,week,week+604800,e.id.c_str());
                    if(!count) reason="database_read_failed";
                    else if(count->Fetch()[0].GetUInt32()>=WeeklyEventLimit(guild->GetMemberSize())) reason="weekly_event_limit";
                }
                if(reason.empty()) {
                    const uint32 minimum=e.kind=="dungeon"?5:e.kind=="social"?1:2;
                    sql="INSERT INTO guild_society_event (event_id,guild_id,revision,origin,event_type,state,title,details,target_id,organizer_guid,scheduled_at,ends_at,minimum_members,maximum_members,tank_slots,healer_slots,damage_slots,created_at,updated_at) VALUES ('"+
                        e.id+"',"+std::to_string(id)+",1,'scheduled','"+e.kind+"','announced','"+Esc(e.title)+"','"+Esc(e.details)+"',"+
                        std::to_string(e.target)+","+std::to_string(guid)+","+std::to_string(e.starts)+","+std::to_string(e.ends)+","+
                        std::to_string(minimum)+",5,"+(e.kind=="dungeon"?"1,1,3":"0,0,0")+","+std::to_string(now)+","+std::to_string(now)+")";
                    if(existing) sql="UPDATE guild_society_event SET revision=revision+1,event_type='"+e.kind+"',title='"+Esc(e.title)+"',details='"+Esc(e.details)+"',target_id="+
                        std::to_string(e.target)+",scheduled_at="+std::to_string(e.starts)+",ends_at="+std::to_string(e.ends)+",minimum_members="+std::to_string(minimum)+
                        ",maximum_members=5,tank_slots="+(e.kind=="dungeon"?"1":"0")+",healer_slots="+(e.kind=="dungeon"?"1":"0")+",damage_slots="+(e.kind=="dungeon"?"3":"0")+
                        ",origin='scheduled',updated_at="+std::to_string(now)+" WHERE event_id='"+e.id+"' AND guild_id="+std::to_string(id)+" AND revision="+std::to_string(e.revision)+" AND state IN ('draft','announced')";
                    afterSql.push_back("UPDATE guild_society_rsvp SET "+std::string(renew?"response=IF(response='accepted','renewal_required',response)":"accepted_revision="+std::to_string(e.revision+1))+" WHERE event_id='"+e.id+"'");
                }
            }
        }
    } else if(f[1]=="EVENT_CANCEL"||f[1]=="EVENT_RSVP") {
        uint32 eventRevision=0;
        const bool cancel=f[1]=="EVENT_CANCEL";
        if(f.size()!=(cancel?6u:7u)||!Id(f[4])||!Number(f[5],eventRevision)) reason="invalid_event_request";
        else if(cancel&&!(permissions&Configure)) reason="guild_information_permission_required";
        else {
            auto existing=CharacterDatabase.PQuery("SELECT revision,state,scheduled_at,COALESCE(ends_at,0) FROM guild_society_event WHERE guild_id=%u AND event_id='%s'",id,f[4].c_str());
            if(!existing) reason="event_not_found";
            else {
                Field* row=existing->Fetch();const std::string state=row[1].GetString();
                const uint32 bufferedEnd=row[3].GetUInt32()>0xffffffffu-900?0xffffffffu:row[3].GetUInt32()+900;
                if(row[0].GetUInt32()!=eventRevision) reason="stale_event_revision";
                else if(TerminalEvent(state)) reason="event_closed";
                else if(cancel) {
                    sql="UPDATE guild_society_event SET state='cancelled',revision=revision+1,failure_reason='manager_cancelled',finished_at="+std::to_string(now)+",updated_at="+std::to_string(now)+" WHERE event_id='"+f[4]+"' AND guild_id="+std::to_string(id);
                    afterSql.push_back("UPDATE guild_society_rsvp SET response='cancelled' WHERE event_id='"+f[4]+"' AND response IN ('accepted','tentative','renewal_required')");
                } else if((state!="draft"&&state!="announced"&&state!="forming")||(f[6]!="accepted"&&f[6]!="tentative"&&f[6]!="declined")) reason="rsvp_not_available";
                else if(f[6]=="accepted"&&CharacterDatabase.PQuery("SELECT r.event_id FROM guild_society_rsvp r JOIN guild_society_event e ON e.event_id=r.event_id WHERE r.character_guid=%u AND r.response='accepted' AND r.accepted_revision=e.revision AND e.state IN ('announced','forming','traveling','active') AND e.event_id<>'%s' AND e.scheduled_at<%u AND COALESCE(e.ends_at,e.scheduled_at+3600)>%u LIMIT 1",guid,f[4].c_str(),bufferedEnd,row[2].GetUInt32()>900?row[2].GetUInt32()-900:0)) reason="conflicting_commitment";
                else {
                    bumpPolicy=false;
                    if(f[6]=="accepted") {
                        auto capacity=CharacterDatabase.PQuery("SELECT COUNT(r.character_guid),MAX(e.maximum_members) FROM guild_society_event e LEFT JOIN guild_society_rsvp r ON r.event_id=e.event_id AND r.response='accepted' AND r.accepted_revision=e.revision AND r.character_guid<>%u WHERE e.guild_id=%u AND e.event_id='%s' GROUP BY e.event_id",guid,id,f[4].c_str());
                        if(!capacity) reason="database_read_failed";
                        else if(capacity->Fetch()[0].GetUInt32()>=capacity->Fetch()[1].GetUInt32()) reason="event_full";
                    }
                    if(reason.empty()) sql="INSERT INTO guild_society_rsvp (event_id,character_guid,response,role,human,accepted_revision,updated_at) VALUES ('"+f[4]+"',"+std::to_string(guid)+",'"+f[6]+"','',1,"+std::to_string(eventRevision)+","+std::to_string(now)+") ON DUPLICATE KEY UPDATE response=VALUES(response),accepted_revision=VALUES(accepted_revision),updated_at=VALUES(updated_at)";
                }
            }
        }
    } else if(f[1]=="DELEGATE") {
        uint32 value=0;
        if(!(permissions&Delegate)) reason="guild_master_only";
        else if(f.size()!=6||!Number(f[5],value,1)||
            (f[4]!="recruitment"&&f[4]!="events"&&f[4]!="supplies"&&f[4]!="promotions")) reason="invalid_delegation";
        else if(f[4]=="promotions"&&value) reason="promotion_preview_only";
        else sql="UPDATE guild_society_policy SET "+f[4]+"="+std::to_string(value)+" WHERE guild_id="+std::to_string(id);
    } else if(f[1]=="OFFICER_ASSIGN") {
        uint32 target=0;
        if(!(permissions&Delegate)) reason="guild_master_only";
        else if(f.size()!=6||!Number(f[5],target)||
            (f[4]!="recruiter"&&f[4]!="event_organizer"&&f[4]!="quartermaster")) reason="invalid_officer_duty";
        else {
            MemberSlot* member=target?guild->GetMemberSlot(ObjectGuid(HIGHGUID_PLAYER,target)):nullptr;
            const uint32 required=f[4]=="recruiter"?GR_RIGHT_INVITE:GR_RIGHT_MODIFY_GUILD_INFO;
            if(target&&(!member||!guild->HasRankRight(member->RankId,required))) reason="officer_native_rights_required";
            else sql="INSERT INTO guild_society_officer (guild_id,duty,character_guid,state,assigned_at,updated_at) VALUES ("+
                std::to_string(id)+",'"+f[4]+"',"+std::to_string(target)+",'"+(target?"active":"revoked")+"',"+
                std::to_string(now)+","+std::to_string(now)+") ON DUPLICATE KEY UPDATE character_guid=VALUES(character_guid),state=VALUES(state),assigned_at=VALUES(assigned_at),updated_at=VALUES(updated_at)";
        }
    } else if(f[1]=="MONEY_GOAL_SAVE") {
        uint32 target=0,priority=0;
        if(!(permissions&Configure)) reason="guild_information_permission_required";
        else if(f.size()!=8||!Id(f[4])||!Number(f[5],target,10000000)||target<100||!Number(f[6],priority,3)||
            f[7].size()<3||f[7].size()>64||Wire(f[7],64)!=f[7]) reason="invalid_money_request";
        else if(!guild->GetPurchasedTabs()) reason="purchase_first_bank_tab_before_fundraising";
        else if(guild->GetGuildBankMoney()>=target) reason="bank_already_has_target_funds";
        else {
            auto existing=CharacterDatabase.PQuery("SELECT guild_id,request_kind,state,purpose,required_quantity FROM guild_society_supply_goal WHERE goal_id='%s'",f[4].c_str());
            if(existing&&(existing->Fetch()[0].GetUInt32()!=id||existing->Fetch()[1].GetCppString()!="money")) reason="goal_scope_or_kind";
            else if(existing&&existing->Fetch()[2].GetCppString()!="active") reason="fundraiser_is_closed";
            else if(existing&&(existing->Fetch()[3].GetString()!=f[7]||target>existing->Fetch()[4].GetUInt32())&&CharacterDatabase.PQuery("SELECT delivery_id FROM guild_society_supply_delivery WHERE guild_id=%u AND goal_id='%s' LIMIT 1",id,f[4].c_str())) reason="committed_fundraiser_cannot_change_purpose_or_increase";
            else if(CharacterDatabase.PQuery("SELECT goal_id FROM guild_society_supply_goal WHERE guild_id=%u AND request_kind='money' AND state='active' AND goal_id<>'%s' LIMIT 1",id,f[4].c_str())) reason="one_active_fundraiser_per_guild";
            else if(existing) sql="UPDATE guild_society_supply_goal SET required_quantity="+std::to_string(target)+",priority="+std::to_string(priority)+",purpose='"+Esc(f[7])+"',updated_at="+std::to_string(now)+" WHERE goal_id='"+f[4]+"' AND guild_id="+std::to_string(id)+" AND request_kind='money' AND state='active'";
            else sql="INSERT INTO guild_society_supply_goal (goal_id,guild_id,goal_type,item_entry,required_quantity,state,priority,requested_by,provenance,request_kind,purpose,created_at,updated_at) VALUES ('"+
                f[4]+"',"+std::to_string(id)+",'money_request',0,"+std::to_string(target)+",'active',"+std::to_string(priority)+","+std::to_string(guid)+",'human_request','money','"+Esc(f[7])+"',"+std::to_string(now)+","+std::to_string(now)+")";
        }
    } else if(f[1]=="GOAL_SAVE") {
        uint32 item=0,quantity=0,priority=0;
        if(!(permissions&Configure)) reason="guild_information_permission_required";
        else if(f.size()!=8||!Id(f[4])||!Number(f[5],item)||!Number(f[6],quantity,10000)||!quantity||
            !Number(f[7],priority,3)||!sObjectMgr.GetItemPrototype(item)) reason="invalid_goal";
        else {
            auto existing=CharacterDatabase.PQuery("SELECT item_entry FROM guild_society_supply_goal WHERE goal_id='%s'",f[4].c_str());
            auto ours=CharacterDatabase.PQuery("SELECT item_entry FROM guild_society_supply_goal WHERE goal_id='%s' AND guild_id=%u",f[4].c_str(),id);
            if(existing&&!ours) reason="goal_scope";
            else if(ours&&ours->Fetch()[0].GetUInt32()!=item) reason="cancel_old_goal_before_changing_item";
            else if(ours) sql="UPDATE guild_society_supply_goal SET required_quantity="+std::to_string(quantity)+",priority="+std::to_string(priority)+",provenance='human_request',state='active',requested_by="+
                std::to_string(guid)+",updated_at="+std::to_string(now)+" WHERE goal_id='"+f[4]+"' AND guild_id="+std::to_string(id)+" AND item_entry="+std::to_string(item);
            else sql="INSERT INTO guild_society_supply_goal (goal_id,guild_id,goal_type,item_entry,required_quantity,state,priority,requested_by,provenance,created_at,updated_at) VALUES ('"+
                f[4]+"',"+std::to_string(id)+",'explicit_request',"+std::to_string(item)+","+std::to_string(quantity)+",'active',"+
                std::to_string(priority)+","+std::to_string(guid)+",'human_request',"+std::to_string(now)+","+std::to_string(now)+")";
        }
    } else if(f[1]=="GOAL_CANCEL") {
        if(!(permissions&Configure)) reason="guild_information_permission_required";
        else if(f.size()!=5||!Id(f[4])) reason="invalid_goal";
        else if(!CharacterDatabase.PQuery("SELECT goal_id FROM guild_society_supply_goal WHERE guild_id=%u AND goal_id='%s'",id,f[4].c_str())) reason="goal_not_found";
        else sql="UPDATE guild_society_supply_goal SET state='cancelled',updated_at="+std::to_string(now)+" WHERE guild_id="+std::to_string(id)+" AND goal_id='"+f[4]+"'";
    } else reason="unsupported_operation";
    if(!sql.empty()) {outcome="completed";reason="saved";}
    if(!CharacterDatabase.BeginTransaction()) {Send(actor,"RESULT\t"+op+"\tfailed\tdatabase_unavailable");return true;}
    if(!sql.empty()) {
        CharacterDatabase.PExecute("%s",sql.c_str());
        for(const auto& extra:afterSql) CharacterDatabase.PExecute("%s",extra.c_str());
        if(bumpPolicy) CharacterDatabase.PExecute("UPDATE guild_society_policy SET revision=revision+1,updated_at=%u WHERE guild_id=%u",now,id);
    }
    CharacterDatabase.PExecute("INSERT INTO guild_society_operation (guild_id,actor_guid,operation_id,operation_type,request_hash,revision,outcome,reason,created_at) VALUES (%u,%u,'%s','%s','%s',%u,'%s','%s',%u)",id,guid,op.c_str(),Esc(f[1]).c_str(),digest.c_str(),p.revision,outcome.c_str(),reason.c_str(),now);
    if(!CharacterDatabase.CommitTransactionDirect()) {Send(actor,"RESULT\t"+op+"\tfailed\tdatabase_write_failed");return true;}
    if(!sql.empty()) {policies_.erase(id);officers_.erase(id);}
    Send(actor,"RESULT\t"+op+"\t"+outcome+"\t"+reason);
    if(Policy* current=Load(guild)) Snapshot(actor,guild,*current,op,"policy",0);
    return true;
}
void PlayerbotGuildGovernance::Record(uint32 guild,uint32 kind,uint32 actor,uint32 target,uint32 value) {
    if(facts_.size()<256) facts_.push_back({guild,kind,actor,target,value,uint32(time(nullptr))});
}
void PlayerbotGuildGovernance::Update(bool active) {
    active_=active;const uint32 now=uint32(time(nullptr));
    if(now<nextFlush_) return;
    nextFlush_=now+10;
    if(!Ready()) {facts_.clear();return;}
    for(const auto& fact:facts_) {
        const char* kind=fact.kind==GUILD_EVENT_LOG_JOIN_GUILD?"member_joined":fact.kind==GUILD_EVENT_LOG_PROMOTE_PLAYER?"member_promoted":
            fact.kind==GUILD_EVENT_LOG_DEMOTE_PLAYER?"member_demoted":fact.kind==GUILD_EVENT_LOG_LEAVE_GUILD?"member_left":"membership_changed";
        CharacterDatabase.PExecute("INSERT INTO guild_society_social_event (guild_id,kind,actor_guid,target_guid,value,reason,occurred_at) VALUES (%u,'%s',%u,%u,%u,'native_guild_operation',%u)",fact.guild,kind,fact.actor,fact.target,fact.value,fact.time);
        if(fact.kind==GUILD_EVENT_LOG_JOIN_GUILD)
            CharacterDatabase.PExecute("INSERT INTO guild_society_member_evidence (guild_id,character_guid,joined_at,tenure_source) VALUES (%u,%u,%u,'native_join') ON DUPLICATE KEY UPDATE joined_at=VALUES(joined_at),tenure_source=VALUES(tenure_source),last_promotion_at=0",fact.guild,fact.actor,fact.time);
        if(fact.kind==GUILD_EVENT_LOG_PROMOTE_PLAYER)
            CharacterDatabase.PExecute("UPDATE guild_society_member_evidence SET last_promotion_at=%u WHERE guild_id=%u AND character_guid=%u",fact.time,fact.guild,fact.target);
    }
    facts_.clear();
    for(auto it=requests_.begin();it!=requests_.end();) if(it->second+60<now) it=requests_.erase(it); else ++it;
    for(auto it=legacySnapshots_.begin();it!=legacySnapshots_.end();) if(it->second.expires<now) it=legacySnapshots_.erase(it); else ++it;
}
