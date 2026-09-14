#include "LivingGatherRecovery.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;
int main() {
    Task saved;saved.id=saved.root="ef5947a7-9ad7-586a-a5bf-68a57293c630";
    saved.source="guild_procurement";saved.priority=Priority::Delivery;
    saved.actor=saved.context.actor=166;saved.context.policyRevision=6;
    saved.mode=Mode::Active;saved.kind=Kind::GuildProcurement;saved.phase=Phase::Reconciling;saved.accepted=true;
    saved.revision=11;saved.createdAtMs=1;saved.updatedAtMs=2;
    saved.checkpoint.step="guild_requested_gather";saved.checkpoint.blocker="native_gather_outcome_uncertain";
    const GuildProcurementJob job{4,166,2447,1,"herb","3b237288-51c1-4f2a-9a21-63152cad95e2"};
    saved.checkpoint.data=EncodeGuildProcurementJob(job);saved.sourceKey=GuildProcurementSourceKey(job);
    NativeGatherResult prior;
    prior.before={166,2447,182,2366,1,70,150,70,2024,0,17370383789919398718ull};
    prior.value=70;prior.maximum=150;prior.money=2024;prior.generation=1789393052400000ull;
    prior.started=prior.effect=prior.finished=prior.succeeded=true; // Cast completed, not acquisition.
    StoredCraftOperation row;row.acknowledged=true;row.journalDigest=std::string(64,'a');
    row.receipt.id="323e8e41-64b3-4f6a-b873-edbb3aaf2d13";row.receipt.task=saved.id;
    row.receipt.taskRevision=10;row.receipt.kind="gather_open";row.receipt.state=OperationState::Reconciling;
    row.receipt.evidence="native_gather_outcome_uncertain";
    row.receipt.nativeReference="gather:17370383789919398718:generation:1789393052400000";
    row.beforeState="{\"effects\":21,\"persistence\":2,\"native\":"+EncodeNativeGatherQuote(prior.before)+'}';
    row.afterState=EncodeNativeGatherResult(prior);
    row.afterState.erase(row.afterState.rfind(",\"loot_type\":"));row.afterState+='}'; // Deployed pre-type receipt.
    const ProfessionHistoryRow fields{"166","11","1",row.receipt.id,saved.id,"10","gather_open","reconciling",
        row.receipt.nativeReference,row.beforeState,row.afterState,row.receipt.evidence,row.journalDigest};
    std::string why;ProfessionHistoryCursor cursor;
    if(!cursor.Begin(saved,{fields},why) || !cursor.Advance(why)) {std::cerr<<why<<'\n';return 1;}
    assert(cursor.Result().complete && cursor.Result().interruptedGather && cursor.Result().attempts.empty());
    assert(!cursor.Begin(saved,{fields,fields},why));
    assert(cursor.Begin(saved,{fields},why) && cursor.Advance(why));
    const auto history=cursor.Result();
    assert(ProfessionHistoryQuery(saved).find("kind='gather_open' AND state IN ('intent','reconciling')")!=std::string::npos);
    NativeGatherResult decoded;assert(DecodeInterruptedGather(saved,row,decoded,why));
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;
    WorldContext current=saved.context;current.boot="ee3e834a-dd9b-4e13-80bb-950a76f56b55";
    current.actorGeneration=current.mapGeneration=1;
    NativeGatherResult native;native.before=prior.before;native.value=70;native.maximum=150;native.money=2024;
    GuildProcurementRecovery result;const std::string receipt="714bd310-a567-4ec2-847a-91a51d64b58f";
    if(!PrepareInterruptedGather(saved,current,history,claims,native,3,receipt,result,why)) {std::cerr<<why<<'\n';return 1;}
    assert(result.task.id==saved.id && result.task.root==saved.root && result.task.revision==12 &&
        result.task.phase==Phase::Verifying && result.task.checkpoint.data==saved.checkpoint.data && result.claims.empty());
    std::string sql;for(const auto& s:result.plan.statements)sql+=s;
    for(const auto* guard:{"native_gather_loot_abandoned_on_restart","prior_observation","acquired_quantity",
        "SHA2(CONCAT(o.before_state,'|',o.after_state),256)","owner_guid=166 AND itemEntry=2447 AND count>0",
        "value=70 AND max=150","money=2024","source_task_id=","state IN ('intent','reconciling'))=1"})
        assert(sql.find(guard)!=std::string::npos);
    for(const auto* forbidden:{"DELETE ","UPDATE item_instance","UPDATE characters SET money","UPDATE character_skills"})
        assert(sql.find(forbidden)==std::string::npos);
    // Already-rebound work can never relabel an operation a second time.
    assert(!PrepareInterruptedGather(result.task,current,history,claims,native,4,receipt,result,why));
    for(unsigned field=0;field<18;++field) {
        auto bad=saved;auto h=history;auto c=claims;auto n=native;auto world=current;
        switch(field) {
        case 0:bad.context.boot=current.boot;break;case 1:bad.context.actorGeneration=1;break;
        case 2:bad.context.mapGeneration=1;break;case 3:world.boot.clear();break;case 4:world.actor=167;break;
        case 5:world.session="human_party";world.sessionRevision=1;break;case 6:h.complete=false;break;
        case 7:h.interruptedGather.reset();break;case 8:h.revision=10;break;case 9:c.complete=false;break;
        case 10:c.claims.push_back(ResourceClaim{});break;case 11:++n.money;break;case 12:++n.value;break;
        case 13:++n.maximum;break;case 14:n.bagCount=1;break;case 15:n.generation=1;break;
        case 16:n.owned=true;break;default:n.uncertain=true;break;
        }
        assert(!PrepareInterruptedGather(bad,world,h,c,n,3,receipt,result,why));
    }
    for(unsigned field=0;field<16;++field) {
        auto bad=row;auto r=prior;
        switch(field) {
        case 0:bad.acknowledged=false;break;case 1:bad.journalDigest="bad";break;
        case 2:bad.receipt.taskRevision=9;break;case 3:bad.receipt.state=OperationState::Intent;break;
        case 4:bad.receipt.nativeReference="other";break;case 5:bad.beforeState="{}";break;
        case 6:r.uncertain=true;break;case 7:r.finished=false;break;case 8:r.effect=false;break;
        case 9:r.succeeded=false;break;case 10:r.bagCount=1;break;case 11:r.money=2025;break;
        case 12:r.generation=0;break;case 13:r.value=69;break;case 14:r.maximum=151;break;
        default:r.lootType=3;break;
        }
        if(field>=6) {r.lootType=field==15?3:6;bad.afterState=EncodeNativeGatherResult(r);}
        assert(!DecodeInterruptedGather(saved,bad,decoded,why));
    }
    prior.lootType=6;row.afterState=EncodeNativeGatherResult(prior);
    assert(DecodeInterruptedGather(saved,row,decoded,why));
}
