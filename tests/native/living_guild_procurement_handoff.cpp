#include "LivingGuildProcurementHandoff.h"
#include "LivingLegacyResourceView.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;
int main(int argc,char**) {
    GuildProcurementJob job{3,314,2770,5,"goal_copper","ef526383-763e-4a84-b501-12b4c2966321"};
    Task saved;saved.id=saved.root="f7528ef3-b678-4cc4-b4cd-a0db5d6ec17e";saved.actor=314;
    saved.source="guild_procurement";saved.sourceKey=GuildProcurementSourceKey(job);saved.kind=Kind::GuildProcurement;
    saved.mode=Mode::Active;saved.phase=Phase::Preparing;saved.createdAtMs=saved.updatedAtMs=1000;
    saved.context.actor=314;saved.context.actorGeneration=saved.context.mapGeneration=1;
    saved.context.boot="f8628b78-d726-437b-8e32-8361c9e90aab";
    saved.checkpoint.data=EncodeGuildProcurementJob(job);saved.checkpoint.step="guild_procurement_prepare";
    const auto receipt="da723f8c-4d34-45d5-ae0a-ae87e7f5bf31";
    ResourceClaim a;a.id="178a1ba5-f6c0-4e1c-871d-f160ce09ec92";a.task=saved.id;a.actor=314;
    a.itemGuid=9001;a.itemEntry=2770;a.quantity=2;a.location="bags";a.state="held";
    auto b=a;b.id="2e423458-599f-40f3-989c-56e9154ad79d";b.itemGuid=9002;b.quantity=3;
    UnsettledClaimBatch batch{1,true,{a,b}};
    std::vector<NativeResourceBalance> stock{{314,9001,2770,8,0,"bags",0},{314,9002,2770,3,0,"bags",0}};
    GuildProcurementHandoff out;std::string why;
    assert(PrepareGuildProcurementHandoff(saved,saved.context,batch,stock,2000,receipt,out,why));
    assert(out.task.phase==Phase::Completed && out.task.checkpoint.step=="guild_procurement_handed_off");
    assert(out.parcels.size()==2 && out.parcels[0].quantity==2 && out.parcels[1].quantity==3);
    assert(out.claims.size()==2 && out.claims[0].after.state=="released" && out.claims[0].after.revision==2);
    const auto good=out;
    assert(out.plan.statements.size()==9); // 3 locks, task/outbox, 2 parcels, 2 releases.
    for(const auto& sql:out.plan.statements) {
        assert(sql.find("DELETE ")==std::string::npos);
        assert(sql.find("INSERT INTO item_instance")==std::string::npos);
        assert(sql.find("UPDATE item_instance")==std::string::npos);
        assert(sql.find("UPDATE characters")==std::string::npos);
        assert(sql.find("INSERT INTO guild_bank_item")==std::string::npos);
    }
    assert(out.plan.statements[3].find("v.slot BETWEEN 23 AND 38")!=std::string::npos);
    assert(out.plan.statements[3].find("i.count=8")!=std::string::npos); // Transfer only the claimed 2 of a stack of 8.
    assert(out.plan.statements[3].find("g.provenance IN")!=std::string::npos);
    assert(out.plan.receiptQuery.find("d.source_claim_id=")!=std::string::npos);
    assert(out.plan.receiptQuery.find("d.carrier_guid=")==std::string::npos); // Survives later courier handoff.
    for(int scenario=0;scenario<13;++scenario) {
        auto task=saved;auto context=saved.context;auto claims=batch;auto balances=stock;
        if(scenario==0)task.phase=Phase::Completed;
        if(scenario==1)task.mode=Mode::Observe;
        if(scenario==2)++context.mapGeneration;
        if(scenario==3)claims.complete=false;
        if(scenario==4)claims.claims.pop_back();
        if(scenario==5)claims.claims[0].location=balances[0].location="bank";
        if(scenario==6)claims.claims[0].state="in_transfer";
        if(scenario==7){claims.claims[0].nativeReference=5;claims.claims[0].location="mail";}
        if(scenario==8)claims.claims[0].quantity=6;
        if(scenario==9)balances[0].quantity=1;
        if(scenario==10)claims.claims[1].id=claims.claims[0].id;
        if(scenario==11)claims.claims[0].task="a777d6d8-c912-4253-8ce3-6e16c9eaa442";
        if(scenario==12)claims.claims[0].state="proposed";
        assert(!PrepareGuildProcurementHandoff(task,context,claims,balances,2000,receipt,out,why));
    }
    auto merged=batch;merged.claims[1].itemGuid=a.itemGuid;
    assert(PrepareGuildProcurementHandoff(saved,saved.context,merged,{stock[0]},2000,receipt,out,why));
    assert(out.parcels.size()==1 && out.parcels[0].id==a.id && out.parcels[0].quantity==5 && out.claims.size()==2);
    std::reverse(merged.claims.begin(),merged.claims.end());
    assert(PrepareGuildProcurementHandoff(saved,saved.context,merged,{stock[0]},2000,receipt,out,why));
    assert(out.parcels[0].id==a.id && out.parcels[0].quantity==5);
    assert(out.plan.receiptQuery.find("d.source_claim_id="+SqlValue(a.id))!=std::string::npos);
    // Additional personal preparations may be released but never converted to
    // requested goods. Incoming/unreconciled stock cannot be silently freed.
    auto money=a;money.id="8e9283f2-5dfe-45c3-89a8-9bbd382f092e";
    money.itemGuid=money.itemEntry=money.quantity=0;money.copper=30;money.location="money";
    batch.claims.push_back(money);stock.push_back({314,0,0,0,100,"money",0});
    assert(PrepareGuildProcurementHandoff(saved,saved.context,batch,stock,2000,receipt,out,why));
    assert(out.parcels.size()==2 && out.claims.size()==3);
    LegacyResourcePublisher publisher;LegacyResourceView view;view.blocked=true;publisher.Replace(view);
    assert(publisher.Inspect()->Item(7) && publisher.Inspect()->Entry(314,2770));
    publisher.SetItem(7,true);assert(publisher.Inspect()->items.count(7));
    view.blocked=false;view.items.insert(9001);view.entries.emplace(314,2770);publisher.Replace(view);
    assert(!publisher.Inspect()->Item(7) && publisher.Inspect()->Item(9001) && publisher.Inspect()->Entry(314,2770));
    // Native MariaDB integration consumes the actual compiled journal, not
    // reconstructed SQL. Output contains fixture values, never private data.
    if(argc>1) {
        boost::property_tree::ptree report,admission,handoff;
        const auto initial=TaskWrite(saved,0,"15e40b72-38a2-4923-8d77-1f6a2d9c5e80","task_admitted");
        for(const auto& sql:initial.statements){boost::property_tree::ptree v;v.put_value(sql);admission.push_back({"",v});}
        for(const auto& sql:good.plan.statements){boost::property_tree::ptree v;v.put_value(sql);handoff.push_back({"",v});}
        report.add_child("admission",admission);report.add_child("handoff",handoff);
        report.put("receipt",good.plan.receiptQuery);
        boost::property_tree::write_json(std::cout,report);
    }
}
