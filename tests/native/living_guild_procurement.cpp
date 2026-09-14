#include "LivingGuildProcurement.h"
#include "LivingGuildDelivery.h"
#include "LivingProfessionJob.h"
#include "LivingActivityRequests.h"
#include "LivingTaskItemRequirements.h"
#include "LivingGathering.h"
#include <cassert>
#include <limits>
using namespace LivingActivity;
int main() {
    using GI=GatheringIntent;using GR=GatheringSkillResult;
    assert(CheckGatheringSkill(0,75,1,false,GI::RequestedMaterials)==GR::UnknownSkill);
    assert(CheckGatheringSkill(74,75,75,false,GI::RequestedMaterials)==GR::InsufficientSkill);
    assert(CheckGatheringSkill(75,75,1,false,GI::RequestedMaterials)==GR::Eligible);
    assert(CheckGatheringSkill(375,375,1,false,GI::RequestedMaterials)==GR::Eligible);
    assert(CheckGatheringSkill(305,300,1,false,GI::RequestedMaterials)==GR::Eligible); // Native glove bonus.
    assert(CheckGatheringSkill(75,75,1,false,GI::SkillGain)==GR::SkillCapped);
    assert(CheckGatheringSkill(150,225,1,false,GI::SkillGain)==GR::NoSkillGain);
    assert(CheckGatheringSkill(375,375,1,true,GI::RequestedMaterials)==GR::UnsupportedIntent);
    assert(CheckGatheringSkill(75,150,1,false,GI(99))==GR::UnsupportedIntent);
    // Preserve the deployed skill-up predicate for all ordinary TBC skill
    // bands. Material eligibility does NOT grant skill, tools, access or loot.
    for(uint32_t current=0;current<=450;++current)for(uint32_t required=0;required<=450;++required)
        for(uint32_t maximum:{75u,150u,225u,300u,375u})for(bool fishing:{false,true}) {
            const bool legacy=current && required<=current && maximum>current && (fishing || required+100>=current);
            assert((CheckGatheringSkill(current,maximum,required,fishing,GI::SkillGain)==GR::Eligible)==legacy);
        }
    const GuildProcurementJob job{3,314,2770,20,"goal_copper","ef526383-763e-4a84-b501-12b4c2966321"};
    std::string why;GuildProcurementJob decoded;
    const auto encoded=EncodeGuildProcurementJob(job);
    assert(DecodeGuildProcurementJob(encoded,decoded,why) && EncodeGuildProcurementJob(decoded)==encoded);
    for(const auto* bad:{"{}","{\"workflow\":\"guild_delivery_v1\"}","[]"})assert(!DecodeGuildProcurementJob(bad,decoded,why));
    for(const auto* extra:{",\"money\":100",",\"guild\":4",",\"recipe\":2329",",\"phase\":\"mailed\"",",\"items\":[]"})
        assert(!DecodeGuildProcurementJob(encoded.substr(0,encoded.size()-1)+extra+"}",decoded,why));
    for(const auto* bad:{"0","-1","4294967296","1.5","null","\"020\""}) {
        auto text=encoded;const auto at=text.find("\"quantity\":20");text.replace(at+11,2,bad);
        assert(!DecodeGuildProcurementJob(text,decoded,why));
    }
    auto invalid=job;invalid.entry=0;assert(!ValidGuildProcurementJob(invalid)); // Gold is not an item purchase.
    invalid=job;invalid.goal="bad'goal";assert(!ValidGuildProcurementJob(invalid));
    invalid=job;invalid.batch.clear();assert(!ValidGuildProcurementJob(invalid));
    Task task;task.id=task.root="f7528ef3-b678-4cc4-b4cd-a0db5d6ec17e";
    task.actor=job.donor;task.kind=Kind::GuildProcurement;task.source="guild_procurement";
    task.sourceKey=GuildProcurementSourceKey(job);task.priority=Priority::Delivery;task.mode=Mode::Active;
    task.checkpoint.data=encoded;task.checkpoint.step="profession_service_purchase_vendor";
    assert(ValidateGuildProcurementTask(task,why));
    assert(!IsProfessionJob(task) && !IsManagedGuildDelivery(task)); // Shared service names cannot forge a recipe/parcel.
    std::vector<ProfessionReagent> items;
    assert(ReadTaskItemRequirements(task,items,why) && items.size()==1 && items[0].entry==2770 && items[0].perAttempt==20);
    Kind kind;assert(ParseKind("guild_procurement",kind) && kind==task.kind);
    assert(std::string(Name(kind))=="guild_procurement");
    for(int n=0;n!=5;++n) {
        auto forged=task;
        if(n==0)++forged.actor;
        if(n==1)forged.source="guild_delivery";
        if(n==2)forged.kind=Kind::Commission;
        if(n==3)forged.sourceKey+="x";
        if(n==4)forged.priority=Priority::Human;
        assert(!ValidateGuildProcurementTask(forged,why));
    }
    auto after=task;after.phase=Phase::Traveling;++after.revision;
    assert(PreserveGuildProcurementIntent(task,after,why));
    auto changed=job;++changed.quantity;after.checkpoint.data=EncodeGuildProcurementJob(changed);
    assert(!PreserveGuildProcurementIntent(task,after,why));
    auto executable=task;executable.phase=Phase::Preparing;
    executable.context.actor=task.actor;executable.context.actorGeneration=executable.context.mapGeneration=1;
    executable.context.policyRevision=1;executable.context.boot="60426118-f43b-4869-9b55-60d3b13af70d";
    assert(SavedTaskExecutable(executable,executable.revision,executable.context,0,why));
    auto stale=executable.context;++stale.actorGeneration;
    assert(!SavedTaskExecutable(executable,executable.revision,stale,0,why) && why=="stale_native_context");
    executable.retryAtMs=10;
    assert(!SavedTaskExecutable(executable,executable.revision,executable.context,0,why) && why=="task_backoff");
    executable.retryAtMs=0;executable.phase=Phase::Paused;
    assert(!SavedTaskExecutable(executable,executable.revision,executable.context,0,why) && why=="task_not_executable");
    assert(UnassignedGuildProcurement(50,2,0,8,0)==40);
    assert(UnassignedGuildProcurement(50,2,0,8,15)==25);
    assert(UnassignedGuildProcurement(50,10,8,8,15)==25); // Keep native bank-reservation semantics.
    assert(UnassignedGuildProcurement(50,2,0,8,40)==0);
    assert(UnassignedGuildProcurement(50,2,0,8,std::numeric_limits<uint64_t>::max())==0);
    assert(UnassignedGuildProcurement(UINT32_MAX,UINT32_MAX,0,UINT32_MAX,0)==0);
    assert(UnassignedGuildProcurement(0,0,0,0,0)==0);
    // The verified custody handoff, NOT a purchase receipt alone, moves 5 units
    // from assigned demand to the native delivery ledger. New assignable need
    // is unchanged, while the real delivery quantity increases.
    assert(UnassignedGuildProcurement(50,2,0,8,15)==UnassignedGuildProcurement(50,2,0,13,10));
    GuildProcurementCoverage coverage(job.guild,job.entry,"pending-new-task");
    assert(coverage.Add(task,why) && coverage.Assigned()==20);
    assert(coverage.Add(task,why) && coverage.Assigned()==20); // Cache + same pending write, once.
    after=task;++after.revision;after.phase=Phase::WaitingExternal;
    assert(coverage.Add(after,why) && coverage.Assigned()==20); // Paid mail still covers demand.
    auto pending=after;++pending.revision;pending.phase=Phase::Completed;
    assert(coverage.AddPending(pending,why) && coverage.Assigned()==20); // No parcel exists before the native commit.
    pending.phase=Phase::Cancelled;
    assert(coverage.AddPending(pending,why) && coverage.Assigned()==20); // Failed cancellation keeps its obligation.
    pending.revision=after.revision;assert(!coverage.AddPending(pending,why));
    assert(!coverage.Add(task,why) && why=="guild_procurement_coverage_revision_conflict");
    after.phase=Phase::Completed;
    assert(!coverage.Add(after,why)); // A conflicting same-revision projection is not proof.
    ++after.revision;
    assert(coverage.Add(after,why) && coverage.Assigned()==0); // Verified terminal replacement.
    auto second=task;second.id=second.root="16959684-2f82-4d71-879a-b1455cefdba2";
    auto otherGoal=job;otherGoal.goal="another_copper_goal";otherGoal.donor=45;
    second.actor=otherGoal.donor;second.sourceKey=GuildProcurementSourceKey(otherGoal);
    second.checkpoint.data=EncodeGuildProcurementJob(otherGoal);
    assert(coverage.Add(second,why) && coverage.Assigned()==20); // Shared native bank, not per-goal double orders.
    auto third=task;third.id=third.root="ae830df2-0ab5-4b0f-815f-48636d3a01e7";
    auto otherItem=job;++otherItem.entry;third.sourceKey=GuildProcurementSourceKey(otherItem);
    third.checkpoint.data=EncodeGuildProcurementJob(otherItem);
    assert(coverage.Add(third,why) && coverage.Assigned()==20);
    otherItem=job;++otherItem.guild;third.checkpoint.data=EncodeGuildProcurementJob(otherItem);
    third.sourceKey=GuildProcurementSourceKey(otherItem);
    assert(coverage.Add(third,why) && coverage.Assigned()==20);
    third=task;third.id=third.root="ae830df2-0ab5-4b0f-815f-48636d3a01e7";third.mode=Mode::Observe;
    assert(coverage.Add(third,why) && coverage.Assigned()==20);
    GuildProcurementCoverage excluded(job.guild,job.entry,task.id);
    assert(excluded.Add(task,why) && excluded.Assigned()==0); // Own demand is validated against other assignments.
    auto invalidTask=task;invalidTask.revision=0;
    assert(!coverage.Add(invalidTask,why) && why=="guild_procurement_coverage_identity_invalid");
    invalidTask=task;invalidTask.checkpoint.data="{}";
    assert(!coverage.Add(invalidTask,why)); // Corrupt accepted demand cannot silently disappear.
    Task unrelated;unrelated.source="profession";
    assert(coverage.Add(unrelated,why) && coverage.Assigned()==20);
    GuildProcurementCoverage large(job.guild,job.entry,"");
    auto huge=job;huge.quantity=UINT32_MAX;
    third=task;third.checkpoint.data=EncodeGuildProcurementJob(huge);
    assert(large.Add(third,why));
    third.id=third.root="ae830df2-0ab5-4b0f-815f-48636d3a01e7";
    assert(large.Add(third,why) && large.Assigned()==2ULL*UINT32_MAX);
}
