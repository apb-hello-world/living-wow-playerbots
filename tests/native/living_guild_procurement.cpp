#include "LivingGuildProcurement.h"
#include "LivingGuildDelivery.h"
#include "LivingProfessionJob.h"
#include "LivingActivityRequests.h"
#include <cassert>
#include <limits>
using namespace LivingActivity;
int main() {
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
    assert(!SavedTaskExecutable(task,task.revision,task.context,0,why) && why=="guild_procurement_executor_not_enabled");
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
}
