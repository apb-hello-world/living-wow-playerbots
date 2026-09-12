#include "LivingMailRecovery.h"
#include "LivingActivityTransfer.h"
namespace {
void TestMailRecovery() {
    using namespace LivingActivity;using namespace LivingActivityTest;
    InterruptedFixture f;f.task.checkpoint.step="profession_mail_collect";
    auto row=*f.history.interruptedCraft;row.receipt.kind="mail_collect";
    f.history.interruptedCraft.reset();
    auto c=f.batch.claims.front();c.itemGuid=902;c.itemEntry=765;c.quantity=2;c.location="mail";c.nativeReference=900;
    f.batch.claims={c};NativeMailQuote q;
    q.actor=705;q.mail=900;q.guid=902;q.entry=765;q.quantity=2;q.mailbox=901;q.mailboxEntry=55;
    q.deliveredAt=1;q.expiresAt=9000;q.moneyBefore=100;q.attachmentsBefore=1;
    q.bagBefore=q.totalBefore=1;q.to=65303;q.mergeGuid=800;q.mergeCount=1;
    row.beforeState="{\"effects\":4,\"persistence\":1,\"native\":{\"native\":"+EncodeNativeMailQuote(q)+
        ",\"transfer\":"+ItemTransferIdentity(c)+"}}";
    f.history.interruptedMail=row;
    MailRecoverySnapshot native{q,{705,800,765,1,0,23}};ProfessionPreparation result;std::string why;
    auto run=[&](const InterruptedFixture& v,const MailRecoverySnapshot& n){
        return PrepareInterruptedMail(v.task,v.current,v.history,v.batch,n,1200,v.receipt,result,why);
    };
    assert(run(f,native));
    assert(result.task.id==f.task.id && result.task.revision==5 && result.task.phase==Phase::Verifying);
    assert(result.task.context==f.current && result.task.checkpoint.step=="profession_prepare");
    assert(result.plan.statements[1].find("FROM mail_items mi JOIN item_instance")!=std::string::npos);
    assert(result.plan.statements[1].find("i.count=2")!=std::string::npos);
    assert(result.plan.statements[1].find("i.count=1")!=std::string::npos);
    assert(result.plan.statements.size()==4); // Native items/money/claims are not mutated.
    for(unsigned fault=0;fault<19;++fault) {
        auto bad=f;auto n=native;
        switch(fault) {
        case 0:bad.task.context.boot=f.current.boot;break;
        case 1:++bad.task.context.actorGeneration;break;
        case 2:++bad.task.revision;break;
        case 3:bad.task.phase=Phase::Verifying;break;
        case 4:bad.history.complete=false;break;
        case 5:bad.history.interruptedMail->acknowledged=false;break;
        case 6:bad.history.interruptedMail->receipt.state=OperationState::Verified;break;
        case 7:bad.history.interruptedMail->afterState="{\"effect\":true}";break;
        case 8:bad.history.interruptedMail->beforeState+=' ';break;
        case 9:bad.batch.claims.front().revision++;break;
        case 10:bad.batch.claims.front().quantity++;break;
        case 11:bad.batch.claims.front().location="bags";break;
        case 12:n.unchanged.moneyBefore++;break;
        case 13:n.unchanged.quantity++;break;
        case 14:n.unchanged.attachmentsBefore--;break;
        case 15:n.destination.count++;break;
        case 16:n.destination.guid++;break;
        case 17:bad.batch.claims.push_back(bad.batch.claims.front());break;
        case 18:bad.history.unresolvedOperation=false;break;
        }
        assert(!run(bad,n) && !why.empty() && result.plan.statements.empty());
    }
    ProfessionHistoryRow fields={"705","4","1",row.receipt.id,f.task.id,"4","mail_collect","intent","",row.beforeState,"{}",""};
    ProfessionHistoryCursor cursor;
    assert(cursor.Begin(f.task,{fields},why) && cursor.Advance(why));
    assert(cursor.Result().complete && cursor.Result().interruptedMail && !cursor.Result().interruptedCraft);
    fields[7]="verified";assert(!cursor.Begin(f.task,{fields},why));
}
}
