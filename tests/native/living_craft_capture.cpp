#include "LivingCraftCapture.h"
#include "LivingProfessionEvidence.h"
#include <boost/property_tree/json_parser.hpp>
#include <atomic>
#include <cassert>
#include <sstream>
#include <thread>
using namespace LivingActivity;
namespace {
    using Tree=boost::property_tree::ptree;
    std::string FrameJson(const CraftFrame& frame) {
        std::string json="{\"skill\":"+std::to_string(frame.skill)+",\"money\":"+std::to_string(frame.money)+",\"stacks\":[";
        for (const auto& item : frame.stacks) {
            if (json.back()!='[') json+=',';
            json+='['+std::to_string(item.guid)+','+std::to_string(item.entry)+','+std::to_string(item.count)+','+
                std::to_string(item.bagGuid)+','+std::to_string(item.slot)+']';
        }
        return json+"]}";
    }
    StoredCraftOperation SavedCraft(const Task& task,const ProfessionJob& job,const CraftFrame& before,
        const CraftFrame& after,bool interrupted) {
        StoredCraftOperation row;row.acknowledged=true;
        row.receipt.id="2c60183b-4a76-41f4-a62e-01d8c65e273d";row.receipt.task=task.id;
        row.receipt.taskRevision=4;row.receipt.kind="profession_craft";
        row.receipt.state=interrupted ? OperationState::Rejected : OperationState::Verified;
        row.receipt.nativeReference="spell:"+std::to_string(job.recipe)+":operation:"+row.receipt.id;
        row.receipt.evidence=interrupted ? "native_cast_cancelled_without_effect" : "native_craft_consumption_output_and_skill_observed";
        const char* ids[]={"137e6854-06ea-5e21-8371-6b40c8c19f4e","2c60183b-4a76-41f4-a62e-01d8c65e273d","37cfba40-31fe-4664-86db-fb2e87036cb8"};
        std::vector<ClaimConsumption> inputs;
        for (const auto& need : job.reagents) for (const auto& item : before.stacks) if (item.entry==need.entry) {
            ResourceClaim c;c.id=ids[inputs.size()];c.actor=task.actor;c.task=task.root;c.state="held";c.location="bags";
            c.itemGuid=item.guid;c.itemEntry=item.entry;c.quantity=need.perAttempt;inputs.push_back({c,need.perAttempt});
        }
        const ItemGainSpec output{job.outputEntry,1};
        const auto native="{\"recipe\":"+std::to_string(job.recipe)+",\"skill\":"+std::to_string(before.skill)+",\"money\":"+std::to_string(before.money)+'}';
        row.beforeState="{\"effects\":21,\"persistence\":2,\"native\":"+ClaimedNativeState(native,inputs)+",\"item_gain\":"+ItemGainSpecJson(output)+'}';
        const auto capture="{\"recipe\":"+std::to_string(job.recipe)+",\"skill_id\":"+std::to_string(job.skill)+
            ",\"effect_entered\":"+(interrupted ? "false" : "true")+",\"native_finished\":true,\"native_succeeded\":"+
            (interrupted ? "false" : "true")+",\"created_calls\":"+(interrupted ? "0" : "1")+
            ",\"created_quantity\":"+(interrupted ? "0" : "1")+",\"before\":"+FrameJson(before)+",\"after\":"+FrameJson(after)+'}';
        if (interrupted) {row.afterState=capture;return row;}
        const auto physical=VerifyCraftResources(task.actor,job,before,after,output);
        assert(physical.result==CraftEvidence::Verified && !physical.attempt.nativeEffectVerified && !physical.attempt.committed);
        std::string proof="{\"result\":"+capture+",\"item_gain\":"+ItemGainSpecJson(output)+",\"stacks\":[";
        for (const auto& gain : physical.gains) {
            if (proof.back()!='[') proof+=',';
            proof+="{\"guid\":"+std::to_string(gain.after.guid)+",\"bag\":"+std::to_string(gain.after.bagGuid)+",\"slot\":"+
                std::to_string(gain.after.slot)+",\"before\":"+std::to_string(gain.before.count)+",\"after\":"+
                std::to_string(gain.after.count)+",\"added\":"+std::to_string(gain.added)+'}';
        }
        row.afterState=ClaimedNativeState(proof+"]}",inputs,8192);return row;
    }
    template<class Change> void Mutate(std::string& json,Change change) {
        Tree tree;std::istringstream input(json);boost::property_tree::read_json(input,tree);change(tree);
        std::ostringstream output;boost::property_tree::write_json(output,tree,false);json=output.str();
    }
    ProfessionHistoryRow HistoryRow(const Task& task,const StoredCraftOperation& operation,bool unresolved=false) {
        const auto& r=operation.receipt;
        const auto state=r.state==OperationState::Verified ? "verified" : r.state==OperationState::Rejected ? "rejected" :
            r.state==OperationState::Intent ? "intent" : "reconciling";
        return {std::to_string(task.actor),std::to_string(task.revision),unresolved ? "1" : "0",r.id,r.task,
            std::to_string(r.taskRevision),r.kind,state,r.nativeReference,operation.beforeState,operation.afterState,r.evidence};
    }
    void StoredHistory(const Task& task,const StoredCraftOperation& saved,const StoredCraftOperation& cancelled) {
        ProfessionHistoryCursor cursor;std::string blocker;
        const auto first=HistoryRow(task,saved);
        assert(cursor.Begin(task,{first},blocker) && !cursor.Result().complete && cursor.Result().attempts.empty());
        assert(cursor.Advance(blocker) && cursor.Result().complete && cursor.Result().attempts.size()==1);
        assert(cursor.Result().task==task.id && cursor.Result().revision==task.revision && !cursor.Result().unresolvedOperation);
        assert(cursor.Advance(blocker) && cursor.Result().attempts.size()==1); // No duplicate decode.
        auto second=HistoryRow(task,cancelled);second[3]="47cfba40-31fe-4664-86db-fb2e87036cb8";second[5]="7";
        second[8]="spell:2329:operation:"+second[3];
        assert(cursor.Begin(task,{first,second},blocker));
        assert(cursor.Advance(blocker) && !cursor.Result().complete && cursor.Result().attempts.size()==1);
        assert(cursor.Advance(blocker) && cursor.Result().complete && cursor.Result().attempts.size()==2);
        assert(!cursor.Result().attempts.back().nativeEffectVerified);
        auto pending=second;pending[2]="1";pending[7]="intent";pending[8]="";pending[10]="{}";pending[11]="";
        auto previous=first;previous[2]="1";
        assert(cursor.Begin(task,{previous,pending},blocker));
        assert(cursor.Advance(blocker) && cursor.Advance(blocker) && cursor.Result().complete);
        assert(cursor.Result().unresolvedOperation && cursor.Result().attempts.size()==1);
        ProfessionHistoryRow empty{};empty[0]=std::to_string(task.actor);empty[1]=std::to_string(task.revision);empty[2]="0";
        assert(cursor.Begin(task,{empty},blocker) && cursor.Result().complete && cursor.Result().attempts.empty());
        empty[2]="1"; // Mail/vendor or a different root can still block this actor.
        assert(cursor.Begin(task,{empty},blocker) && cursor.Result().complete && cursor.Result().unresolvedOperation);
        auto fails=[&](std::vector<ProfessionHistoryRow> rows) {
            assert(!cursor.Begin(task,rows,blocker) && !blocker.empty());
            assert(!cursor.Result().complete && cursor.Result().attempts.empty());
        };
        fails({});fails({empty,first});fails({first,first});fails({second,first});
        fails(std::vector<ProfessionHistoryRow>(21,first));
        for (const auto& changed : std::vector<std::pair<size_t,std::string>>{
            {0,"999999"},{1,"8"},{1,"18446744073709551616"},{2,"true"},{3,"not-an-id"},
            {4,"47cfba40-31fe-4664-86db-fb2e87036cb8"},{5,"0"},{5,"10"},{6,"vendor_purchase"},
            {7,"success"},{8,std::string(161,'x')},{9,std::string(8193,' ')},{10,std::string(8193,' ')},{11,std::string(65,'x')}}) {
            auto row=first;row[changed.first]=changed.second;fails({row});
        }
        auto row=pending;row[2]="0";fails({row});
        row=second;row[2]="1";fails({first,row});
        row=first;row[9]="{}";
        assert(cursor.Begin(task,{row},blocker) && !cursor.Advance(blocker));
        assert(!cursor.Result().complete && cursor.Result().attempts.empty());
        // Previous valid proofs never leak through an invalid later receipt.
        row=second;row[10]="{}";
        assert(cursor.Begin(task,{first,row},blocker) && cursor.Advance(blocker));
        assert(!cursor.Advance(blocker) && !cursor.Result().complete && cursor.Result().attempts.empty());
        auto changedTask=task;changedTask.mode=Mode::Observe;
        bool threw=false;try {(void)ProfessionHistoryQuery(changedTask);}catch (const std::invalid_argument&) {threw=true;}
        assert(threw);
        const auto query=ProfessionHistoryQuery(task);
        assert(query.find("LEFT JOIN")!=std::string::npos && query.find("LIMIT 6")!=std::string::npos);
        assert(query.find("owner.actor_guid=t.actor_guid")!=std::string::npos);
    }
    void StoredEvidence(const ProfessionJob& job,const CraftFrame& before,const CraftFrame& after) {
        Task task;task.id=task.root="137e6854-06ea-5e21-8371-6b40c8c19f4e";task.actor=before.actor;
        task.mode=Mode::Active;task.kind=Kind::Profession;task.source="profession_job";task.accepted=true;task.revision=9;
        task.checkpoint.data=EncodeProfessionJob(job);
        const auto saved=SavedCraft(task,job,before,after,false);StoredCraftProof proof;std::string blocker;
        assert(DecodeStoredCraftProof(task,saved,proof,blocker) && blocker.empty());
        assert(proof.attempt.committed && proof.attempt.nativeEffectVerified && proof.inputs.size()==3 && proof.gains.size()==1);
        assert(proof.attempt.receipt.id==saved.receipt.id && proof.attempt.skillAfter==after.skill);
        auto fails=[&](const Task& owner,const StoredCraftOperation& changed) {
            proof.attempt.committed=proof.attempt.nativeEffectVerified=true;
            assert(!DecodeStoredCraftProof(owner,changed,proof,blocker) && !blocker.empty());
            assert(!proof.attempt.committed && !proof.attempt.nativeEffectVerified && proof.inputs.empty() && proof.gains.empty());
        };
        auto row=saved;row.acknowledged=false;fails(task,row);
        row=saved;row.receipt.state=OperationState::Intent;fails(task,row);
        row=saved;row.receipt.state=OperationState::Reconciling;fails(task,row);
        row=saved;row.receipt.task="37cfba40-31fe-4664-86db-fb2e87036cb8";fails(task,row);
        row=saved;row.receipt.taskRevision=task.revision;fails(task,row);
        row=saved;row.receipt.kind="vendor_purchase";fails(task,row);
        row=saved;row.receipt.nativeReference="spell:2329";fails(task,row);
        row=saved;row.receipt.evidence="plausible_success";fails(task,row);
        auto owner=task;owner.actor++;fails(owner,saved);
        owner=task;owner.mode=Mode::Observe;fails(owner,saved);
        row=saved;row.beforeState.insert(1,"\"effects\":21,");fails(task,row);
        row=saved;row.afterState=std::string(8193,' ');fails(task,row);
        for (const auto& field : {"effects","persistence"}) {
            row=saved;Mutate(row.beforeState,[&](Tree& p){p.put(field,0);});fails(task,row);
        }
        for (const auto& field : {"native.result.recipe","native.result.skill_id","native.result.created_quantity",
             "native.result.created_calls","native.result.after.money","native.result.after.skill","native.item_gain.quantity"}) {
            row=saved;Mutate(row.afterState,[&](Tree& p){p.put(field,0);});fails(task,row);
        }
        row=saved;Mutate(row.afterState,[](Tree& p){p.put("native.result.native_finished",false);});fails(task,row);
        row=saved;Mutate(row.afterState,[](Tree& p){p.put("native.result.before.money","18446744073709551616");});fails(task,row);
        row=saved;Mutate(row.afterState,[](Tree& p){auto& a=p.get_child("claimed_consumption");a.push_back(*a.begin());});fails(task,row);
        row=saved;Mutate(row.afterState,[](Tree& p){auto& a=p.get_child("native.result.after.stacks");a.push_back(*a.begin());});fails(task,row);
        row=saved;Mutate(row.afterState,[](Tree& p){p.get_child("native.stacks").front().second.put("added",2);});fails(task,row);
        row=saved;Mutate(row.afterState,[](Tree& p){p.get_child("claimed_consumption").front().second.put("used",2);});fails(task,row);
        const auto cancelled=SavedCraft(task,job,before,before,true);
        StoredHistory(task,saved,cancelled);
        assert(DecodeStoredCraftProof(task,cancelled,proof,blocker));
        assert(proof.attempt.committed && !proof.attempt.nativeEffectVerified && proof.gains.empty() && proof.attempt.produced.empty());
        assert(proof.attempt.skillBefore==proof.attempt.skillAfter && proof.attempt.receipt.state==OperationState::Rejected);
        for (const auto& field : {"created_calls","created_quantity","after.money","after.skill"}) {
            row=cancelled;Mutate(row.afterState,[&](Tree& p){p.put(field,99);});fails(task,row);
        }
        row=cancelled;Mutate(row.afterState,[](Tree& p){p.put("effect_entered",true);});fails(task,row);
        row=cancelled;Mutate(row.afterState,[](Tree& p){p.put("native_succeeded",true);});fails(task,row);
        // Historical proof requires no fabricated current world/lease stamp.
        // It remains usable after zoning while actual execution needs fresh authority.
        task.context.actorGeneration=100;task.context.mapGeneration=200;
        assert(DecodeStoredCraftProof(task,saved,proof,blocker));
        auto unchanged=after;unchanged.skill=before.skill;
        assert(DecodeStoredCraftProof(task,SavedCraft(task,job,before,unchanged,false),proof,blocker));
        assert(proof.attempt.committed && proof.attempt.skillBefore==proof.attempt.skillAfter);
    }
}
int main() {
    CraftIdentity identity;
    identity.task="137e6854-06ea-5e21-8371-6b40c8c19f4e";
    identity.operation="2c60183b-4a76-41f4-a62e-01d8c65e273d";
    identity.revision=4;identity.ownerGeneration=9;identity.recipe=2329;identity.skill=171;
    identity.world.actor=235;identity.world.policyRevision=1;
    identity.world.actorGeneration=3;identity.world.mapGeneration=1;
    identity.world.boot="37cfba40-31fe-4664-86db-fb2e87036cb8";
    ProfessionJob job;job.recipe=2329;job.skill=171;job.outputEntry=2454;job.outputQuantity=1;
    job.initialSkill=1;job.targetSkill=2;job.reagents={{765,1},{2449,1},{3371,1}};
    const ItemGainSpec output{2454,1};
    CraftFrame before{235,1,500,{{235,100,765,2,0,23},{235,101,2449,1,0,24},
        {235,102,3371,5,200,0},{235,103,2454,2,200,1}}};
    CraftFrame after=before;after.skill=2;after.stacks[0].count=1;
    after.stacks.erase(after.stacks.begin()+1);after.stacks[1].count=4;after.stacks[2].count=3;
    StoredEvidence(job,before,after);
    assert(ValidCraftFrame(before) && ValidCraftFrame(after));
    {
        ResourceView view;assert(view.HasUncertainItem(235,765));assert(view.ProtectedItem(100)>5);
        view.ready=true;assert(!view.HasUncertainItem(235,765));assert(!view.ProtectedItem(100));
        auto exact=std::make_shared<ResourceBucket>();exact->items[100]=10;view.buckets[100%256]=exact;
        assert(!view.UnreservedItem(235,100,765,5) && view.ProtectedItem(100)==10);
        auto uncertain=std::make_shared<ResourceBucket>();uncertain->uncertain[{235,765}]=1;
        view.buckets[235%256]=uncertain;assert(view.HasUncertainItem(235,765));
    }
    auto invalid=before;invalid.stacks.push_back(invalid.stacks.front());assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[1].slot=23;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[0].actor=999;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks[0].count=0;assert(!ValidCraftFrame(invalid));
    invalid=before;invalid.stacks.resize(257);assert(!ValidCraftFrame(invalid));
    CraftCapture capture(identity);
    assert(!capture.ReadFinished());assert(!capture.Created(identity,2454,1));
    assert(!capture.EnterEffect(identity,before));
    auto stale=identity;++stale.revision;assert(!capture.Start(stale,before));
    assert(capture.Start(identity,before));assert(!capture.Start(identity,before));
    assert(!capture.Finish(stale,true,after));
    assert(capture.EnterEffect(identity,before));assert(!capture.EnterEffect(identity,before));
    assert(capture.Created(identity,2454,1));assert(capture.Finish(identity,true,after));
    assert(!capture.Finish(identity,false,before));assert(!capture.Created(identity,2454,1));
    capture.Abandon(); // A destructor cannot overwrite an observed finish.
    const auto observed=*capture.ReadFinished();
    assert(capture.ReadFinished()->nativeSucceeded);
    auto verified=VerifyCraftCapture(identity,job,observed,output);
    assert(verified.result==CraftEvidence::Verified && verified.blocker.empty());
    assert(verified.gains.size()==1 && verified.gains[0].added==1);
    assert(verified.attempt.nativeEffectVerified && !verified.attempt.committed && verified.attempt.receipt.id.empty());
    assert(verified.attempt.skillBefore==1 && verified.attempt.skillAfter==2);
    auto fails=[&](CraftCaptureResult changed,const char* blocker) {
        const auto result=VerifyCraftCapture(identity,job,changed,output);
        assert(result.result==CraftEvidence::Reconciling && result.blocker==blocker);
        assert(!result.attempt.committed && !result.attempt.nativeEffectVerified);
    };
    auto changed=observed;++changed.identity.revision;fails(changed,"native_craft_identity_mismatch");
    changed=observed;++changed.identity.ownerGeneration;fails(changed,"native_craft_identity_mismatch");
    changed=observed;++changed.identity.world.mapGeneration;fails(changed,"native_craft_identity_mismatch");
    changed=observed;changed.nativeFinished=false;fails(changed,"native_craft_completion_proof_missing");
    changed=observed;changed.createdCalls=0;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdCalls=2;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdQuantity=2;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.createdEntry=3371;fails(changed,"native_craft_creation_receipt_mismatch");
    changed=observed;changed.nativeSucceeded=false;fails(changed,"native_craft_cancelled_after_possible_effect");
    changed=observed;changed.after.money=499;fails(changed,"native_craft_wallet_or_skill_changed_unexpectedly");
    changed=observed;changed.after.stacks[0].count=2;fails(changed,"native_craft_consumption_mismatch");
    changed=observed;changed.after.stacks[0].guid=555;fails(changed,"native_craft_input_identity_changed");
    changed=observed;changed.after.stacks[0].slot=25;fails(changed,"native_craft_input_identity_changed");
    changed=observed;changed.after.stacks[2].guid=101;fails(changed,"native_craft_input_identity_reused_as_output");
    changed=observed;changed.after.stacks[2].count=2;fails(changed,"native_item_gain_quantity_or_identity_mismatch");
    // A native craft can be successful without gaining skill. The profession
    // job's skill-gain criterion remains unmet; the callback cannot invent it.
    changed=observed;changed.after.skill=1;
    verified=VerifyCraftCapture(identity,job,changed,output);
    assert(verified.result==CraftEvidence::Verified && verified.attempt.skillAfter==1 && !verified.attempt.committed);
    // Item creation after wholly consuming a stack may reuse its SLOT, not its GUID.
    changed=observed;changed.before.stacks.pop_back();changed.after.stacks[2]={235,104,2454,1,0,24};
    assert(VerifyCraftCapture(identity,job,changed,output).result==CraftEvidence::Verified);
    {
        CraftCapture cancelled(identity);assert(cancelled.Start(identity,before));
        assert(cancelled.Finish(identity,false,before));
        assert(VerifyCraftCapture(identity,job,*cancelled.ReadFinished(),output).result==CraftEvidence::RejectedWithoutEffect);
    }
    {
        CraftCapture uncertain(identity);assert(uncertain.Start(identity,before));uncertain.Abandon();
        assert(!uncertain.Finish(identity,true,after));
        fails(*uncertain.ReadFinished(),"native_craft_completion_callback_missing");
    }
    {
        CraftCapture moved(identity);assert(moved.Start(identity,before));auto mutated=before;--mutated.stacks[0].count;
        assert(!moved.EnterEffect(identity,mutated));assert(moved.Finish(identity,false,mutated));
        fails(*moved.ReadFinished(),"native_craft_inputs_changed_before_effect");
    }
    {
        CraftCapture repeated(identity);assert(repeated.Start(identity,before));assert(repeated.EnterEffect(identity,before));
        assert(repeated.Created(identity,2454,1));assert(repeated.Created(identity,2454,1));assert(repeated.Finish(identity,true,after));
        fails(*repeated.ReadFinished(),"native_craft_creation_receipt_mismatch");
    }
    {
        CraftCapture race(identity);std::atomic<unsigned> starts{0};
        std::thread first([&]{if(race.Start(identity,before))++starts;});
        std::thread second([&]{if(race.Start(identity,before))++starts;});first.join();second.join();assert(starts==1);
        assert(race.EnterEffect(identity,before));assert(race.Created(identity,2454,1));
        std::atomic<unsigned> finishes{0};
        std::thread one([&]{if(race.Finish(identity,true,after))++finishes;});
        std::thread two([&]{if(race.Finish(identity,true,after))++finishes;});one.join();two.join();assert(finishes==1);
        assert(VerifyCraftCapture(identity,job,*race.ReadFinished(),output).result==CraftEvidence::Verified);
    }
}
