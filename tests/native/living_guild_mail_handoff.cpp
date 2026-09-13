#include "fixtures/GuildMailHandoff.h"
#include <cassert>
#include <iostream>
using namespace LivingActivity;
using namespace GuildMailFixture;
int main() {
    auto q=Quote();auto source=Sender(q);source.revision=4;source.phase=Phase::Verifying;
    auto target=Recipient(source,q);auto uses=Uses(source,q);auto parcel=Parcel(q);auto proof=Proof(source);
    const std::string receipt="3e2be28d-0f20-51f4-a7a4-c40abbdc6d9c",admission="89ac89cf-f718-51c8-aaf0-8393d86602d6";
    GuildMailQuote decoded;const auto text=EncodeGuildMailQuote(q);
    assert(DecodeGuildMailQuote(text,decoded) && EncodeGuildMailQuote(decoded)==text);
    assert(!DecodeGuildMailQuote(text+' ',decoded));
    auto duplicate=text;duplicate.insert(1,"\"sender\":9798,");assert(!DecodeGuildMailQuote(duplicate,decoded));
    const auto v1=EncodeGuildDeliveryJob(q.job);GuildDeliveryJob job;std::string why;
    assert(v1.find("guild_delivery_v1")!=std::string::npos && v1.find("mail_sender")==std::string::npos);
    assert(DecodeGuildDeliveryJob(v1,job,why) && EncodeGuildDeliveryJob(job)==v1);
    const auto v2=target.checkpoint.data;
    assert(DecodeGuildDeliveryJob(v2,job,why) && job.donor==9797 && job.mailSender==9798 && job.incomingMail==90014);
    assert(EncodeGuildDeliveryJob(job)==v2 && GuildDeliveryMailSender(job)==9798);
    auto bad=v2;bad.replace(bad.find("9798",bad.find("mail_sender")),4,"9797");assert(!DecodeGuildDeliveryJob(bad,job,why));
    bad=v2;bad.replace(bad.find("guild_delivery_v2"),17,"guild_delivery_v1");assert(!DecodeGuildDeliveryJob(bad,job,why));
    assert(ExactGuildMailConsumption(source,q,uses));
    const auto result=GuildMailHandoffWrite(source,3,proof,receipt,"{}",q,uses,parcel,target,admission);
    assert(result.changes.size()==3 && result.changes[0].after.state=="consumed" && result.changes[1].after.state=="consumed");
    assert(result.recipientClaim.actor==9799 && result.recipientClaim.task==target.id && result.recipientClaim.nativeReference==90014);
    assert(result.journal.receiptQuery.find(SqlValue(admission))!=std::string::npos &&
        result.journal.receiptQuery.find(SqlValue(result.recipientClaim.id))!=std::string::npos);
    size_t largest=0;for(const auto& sql:result.journal.statements)largest=std::max(largest,sql.size());
    std::cout<<"handoff native bounds: statements="<<result.journal.statements.size()<<" largest="<<largest
        <<" receipt="<<result.journal.receiptQuery.size()<<std::endl;
    assert(largest<32*1024 && result.journal.receiptQuery.size()<=30000);
    // Production-sized source IDs, timestamps, party context and a measured
    // native result must also fit the pinned CMaNGOS save/query limits.
    auto longQ=q;longQ.job.goal=std::string(64,'g');longQ.job.delivery=UINT64_MAX;
    auto longSource=Sender(longQ);longSource.revision=14;longSource.phase=Phase::Verifying;
    longSource.createdAtMs=longSource.updatedAtMs=1789268399000;
    auto longTarget=Recipient(longSource,longQ);longTarget.context.session=std::string(120,'s');
    const auto bounded=GuildMailHandoffWrite(longSource,13,Proof(longSource),receipt,
        R"({"mail":90014,"sender":9798,"receiver":9799,"original_donor":9797,"item":9798004,"money":470})",
        longQ,Uses(longSource,longQ),Parcel(longQ),longTarget,admission);
    for(const auto& sql:bounded.journal.statements)assert(sql.size()<32*1024);
    assert(bounded.journal.receiptQuery.size()<=30000);
    for(unsigned fault=0;fault<23;++fault) {
        auto x=q;auto s=source;auto t=target;auto u=uses;auto p=parcel;auto r=proof;
        switch(fault) {
        case 0:x.receiver=x.sender;break;
        case 1:x.job.donor=x.sender;break;
        case 2:x.postage=0;break;
        case 3:x.moneyBefore=29;break;
        case 4:u[0].before.quantity=5;break;
        case 5:u[0].used=3;break;
        case 6:u[1].before.copper=31;break;
        case 7:u[1].before.task=target.id;break;
        case 8:u[0].before.state="consumed";break;
        case 9:u.push_back(u.front());break;
        case 10:p.actor=q.sender;break;
        case 11:p.nativeReference=q.job.incomingMail;break;
        case 12:p.quantity=5;break;
        case 13:p.itemGuid++;break;
        case 14:t.actor++;t.context.actor++;break;
        case 15:t.phase=Phase::Completed;break;
        case 16:t.revision=2;break;
        case 17:t.ownerGeneration=1;break;
        case 18:t.context.policyRevision++;break;
        case 19:r.state=OperationState::Reconciling;break;
        case 20:r.evidence="mail_attempted";break;
        case 21:s.phase=Phase::Preparing;break;
        case 22:t.mode=Mode::Observe;break;
        }
        bool threw=false;
        try{GuildMailHandoffWrite(s,3,r,receipt,"{}",x,u,p,t,admission);}catch(const std::exception&){threw=true;}
        assert(threw);
    }
    // Claims bridge the native effect and receipt acknowledgement across TWO
    // actors, without weakening the existing same-actor transfer validator.
    ResourceClaimBook book;assert(book.FinishRestore());
    assert(book.InstallReceipt({{uses[0].before,0},{uses[1].before,0}})==ClaimInstall::Installed);
    assert(book.ReservePending(receipt,{{result.recipientClaim,0}},{parcel})==ClaimInstall::Invalid);
    assert(book.ReserveMailedHandoff(receipt,result.changes,parcel)==ClaimInstall::Installed);
    assert(book.ReserveMailedHandoff(receipt,result.changes,parcel)==ClaimInstall::Duplicate);
    assert(book.Protection().ProtectedItem(q.sender,q.item,q.job.entry)==4);
    assert(book.Protection().ProtectedItem(q.receiver,q.item,q.job.entry)==4);
    assert(book.CommitReservation(receipt)==ClaimInstall::Installed);
    assert(book.InstallReceipt({result.changes[0],result.changes[1]})==ClaimInstall::Duplicate);
    assert(book.Inspect(uses[0].before.id)->state=="consumed");
    assert(book.Protection().ProtectedItem(q.receiver,q.item,q.job.entry)==4);
    assert(book.Reader().Inspect()->ProtectedItem(q.item)==4);
    std::cout<<"guild mail handoff: lineage, exact claims, cross-actor protection and 23 rejected mutations passed\n";
}
