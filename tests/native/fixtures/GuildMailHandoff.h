#pragma once
#include "LivingGuildMailHandoff.h"
namespace GuildMailFixture {
using namespace LivingActivity;
inline GuildMailQuote Quote() {
    GuildMailQuote q;q.job={14,31,9797,2770,4,90013,"guild_mail_handoff_fixture",false};
    q.sender=9798;q.receiver=9799;q.item=9798004;q.moneyBefore=500;q.postage=30;
    q.delay=120;q.bagBefore=q.totalBefore=4;q.position=65303;q.mailbox=123;
    return q;
}
inline Task Sender(const GuildMailQuote& q) {
    Task task;task.id=task.root="6c7b3141-b7a8-5152-9c6b-2249c1e1dcf4";
    task.actor=task.context.actor=q.sender;task.source="guild_delivery";
    task.sourceKey=GuildDeliverySourceKey(q.job,task.actor);task.kind=Kind::GuildDelivery;
    task.mode=Mode::Active;task.priority=Priority::Delivery;task.accepted=true;task.phase=Phase::Queued;
    task.createdAtMs=task.updatedAtMs=1000;task.context.boot="10d480c0-a8ec-5c30-83d4-fd2ab2044c90";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.checkpoint.step="guild_mail_send";task.checkpoint.data=EncodeGuildDeliveryJob(q.job);return task;
}
inline std::vector<ClaimConsumption> Uses(const Task& task,const GuildMailQuote& q) {
    ResourceClaim item;item.id="92cc2f69-16a8-58f5-8f73-70f47d33042b";item.task=task.id;item.actor=task.actor;
    item.itemGuid=q.item;item.itemEntry=q.job.entry;item.quantity=q.job.quantity;item.location="bags";item.state="held";
    ResourceClaim money;money.id="f8f68a19-5ef4-51b2-a67b-7205345dad99";money.task=task.id;money.actor=task.actor;
    money.copper=q.postage;money.location="money";money.state="held";
    return {{item,q.job.quantity},{money,q.postage}};
}
inline NativeResourceBalance Parcel(const GuildMailQuote& q) {return {q.receiver,q.item,q.job.entry,q.job.quantity,0,"mail",90014};}
inline Task Recipient(const Task& source,const GuildMailQuote& q) {
    auto task=source;task.id=task.root="f510630b-82a5-5b8f-b352-4b6be012d360";
    task.actor=task.context.actor=q.receiver;task.phase=Phase::Queued;task.revision=1;task.ownerGeneration=0;
    const auto job=GuildMailRecipientJob(q,90014);task.sourceKey=GuildDeliverySourceKey(job,task.actor);
    task.checkpoint.data=EncodeGuildDeliveryJob(job);task.checkpoint.step="guild_mail_prepare";
    task.createdAtMs=task.updatedAtMs=source.updatedAtMs;return task;
}
inline OperationResult Proof(const Task& source) {
    OperationResult proof;proof.id="ee529e5b-4c50-5082-b060-f3862e62b5d1";proof.task=source.id;
    proof.kind="guild_mail_send";proof.taskRevision=source.revision-1;proof.state=OperationState::Verified;
    proof.evidence="native_guild_parcel_postage_and_handoff_observed";proof.nativeReference="mail:90014:item:9798004";return proof;
}
}
