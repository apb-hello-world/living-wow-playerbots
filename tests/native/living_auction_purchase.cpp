#include "LivingAuctionQuote.h"
#include <cassert>
#include <algorithm>
using namespace LivingActivity;
int main() {
    NativeAuctionQuote q;q.actor=7;q.seller=8;q.house=1;q.auction=99;q.guid=123;q.entry=765;q.quantity=2;
    q.copper=20;q.moneyBefore=100;q.proceeds=19;q.auctioneerEntry=8661;q.auctioneer=10000;q.expiresAt=9000;
    std::string why;NativeAuctionQuote decoded;
    assert(DecodeNativeAuctionQuote(EncodeNativeAuctionQuote(q),decoded));
    for(unsigned n=0;n<8;++n) {
        auto bad=q;
        switch(n){case 0:bad.seller=7;break;case 1:bad.guid=0;break;case 2:bad.quantity=0;break;
            case 3:bad.copper=101;break;case 4:bad.bidder=7;bad.bid=10;break;case 5:bad.bidder=9;break;
            case 6:bad.bid=20;bad.bidder=9;break;default:bad.auctioneer=0;}
        assert(!DecodeNativeAuctionQuote(EncodeNativeAuctionQuote(bad),decoded));
    }
    AuctionMail won;won.id=11;won.sender=1;won.receiver=7;won.itemGuid=123;won.itemEntry=765;
    won.quantity=2;won.attachments=1;won.deliveredAt=1000;won.expiresAt=100000;won.subject="765:0:1";
    AuctionMail sold=won;sold.id=12;sold.receiver=8;sold.money=19;sold.subject="765:0:2";
    sold.itemGuid=sold.itemEntry=sold.quantity=sold.attachments=0;
    AuctionMail pending=sold;pending.id=13;pending.money=0;pending.subject="765:0:6";
    std::vector<AuctionMail> mails{pending,sold,won};NativeResourceBalance acquired;
    assert(VerifyAuctionMails(q,mails,acquired,why));
    assert(acquired.location=="mail" && acquired.nativeReference==11 && acquired.itemGuid==123);
    // The original listing determines mail provenance even when purchased
    // through a different auctioneer in the same faction market.
    auto otherCity=q;otherCity.auctioneerEntry=8670;otherCity.auctioneer=20000;
    assert(VerifyAuctionMails(otherCity,mails,acquired,why));
    otherCity.house=4;assert(!VerifyAuctionMails(otherCity,mails,acquired,why));
    for(unsigned n=0;n<9;++n) {
        auto bad=mails;
        switch(n){case 0:bad.pop_back();break;case 1:bad[0].id=12;break;case 2:++bad[1].money;break;
            case 3:++bad[2].itemGuid;break;case 4:++bad[2].quantity;break;case 5:bad[2].cod=1;break;
            case 6:bad[2].receiver=9;break;case 7:bad[2].sender=9;break;default:bad[2].expiresAt=1000;}
        assert(!VerifyAuctionMails(q,bad,acquired,why));
    }
    q.bidder=9;q.bid=10;AuctionMail refund=sold;refund.id=14;refund.receiver=9;refund.money=10;refund.subject="765:0:0";
    assert(!VerifyAuctionMails(q,mails,acquired,why));
    mails.push_back(refund);assert(VerifyAuctionMails(q,mails,acquired,why));
    std::reverse(mails.begin(),mails.end());assert(VerifyAuctionMails(q,mails,acquired,why));
    mails[0].money=11;assert(!VerifyAuctionMails(q,mails,acquired,why));mails[0].money=10;
    assert(VerifyAuctionMails(q,mails,acquired,why));
    {AuctionMailCapture capture;for(const auto& m:mails)AuctionMailCapture::Observe(m);
        assert(capture.Valid() && capture.Rows().size()==4);AuctionMailCapture::Observe(won);assert(!capture.Valid());}
    {AuctionMailCapture outer;{AuctionMailCapture nested;assert(!nested.Valid());}assert(outer.Valid());}
    AuctionMailCapture::Observe(won); // No scope, no capture or gameplay side effect.
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f38";task.actor=7;
    task.kind=Kind::Profession;task.source="profession_job";task.sourceKey="test";task.accepted=true;task.mode=Mode::Active;
    task.phase=Phase::Verifying;task.revision=4;task.createdAtMs=100;task.updatedAtMs=400;
    task.context.actor=7;task.context.actorGeneration=1;task.context.mapGeneration=1;task.context.policyRevision=1;
    task.context.boot="ff2efbdf-f0ec-4539-b840-299847970c00";
    const std::string operation="ff2efbdf-f0ec-4539-b840-299847970c01", receipt="ff2efbdf-f0ec-4539-b840-299847970c02";
    auto gain=MailGainClaim(task,operation,q.Stack(),acquired);
    assert(gain.expectedRevision==0 && gain.after.quantity==2 && gain.after.location=="mail" && gain.after.nativeReference==11);
    ResourceClaimBook book;assert(book.FinishRestore());
    assert(book.ReservePending(receipt,{gain},{acquired})==ClaimInstall::Installed);
    assert(book.Protection().ProtectedItem(7,123,765)==2);
    auto changed=acquired;changed.location="bags";changed.nativeReference=0;
    assert(!VerifyNativeMailGain(7,q.Stack(),changed,why));
    ResourceClaim money;money.id="ff2efbdf-f0ec-4539-b840-299847970c03";money.task=task.id;money.actor=7;
    money.location="money";money.state="held";money.copper=20;
    OperationResult proof;proof.id=operation;proof.task=task.id;proof.taskRevision=3;proof.kind="auction_purchase";
    proof.state=OperationState::Verified;proof.nativeReference="auction:99:item:123";proof.evidence="native_auction_payment_and_mail_observed";
    const auto output=MailedOperationWrite(task,3,proof,receipt,"{\"mails\":"+AuctionMailsJson(mails)+'}',{{money,20}},q.Stack(),acquired);
    assert(output.changes.size()==2 && output.changes[0].after.state=="consumed" && output.changes[1].after.location=="mail");
    assert(output.journal.statements.front().find("$.mail_gain")!=std::string::npos);
    assert(output.journal.receiptQuery.find("c.native_reference=11")!=std::string::npos);
    assert(output.journal.receiptQuery.find("c.location='mail'")!=std::string::npos);
    for(unsigned n=0;n<4;++n){bool rejected=false;try {
        auto p=proof;auto c=acquired;auto s=q.Stack();
        switch(n){case 0:p.state=OperationState::Rejected;break;case 1:p.kind="vendor_purchase";break;
            case 2:c.quantity=3;break;default:s.guid=124;}
        MailedOperationWrite(task,3,p,receipt,"{}",{{money,20}},s,c);
    }catch(...){rejected=true;}assert(rejected);}
}
