#include "LivingGuildProcurementPreparation.h"
#include "LivingGuildProcurementRecovery.h"
#include "LivingGuildProcurementHandoff.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;
int main(int argc,char**) {
    GuildProcurementJob job{3,314,2770,5,"goal_copper","ef526383-763e-4a84-b501-12b4c2966321"};
    Task task;task.id=task.root="f7528ef3-b678-4cc4-b4cd-a0db5d6ec17e";task.actor=314;
    task.source="guild_procurement";task.sourceKey=GuildProcurementSourceKey(job);task.kind=Kind::GuildProcurement;
    task.mode=Mode::Active;task.phase=Phase::Preparing;task.createdAtMs=task.updatedAtMs=1000;
    task.context.actor=314;task.context.actorGeneration=task.context.mapGeneration=1;
    task.context.boot="f8628b78-d726-437b-8e32-8361c9e90aab";
    task.checkpoint.data=EncodeGuildProcurementJob(job);task.checkpoint.step="guild_procurement_prepare";
    ResourceClaim a;a.id="178a1ba5-f6c0-4e1c-871d-f160ce09ec92";a.task=task.id;a.actor=314;
    a.itemGuid=9001;a.itemEntry=2770;a.quantity=2;a.location="bags";a.state="held";
    auto b=a;b.id="2e423458-599f-40f3-989c-56e9154ad79d";b.itemGuid=9002;b.quantity=3;
    const std::string receipt="0b55ea74-fb78-4f40-b562-6d09667cf96e";
    UnsettledClaimBatch batch{1,true,{a,b}};
    std::vector<NativeResourceBalance> stock{{314,9001,2770,8,0,"bags",0},{314,9002,2770,3,0,"bags",0}};
    std::string why;GuildProcurementMaterialPlan plan;
    assert(PlanGuildProcurementMaterials(task,batch,stock,plan,why) && plan.carriedReady && plan.changes.empty());
    auto empty=batch;empty.claims.clear();
    assert(PlanGuildProcurementMaterials(task,empty,stock,plan,why));
    assert(!plan.carriedReady && plan.changes.size()==1 && plan.changes[0].after.quantity==5);
    assert(plan.changes[0].after.id.empty() && plan.changes[0].expectedRevision==0);
    auto partial=batch;partial.claims.pop_back();
    assert(PlanGuildProcurementMaterials(task,partial,stock,plan,why));
    assert(plan.changes.size()==1 && plan.changes[0].expectedRevision==1 && plan.changes[0].after.id==a.id && plan.changes[0].after.quantity==5);
    auto surplus=batch;surplus.claims[0].quantity=8;
    assert(PlanGuildProcurementMaterials(task,surplus,stock,plan,why));
    assert(!plan.carriedReady && plan.changes.size()==2);
    assert(plan.changes[0].after.quantity==5 && plan.changes[1].after.state=="released");
    ResourceClaimBook book;assert(book.RestoreBatch(surplus.claims)==ClaimInstall::Installed && book.FinishRestore());
    // Pending trimming still protects ALL original goods. Only the committed
    // reservation receipt releases surplus, and never changes native stacks.
    assert(book.ReservePending(receipt,plan.changes,stock)==ClaimInstall::Installed);
    assert(book.Protection().ProtectedItem(314,9001,2770)==8);
    assert(book.CommitReservation(receipt)==ClaimInstall::Installed);
    assert(book.Protection().ProtectedItem(314,9001,2770)==5 && book.Protection().ProtectedItem(314,9002,2770)==0);
    auto mail=batch;mail.claims[1].location="mail";mail.claims[1].nativeReference=7;
    assert(PlanGuildProcurementMaterials(task,mail,{stock[0]},plan,why) && plan.incoming==3 && plan.changes.empty() && !plan.carriedReady);
    auto bank=mail;bank.claims[1].location="bank";bank.claims[1].nativeReference=0;bank.claims[1].quantity=10;
    assert(PlanGuildProcurementMaterials(task,bank,{stock[0]},plan,why) && plan.incoming==10 && plan.changes.empty());
    auto merged=batch;merged.claims[1].itemGuid=a.itemGuid;
    assert(PlanGuildProcurementMaterials(task,merged,{stock[0]},plan,why) && plan.carriedReady);
    for(int n=0;n!=11;++n) {
        auto t=task;auto c=batch;auto s=stock;
        if(n==0)t.mode=Mode::Observe;
        if(n==1)t.phase=Phase::Verifying;
        if(n==2)c.complete=false;
        if(n==3)c.bookRevision=0;
        if(n==4)c.claims[0].state="in_transfer";
        if(n==5)c.claims[0].id=c.claims[1].id;
        if(n==6)s[0].quantity=1;
        if(n==7)s[0].location="bank";
        if(n==8)s[0].actor=99;
        if(n==9)s.pop_back(); // No claiming a portion from another root's protected stack.
        if(n==10)s.push_back(s[0]);
        assert(!PlanGuildProcurementMaterials(t,c,s,plan,why));
    }
    auto current=task.context;++current.mapGeneration;current.boot="e9177d1b-f094-41c2-bd10-7f18a381f568";
    auto paused=task;paused.phase=Phase::WaitingExternal;paused.retryAtMs=1900;paused.checkpoint.activeElapsedMs=71;paused.dueAtMs=1500;
    GuildProcurementRecovery recovery;
    assert(PrepareGuildProcurementResumption(paused,current,batch,stock,2000,receipt,recovery,why));
    assert(recovery.task.phase==Phase::Preparing && recovery.task.id==task.id && recovery.task.checkpoint.data==task.checkpoint.data);
    assert(recovery.task.checkpoint.activeElapsedMs==71 && recovery.task.dueAtMs==1500 && recovery.task.retryAtMs==1900 && recovery.claims.empty());
    assert(recovery.plan.statements.front().find("UPDATE living_activity_task SET actor_guid=actor_guid")!=std::string::npos);
    auto mailStock=stock;mailStock[1].location="mail";mailStock[1].nativeReference=7;
    assert(PrepareGuildProcurementResumption(task,current,mail,mailStock,2000,receipt,recovery,why));
    assert(recovery.plan.statements[1].find("m.cod=0")!=std::string::npos);
    auto bankStock=stock;bankStock[1].location="bank";bankStock[1].quantity=10;
    assert(PrepareGuildProcurementResumption(task,current,bank,bankStock,2000,receipt,recovery,why));
    for(int n=0;n!=8;++n) {
        auto t=task;auto c=batch;auto s=stock;auto context=current;
        if(n==0)t.phase=Phase::Completed;
        if(n==1)context.actor=9;
        if(n==2)context.mapGeneration=0;
        if(n==3)c.complete=false;
        if(n==4)c.claims[0].state="reconciling";
        if(n==5)s[0].quantity=1;
        if(n==6)c.claims[0].location="bank";
        if(n==7)s.push_back(s[0]);
        assert(!PrepareGuildProcurementResumption(t,context,c,s,2000,receipt,recovery,why));
    }
    GuildProcurementClosure goal{"cancelled","item",2770,5,0,1,0};
    assert(PrepareGuildProcurementCancellation(task,task.context,goal,batch,stock,2000,receipt,recovery,why));
    assert(recovery.task.phase==Phase::Cancelled && recovery.claims.size()==2 && recovery.claims[0].after.state=="released");
    assert(!PrepareGuildProcurementCancellation(task,task.context,goal,mail,mailStock,2000,receipt,recovery,why));
    assert(why=="guild_procurement_cancel_collect_paid_mail_first");
    goal.state="active";assert(GuildProcurementClosureReason(job,goal).empty());
    goal.banked=5;assert(GuildProcurementClosureReason(job,goal)=="guild_procurement_surplus_items_preserved");
    goal.reserved=1;assert(GuildProcurementClosureReason(job,goal).empty());
    goal.entry=159;assert(GuildProcurementClosureReason(job,goal)=="guild_procurement_changed_request_items_preserved");
    goal={};assert(GuildProcurementClosureReason(job,goal).empty()); // Failed reads are not cancelled work.
    // Export actual compiled metadata journals for copied MariaDB integration.
    if(argc>1) {
        boost::property_tree::ptree report;
        auto add=[&](const char* name,const WritePlan& write) {
            boost::property_tree::ptree value,statements;
            for(const auto& sql:write.statements){boost::property_tree::ptree v;v.put_value(sql);statements.push_back({"",v});}
            value.add_child("statements",statements);value.put("receipt",write.receiptQuery);report.add_child(name,value);
        };
        assert(PrepareGuildProcurementResumption(task,current,batch,stock,2000,receipt,recovery,why));add("resume",recovery.plan);
        goal={"cancelled","item",2770,5,0,1,0};
        assert(PrepareGuildProcurementCancellation(task,task.context,goal,batch,stock,2000,receipt,recovery,why));add("cancel",recovery.plan);
        assert(PlanGuildProcurementMaterials(task,surplus,stock,plan,why));
        auto next=task;++next.revision;next.updatedAtMs=2000;
        add("trim",ResourceReservationWrite(next,task.revision,receipt,plan.changes,stock));
        GuildProcurementHandoff handoff;
        assert(PrepareGuildProcurementHandoff(task,task.context,merged,{stock[0]},2000,receipt,handoff,why));add("merged",handoff.plan);
        boost::property_tree::write_json(std::cout,report);
    }
}
