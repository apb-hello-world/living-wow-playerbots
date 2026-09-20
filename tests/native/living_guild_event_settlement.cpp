#include "LivingGuildEventSettlement.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;
int main(int argc,char** argv) {
    const bool dungeon=argc>1 && std::string(argv[1])=="dungeon";
    GuildEventCommitment c{"qa:native:event","quest",4,3,42,1000,4600};
    if(dungeon){c.kind="dungeon";c.target=33;}
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f38";task.source="guild_event_commitment";
    task.actor=task.context.actor=497;task.kind=Kind::GuildEvent;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.priority=Priority::Scheduled;task.accepted=true;task.sourceKey=GuildEventCommitmentKey(c,task.actor);
    task.dueAtMs=1000000;task.createdAtMs=task.updatedAtMs=1000;task.checkpoint.step="guild_event_wait";
    task.checkpoint.data=EncodeGuildEventCommitment(c);task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.context.boot="ff2efbdf-f0ec-4539-b840-299847970c00";
    const auto admitted=TaskWrite(task,0,"ff2efbdf-f0ec-4539-b840-299847970c01","task_admitted");
    ++task.revision;task.phase=Phase::Preparing;task.updatedAtMs=1100;task.checkpoint.step="guild_event_form";
    const auto preparing=TaskWrite(task,1,"ff2efbdf-f0ec-4539-b840-299847970c02","task_admitted");
    GuildEventClosure closure{"completed",dungeon?"dungeon_encounters_verified":"quest_reward_verified",1000,1300,1200,dungeon?700u:0u};
    GuildEventSettlement result;std::string why;
    auto ready=task;ready.checkpoint.step="guild_quest_reward_ready";
    assert(ValidateGuildEventCommitmentTask(ready,why));
    assert(PreserveGuildEventCommitment(task,ready,why));
    assert(PrepareGuildEventSettlement(task,task.context,closure,1300000,
        "ff2efbdf-f0ec-4539-b840-299847970c03",result,why));
    const auto completed=result;
    auto restart=task.context;++restart.actorGeneration;
    const std::vector<std::string> invalidations={"guild_event_removed","guild_event_revision_requires_renewal",
        "guild_event_membership_changed","guild_event_acceptance_withdrawn"};
    for(const auto& reason:invalidations) {
        assert(PrepareGuildEventInvalidation(task,restart,reason,1300000,
            "ff2efbdf-f0ec-4539-b840-299847970c04",result,why));
        assert(result.task.phase==Phase::Cancelled && result.task.context==restart && result.task.revision==3);
        assert(result.task.checkpoint.blocker==reason && result.task.checkpoint.data==task.checkpoint.data);
    }
    assert(!PrepareGuildEventInvalidation(task,restart,"database_unavailable",1300000,
        "ff2efbdf-f0ec-4539-b840-299847970c04",result,why));
    if(argc>1) {
        boost::property_tree::ptree output;
        auto add=[&](const char* key,const WritePlan& plan){
            boost::property_tree::ptree entry,array;
            for(const auto& sql:plan.statements){boost::property_tree::ptree item;item.put_value(sql);array.push_back({"",item});}
            entry.add_child("statements",array);entry.put("receipt",plan.receiptQuery);output.add_child(key,entry);
        };
        add("admit",admitted);add("prepare",preparing);add("complete",completed.plan);
        for(const auto& reason:invalidations) {
            assert(PrepareGuildEventInvalidation(task,restart,reason,1300000,
                "ff2efbdf-f0ec-4539-b840-299847970c04",result,why));
            add(reason.c_str(),result.plan);
        }
        output.put("projection",GuildEventClosureQuery(task,c));output.put("kind",c.kind);
        boost::property_tree::write_json(std::cout,output,false);
    }
}
