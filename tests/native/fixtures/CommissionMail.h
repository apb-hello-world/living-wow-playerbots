#pragma once
#include "LivingCommissionMail.h"
inline void TestCommissionMail() {
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={"lwc-123","wow-tx-existing-offer","mail",EncodeProfessionJob(recipe),703,9,120,1000};
    job.craft=job.agreement.recipe;job.craftFinishedRevision=5;
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";task.actor=task.context.actor=703;
    task.source="commission_job";task.sourceKey=job.agreement.id;task.kind=Kind::Commission;
    task.mode=Mode::Active;task.phase=Phase::Preparing;task.revision=5;task.checkpoint.data=EncodeCommissionJob(job);
    task.createdAtMs=task.updatedAtMs=1000;task.accepted=true;
    task.context.boot="ff2efbdf-f0ec-4539-b840-299847970c00";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    CommissionMailQuote q{"lwc-123",703,9,103,2454,1,1,100,30,120,120,23,123456789};
    assert(ValidCommissionMailQuote(q));const auto encoded=EncodeCommissionMailQuote(q);CommissionMailQuote decoded;
    assert(DecodeCommissionMailQuote(encoded,decoded) && EncodeCommissionMailQuote(decoded)==encoded);
    auto malformed=encoded;malformed.insert(1,"\"cod\":0,");assert(!DecodeCommissionMailQuote(malformed,decoded));
    malformed=encoded;const auto at=malformed.find("\"120\"");assert(at!=std::string::npos);
    malformed.replace(at,5,"\"4294967296\"");assert(!DecodeCommissionMailQuote(malformed,decoded));
    ResourceClaim item;item.id="d1879146-6e96-4e71-827d-6d12d965edb7";item.task=task.id;item.actor=703;
    item.itemGuid=103;item.itemEntry=2454;item.quantity=1;item.state="held";item.location="bags";
    ResourceClaim money;money.id="f90f9bb6-58c8-4d82-a3be-a25d3fec47b7";money.task=task.id;money.actor=703;
    money.copper=30;money.state="held";money.location="money";
    const std::vector<ClaimConsumption> uses={{item,1},{money,30}};
    assert(ExactCommissionMailConsumption(task,q,uses));
    for(unsigned i=0;i<8;++i) {
        auto changed=q;auto c=uses;auto t=task;
        if(i==0)changed.receiver=10;
        if(i==1)changed.cod=0;
        if(i==2)changed.quantity=2;
        if(i==3)changed.item=104;
        if(i==4)c[0].before.task="637bd562-36d2-5b01-bc01-e2d831c49f93";
        if(i==5)c[1].used=29;
        if(i==6)c.pop_back();
        if(i==7){auto unfinished=job;unfinished.craftFinishedRevision=0;t.checkpoint.data=EncodeCommissionJob(unfinished);}
        assert(!ExactCommissionMailConsumption(t,changed,c));
    }
    const std::string operation="ff2efbdf-f0ec-4539-b840-299847970c01";
    AuctionMail sent;sent.id=9832;sent.sender=703;sent.receiver=9;sent.cod=120;sent.itemGuid=103;
    sent.itemEntry=2454;sent.quantity=1;sent.attachments=1;sent.deliveredAt=1100;sent.expiresAt=3000;
    sent.subject=CommissionMailSubject(operation);
    assert(VerifyCommissionMailSent(q,sent,operation));
    const auto proof=CommissionMailSentProof(task,q,sent,operation);
    assert(proof.find("m.cod=120")!=std::string::npos && proof.find("i.owner_guid=9")!=std::string::npos);
    assert(proof.find("c.money=70")!=std::string::npos && proof.find("i.count=1")!=std::string::npos);
    assert(proof.find("NOT EXISTS (SELECT 1 FROM character_inventory")!=std::string::npos);
    for(unsigned i=0;i<8;++i) {
        auto bad=sent;
        if(i==0)bad.id=0;
        if(i==1)bad.receiver=10;
        if(i==2)bad.cod=0;
        if(i==3)bad.money=120;
        if(i==4)bad.attachments=2;
        if(i==5)bad.itemGuid=104;
        if(i==6)bad.quantity=2;
        if(i==7)bad.subject="unrelated mail";
        assert(!VerifyCommissionMailSent(q,bad,operation) && CommissionMailSentProof(task,q,bad,operation).empty());
    }
    { // Real mail types remain isolated even during nested native calls.
        AuctionMailCapture auction;NormalMailCapture normal;
        NormalMailCapture::Observe(sent);assert(normal.Rows().size()==1 && auction.Rows().empty());
        AuctionMailCapture::Observe(sent);assert(auction.Rows().size()==1 && normal.Rows().size()==1);
        {NormalMailCapture nested;assert(!nested.Valid());}
        assert(normal.Valid());
    }
    NormalMailCapture::Observe(sent);{NormalMailCapture fresh;assert(fresh.Rows().empty());}
    OperationRequest request;request.transition.task=task;++request.transition.task.revision;
    request.transition.task.phase=Phase::Executing;request.transition.expectedRevision=task.revision;
    request.transition.receipt=operation;request.kind="commission_mail_send";request.beforeState=encoded;
    request.persistence=NativePersistence::Inventory;request.effects=Mask(Effect::Inventory)|Mask(Effect::Money);request.consumption=uses;
    request.authorization.task=request.authorization.rootTask=task.id;request.authorization.world=task.context;
    request.authorization.revision=task.revision;request.authorization.ownerGeneration=7;
    request.authorization.origin="commission_mail_send";request.authorization.permittedEffects=request.effects;
    std::string why;
    assert(ValidateOperationRequest(request,task,task.context,nullptr,1000,why));
    auto plan=OperationRequestWrite(request);assert(!plan.statements.empty());
    auto bad=request;bad.effects|=Mask(Effect::Guild);bool rejected=false;
    try{OperationRequestWrite(bad);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    TestAdapter adapter(request);adapter.consumes=true;
    assert(!ValidateOperationAdapter(request,adapter,why) && why=="native_commission_mail_adapter_required");
    // No sent receipt claims completion or moves the customer's money.
    auto complete=task;complete.phase=Phase::Completed;assert(!ValidateCommissionTask(complete,why));
    assert(CommissionMailOperationFromSubject(sent.subject)==operation);
    assert(CommissionMailOperationFromSubject(sent.subject+" ").empty());
    assert(CommissionMailOperationFromSubject("Commission delivery not-an-operation").empty());
    CommissionMailObservation received;received.sendOperation=operation;received.mail=sent.id;
    received.sender=q.sender;received.receiver=q.receiver;received.item=q.item;received.entry=q.entry;received.quantity=1;
    received.copper=q.cod;received.moneyBefore=1000;received.moneyAfter=880;
    received.inventoryBefore=4;received.inventoryAfter=5;received.atMs=2000000;
    auto& payment=received.generated;payment.id=9833;payment.sender=q.receiver;payment.receiver=q.sender;
    payment.money=q.cod;payment.subject=sent.subject;payment.deliveredAt=2000;payment.expiresAt=3000;
    const auto receiptSql=CommissionMailObservationWrite(received);assert(!receiptSql.empty());
    assert(receiptSql.find(SqlValue("commission_customer_received"))!=std::string::npos);
    assert(receiptSql.find("ON DUPLICATE KEY UPDATE operation_id=VALUES(operation_id)")!=std::string::npos);
    for(unsigned i=0;i<10;++i) {
        auto bad=received;
        if(i==0)bad.moneyAfter=1000;
        if(i==1)bad.inventoryAfter=4;
        if(i==2)bad.generated.money=119;
        if(i==3)bad.generated.sender=10;
        if(i==4)bad.generated.receiver=10;
        if(i==5)bad.generated.attachments=1;
        if(i==6)bad.generated.subject="forged";
        if(i==7)bad.quantity=2;
        if(i==8)bad.generated.id=0;
        if(i==9)bad.sendOperation="not-an-operation";
        assert(CommissionMailObservationWrite(bad).empty());
    }
    auto free=received;free.copper=0;free.moneyAfter=free.moneyBefore;free.generated={};
    assert(!CommissionMailObservationWrite(free).empty());
    CommissionMailObservation paid;paid.event=CommissionMailEvent::FeeCollected;paid.sendOperation=operation;
    paid.mail=payment.id;paid.sender=q.receiver;paid.receiver=q.sender;paid.copper=q.cod;
    paid.moneyBefore=70;paid.moneyAfter=190;paid.atMs=2000010;
    assert(!CommissionMailObservationWrite(paid).empty());
    auto overflow=paid;overflow.moneyBefore=UINT32_MAX-10;overflow.moneyAfter=109;
    assert(CommissionMailObservationWrite(overflow).empty());
    auto noFee=paid;noFee.copper=0;assert(CommissionMailObservationWrite(noFee).empty());
    CommissionMailObservation returned;returned.event=CommissionMailEvent::ParcelReturned;
    returned.sendOperation=operation;returned.mail=9834;returned.sender=q.receiver;returned.receiver=q.sender;
    returned.item=q.item;returned.entry=q.entry;returned.quantity=1;returned.atMs=2000000;
    returned.generated=sent;returned.generated.id=returned.mail;returned.generated.sender=q.receiver;
    returned.generated.receiver=q.sender;returned.generated.cod=0;
    assert(!CommissionMailObservationWrite(returned).empty());
    auto badReturn=returned;badReturn.generated.itemGuid=104;assert(CommissionMailObservationWrite(badReturn).empty());
    const auto receiveId=CommissionMailReceiptId(operation,CommissionMailEvent::CustomerReceived);
    assert(IsUuid(receiveId) && receiveId==CommissionMailReceiptId(operation,CommissionMailEvent::CustomerReceived));
    assert(receiveId!=CommissionMailReceiptId(operation,CommissionMailEvent::FeeCollected));
    assert(receiveId!=CommissionMailReceiptId(operation,CommissionMailEvent::ParcelReturned));
}
