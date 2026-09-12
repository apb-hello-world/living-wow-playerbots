#include "LivingGuildDeposit.h"
#include "LivingGuildSaveFence.h"
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
int main() {
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
