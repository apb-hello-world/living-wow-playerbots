#include "LivingGuildDeposit.h"
#include "LivingGuildSaveFence.h"
#include "LivingGuildDeliverySettlement.h"
#include <cassert>
#include <thread>
#include <limits>
using namespace LivingActivity;
const std::string TaskId="637bd562-36d2-5b01-bc01-e2d831c49f38";
const std::string Operation="ff2efbdf-f0ec-4539-b840-299847970c00";
static GuildDepositQuote Quote() {
    GuildDepositQuote q;q.job={12,31,804,2840,8,0,"supply_iron",false};
    q.actor=804;q.item=1234;q.itemCount=10;q.amount=4;q.deposited=0;q.bankCount=2;q.bagCount=12;
    q.money=500;q.goalTarget=50;q.goalReserved=3;q.position=255*256+23;q.tab=0;q.bank=321;
    return q;
}
static void Settlement() {
    auto job=Quote().job;job.incomingMail=91;
    Task task;task.id=task.root=TaskId;task.actor=task.context.actor=901;
    task.source="guild_delivery";task.sourceKey=GuildDeliverySourceKey(job,task.actor);
    task.kind=Kind::GuildDelivery;task.priority=Priority::Delivery;task.mode=Mode::Active;task.accepted=true;
    task.phase=Phase::Verifying;task.createdAtMs=task.updatedAtMs=1000;
    task.context.boot=Operation;task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.checkpoint.step="guild_bank_deposit";task.checkpoint.data=EncodeGuildDeliveryJob(job);
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;
    ResourceClaim stored;stored.id=Operation;stored.task=TaskId;stored.actor=901;stored.itemGuid=1000;
    stored.itemEntry=2447;stored.quantity=3;stored.location="bank";stored.state="held";claims.claims={stored};
    const std::vector<NativeResourceBalance> stock{{901,1000,2447,3,0,"bank"}};
    GuildDeliverySettlement result;std::string why;
    assert(PrepareGuildDeliverySettlement(task,task.context,claims,1001,Operation,result,why,stock));
    assert(result.task.phase==Phase::Completed && result.task.revision==task.revision+1);
    assert(result.claims.size()==1 && result.claims[0].after.state=="released");
    assert(result.claims[0].after.quantity==3 && result.claims[0].after.itemGuid==1000);
    for(unsigned fault=0;fault<12;++fault) {
        auto badTask=task;auto badClaims=claims;auto badStock=stock;
        switch(fault) {
        case 0:badTask.accepted=false;break;
        case 1:badTask.mode=Mode::Observe;break;
        case 2:badTask.phase=Phase::Preparing;break;
        case 3:badClaims.complete=false;break;
        case 4:badClaims.claims[0].location="mail";badClaims.claims[0].nativeReference=91;break;
        case 5:badClaims.claims[0].location="bags";break;
        case 6:badClaims.claims[0].state="reconciling";break;
        case 7:badClaims.claims[0].itemEntry=job.entry;break;
        case 8:badStock[0].quantity=2;break;
        case 9:badStock[0].actor=902;break;
        case 10:badClaims.claims.push_back(stored);break;
        case 11:badTask.context.actor=902;break;
        }
        assert(!PrepareGuildDeliverySettlement(badTask,task.context,badClaims,1001,Operation,result,why,badStock));
    }
    task.phase=Phase::Reconciling;auto restart=task.context;++restart.actorGeneration;
    assert(PrepareGuildDeliverySettlement(task,restart,claims,1001,Operation,result,why,stock));
    assert(result.task.context==restart);
}
int main() {
    Settlement();
    assert(GuildDepositRetryAt(0,1000)==0);
    assert(GuildDepositRetryAt(1,1000)==301000);
    assert(GuildDepositRetryAt(2,1000)==1801000);
    assert(GuildDepositRetryAt(9,1000)==1801000); // Never permanently give up on accepted items.
    assert(GuildDepositRetryAt(1,UINT64_MAX-2)==UINT64_MAX);
    auto q=Quote();assert(ValidGuildDepositQuote(q));
    auto text=EncodeGuildDepositQuote(q);GuildDepositQuote decoded;
    assert(DecodeGuildDepositQuote(text,decoded) && EncodeGuildDepositQuote(decoded)==text);
    assert(!DecodeGuildDepositQuote(text+" ",decoded));
    auto duplicate=text;duplicate.insert(1,"\"actor\":804,");assert(!DecodeGuildDepositQuote(duplicate,decoded));
    auto invalid=q;invalid.amount=11;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.amount=9;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.deposited=8;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.bankCount=UINT32_MAX;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.goalTarget=8;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.job.money=true;assert(!ValidGuildDepositQuote(invalid));
    invalid=q;invalid.tab=6;assert(!DecodeGuildDepositQuote(EncodeGuildDepositQuote(invalid),decoded));
    assert(VerifyGuildDeposit(q,6,8,6,500));
    assert(!VerifyGuildDeposit(q,6,8,2,500)); // Consumption without a bank gain is not a delivery.
    assert(!VerifyGuildDeposit(q,10,12,6,500)); // A deposit log alone is not proof.
    assert(!VerifyGuildDeposit(q,6,8,6,501));
    assert(!VerifyGuildDeposit(q,0,2,6,500)); // Do not consume the unassigned remainder.
    Task outcome;outcome.id=TaskId;outcome.actor=q.actor;outcome.revision=4;
    const auto delivered=GuildDepositNativeProof(q,outcome,6,8,6,500);
    assert(delivered.find("g.state='active'")!=std::string::npos);
    const auto rejected=GuildDepositNativeProof(q,outcome,10,12,2,500);
    assert(!rejected.empty() && rejected.find("g.state='active'")==std::string::npos);
    assert(rejected.find("d.deposited_quantity=0")!=std::string::npos);
    assert(GuildDepositNativeProof(q,outcome,6,8,2,500).empty());
    assert(GuildDepositNativeProof(q,outcome,6,8,6,501).empty());
    ResourceClaim claim;claim.id=Operation;claim.task=TaskId;claim.actor=q.actor;claim.itemGuid=q.item;
    claim.itemEntry=q.job.entry;claim.quantity=4;claim.location="bags";claim.state="held";
    assert(ExactGuildDepositClaim(q,claim,TaskId));
    claim.actor++;assert(!ExactGuildDepositClaim(q,claim,TaskId));claim.actor--;
    claim.quantity=9;assert(!ExactGuildDepositClaim(q,claim,TaskId));claim.quantity=4;
    claim.location="mail";assert(!ExactGuildDepositClaim(q,claim,TaskId));claim.location="bags";
    claim.state="consumed";assert(!ExactGuildDepositClaim(q,claim,TaskId));
    q.job.incomingMail=91;q.actor=901;assert(ValidGuildDepositQuote(q)); // A real courier need not be donor.
    q.job.incomingMail=0;assert(!ValidGuildDepositQuote(q));
    GuildSaveFence fence;
    assert(!fence.Blocks(31));assert(!fence.Hold(0,804,Operation));
    assert(!fence.Hold(31,804,"pretend"));assert(fence.Hold(31,804,Operation));
    assert(fence.Hold(31,804,Operation));assert(!fence.Hold(31,901,TaskId));
    assert(fence.Blocks(31));assert(fence.Blocks(31,804));assert(fence.Blocks(31,804,TaskId));
    assert(fence.Blocks(31,901,Operation));assert(!fence.Blocks(31,804,Operation));assert(!fence.Blocks(32));
    assert(!fence.Release(31,901,Operation));assert(!fence.Release(31,804,TaskId));
    std::thread reader([&]{for(unsigned n=0;n<1000;++n)assert(fence.Blocks(31,901));});
    for(unsigned n=0;n<100;++n){assert(fence.Hold(32,901,TaskId));assert(fence.Release(32,901,TaskId));}
    reader.join();assert(fence.Release(31,804,Operation));assert(!fence.Blocks(31));
    assert(!fence.Release(31,804,Operation));
    for(unsigned g=1;g<=16;++g)assert(fence.Hold(g,804,Operation));
    assert(!fence.Hold(17,804,Operation));
}
