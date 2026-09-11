#ifndef LIVING_TEST_CRAFT_EVIDENCE_H
#define LIVING_TEST_CRAFT_EVIDENCE_H
#include "LivingProfessionEvidence.h"
#include <cassert>
namespace LivingActivityTest {
    using namespace LivingActivity;
    inline std::string FrameJson(const CraftFrame& frame) {
        std::string json="{\"skill\":"+std::to_string(frame.skill)+",\"money\":"+std::to_string(frame.money)+",\"stacks\":[";
        for (const auto& item : frame.stacks) {
            if (json.back()!='[') json+=',';
            json+='['+std::to_string(item.guid)+','+std::to_string(item.entry)+','+std::to_string(item.count)+','+
                std::to_string(item.bagGuid)+','+std::to_string(item.slot)+']';
        }
        return json+"]}";
    }
    inline StoredCraftOperation SavedCraft(const Task& task,const ProfessionJob& job,const CraftFrame& before,
        const CraftFrame& after,bool interrupted,bool reserveFullStacks=false) {
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
            c.itemGuid=item.guid;c.itemEntry=item.entry;c.quantity=reserveFullStacks ? item.count : need.perAttempt;
            inputs.push_back({c,need.perAttempt});
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

}
#endif
