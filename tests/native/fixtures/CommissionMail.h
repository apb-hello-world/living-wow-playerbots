#pragma once
#include "LivingCommissionMail.h"
#include "LivingCommissionSettlement.h"
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
    {
        auto stored=item;stored.id="d1879146-6e96-4e71-827d-6d12d965ed10";
        stored.itemGuid=104;stored.itemEntry=765;stored.quantity=7;stored.location="bank";stored.revision=2;
        UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;claims.claims={item,money,stored};
        std::vector<NativeResourceBalance> stock={{703,103,2454,3,0,"bags"},{703,0,0,0,100,"money"},{703,104,765,7,0,"bank"}};
        CommissionReturnClosure ready;std::string why;
        const std::string receipt="d1879146-6e96-4e71-827d-6d12d965ed11";
        assert(PrepareCommissionParcelClaims(task,task.context,claims,stock,2000,receipt,ready,why));
        assert(ready.claims.size()==1 && ready.claims.front().after.id==stored.id && ready.claims.front().after.state=="released");
        assert(ready.task.phase==Phase::Preparing && ready.task.checkpoint.step=="commission_mail_prepare");
        assert(ready.plan.statements[1].find("native_bank_stack_deposited")!=std::string::npos);
        for(unsigned i=0;i<5;++i) {
            auto bad=claims;auto native=stock;
            if(i==0)bad.claims[0].quantity=2;
            if(i==1)native.pop_back();
            if(i==2)bad.claims[2].location="mail";
            if(i==3)bad.complete=false;
            if(i==4)bad.claims[1].copper=31;
            assert(!PrepareCommissionParcelClaims(task,task.context,bad,native,2000,receipt,ready,why));
        }
        claims.claims.pop_back();stock.pop_back();
        assert(!PrepareCommissionParcelClaims(task,task.context,claims,stock,2000,receipt,ready,why));
        auto restored=task;restored.context.boot.clear();restored.context.actorGeneration=restored.context.mapGeneration=0;
        restored.phase=Phase::Traveling;restored.retryAtMs=5000;
        assert(PrepareCommissionParcelClaims(restored,task.context,claims,stock,2000,receipt,ready,why));
        assert(ready.claims.empty() && ready.task.context==task.context && ready.task.retryAtMs==5000);
    }
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
    { // Native replay: source stack 2 -> mailed 1 + retained new GUID 1.
        auto split=q;split.count=2;split.splitPosition=uint16_t(255u<<8|24u);
        auto sending=request;sending.beforeState=EncodeCommissionMailQuote(split);
        const NativeResourceBalance retained{703,104,2454,1,0,"bags"};
        const std::vector<NativeResourceBalance> before={{703,103,2454,2,0,"bags"},{703,0,0,0,100,"money"}};
        const std::vector<NativeResourceBalance> after={retained,{703,0,0,0,70,"money"}};
        assert(!VerifyConsumedNativeResources(sending,before,after,why));
        assert(VerifyConsumedNativeResources(sending,before,after,why,retained));
        for(unsigned i=0;i<10;++i) {
            auto r=retained;auto start=before;auto end=after;auto req=sending;
            if(i==0)r.itemGuid=103;
            if(i==1)r.actor=9;
            if(i==2)r.quantity=2;
            if(i==3)r.itemEntry=765;
            if(i==4)r.location="bank";
            if(i==5)end.erase(end.begin());
            if(i==6)end.push_back({703,103,2454,1,0,"bags"});
            if(i==7)start.push_back(retained);
            if(i==8)end.back().copper=100;
            if(i==9)req.beforeState=encoded;
            assert(!VerifyConsumedNativeResources(req,start,end,why,r));
        }
    }
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
    auto owner=task;owner.revision=8;owner.phase=Phase::Verifying;owner.checkpoint.step="commission_mail_send";
    ProfessionHistory history;history.task=owner.id;history.revision=owner.revision;history.complete=true;history.unresolvedOperation=false;
    StoredCraftOperation sendRow;sendRow.acknowledged=true;sendRow.journalDigest=std::string(64,'a');
    sendRow.receipt.id=operation;sendRow.receipt.task=owner.id;sendRow.receipt.taskRevision=6;
    sendRow.receipt.kind="commission_mail_send";sendRow.receipt.state=OperationState::Verified;
    sendRow.receipt.evidence="native_commission_parcel_and_postage_observed";sendRow.receipt.nativeReference="mail:9832:item:103";
    sendRow.beforeState="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(encoded,uses,8192)+'}';
    sendRow.afterState=ClaimedNativeState("{\"mail\":9832,\"item\":103,\"receiver\":9,\"cod\":120,\"delivered_at\":1100,\"expires_at\":3000,\"postage\":30,\"customer_received\":false,\"fee_paid\":false}",uses,8192);
    history.commissionMail={sendRow};CommissionDeliveryProof delivered;
    assert(InspectCommissionDelivery(owner,history,delivered,why) && delivered.state==CommissionDeliveryState::WaitingCustomer);
    auto eventRow=[&](const CommissionMailObservation& e,const char* kind) {
        StoredCraftOperation row;row.acknowledged=true;row.journalDigest=std::string(64,'b');
        row.receipt.id=CommissionMailReceiptId(operation,e.event);row.receipt.task=owner.id;row.receipt.taskRevision=7;
        row.receipt.kind=kind;row.receipt.state=OperationState::Verified;row.receipt.evidence="native_mail_transaction_observed";
        row.receipt.nativeReference="mail:"+std::to_string(e.mail);row.beforeState="{}";
        boost::property_tree::ptree p;p.put("version",1);p.put("send_operation",operation);p.put("mail",e.mail);
        p.put("sender",e.sender);p.put("receiver",e.receiver);p.put("item",e.item);p.put("entry",e.entry);p.put("quantity",e.quantity);
        p.put("copper",e.copper);p.put("money_before",e.moneyBefore);p.put("money_after",e.moneyAfter);
        p.put("inventory_before",e.inventoryBefore);p.put("inventory_after",e.inventoryAfter);
        p.put("payment_mail",e.event==CommissionMailEvent::CustomerReceived?e.generated.id:0);p.put("observed_at_ms",e.atMs);
        row.afterState=EnchantCodec::Json(p);return row;
    };
    const auto receivedRow=eventRow(received,"commission_customer_received"),feeRow=eventRow(paid,"commission_fee_collected");
    history.commissionMail.push_back(receivedRow);
    assert(InspectCommissionDelivery(owner,history,delivered,why) && delivered.state==CommissionDeliveryState::WaitingFee);
    history.commissionMail.push_back(feeRow);
    assert(InspectCommissionDelivery(owner,history,delivered,why) && delivered.state==CommissionDeliveryState::Complete);
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;ProfessionPreparation settled;
    const auto settlement="d1879146-6e96-4e71-827d-6d12d965edb8";
    assert(PrepareCommissionSettlement(owner,owner.context,history,claims,2002000,settlement,settled,why));
    assert(settled.task.phase==Phase::Completed && settled.task.id==owner.id && settled.task.revision==9);
    auto restarted=owner;restarted.context.boot.clear();restarted.context.actorGeneration=restarted.context.mapGeneration=0;
    assert(PrepareCommissionSettlement(restarted,owner.context,history,claims,2002000,settlement,settled,why));
    for(unsigned i=0;i<10;++i) {
        auto bad=history;
        if(i==0)bad.complete=false;
        if(i==1)bad.unresolvedOperation=true;
        if(i==2)bad.commissionMail[0].receipt.state=OperationState::Intent;
        if(i==3)bad.commissionMail[1].receipt.id=settlement;
        if(i==4)bad.commissionMail[2].receipt.nativeReference="mail:9999";
        if(i==5)bad.commissionMail.push_back(receivedRow);
        if(i==6)bad.commissionMail[0].journalDigest.clear();
        if(i==7){auto wrong=paid;wrong.moneyAfter=70;bad.commissionMail[2]=eventRow(wrong,"commission_fee_collected");}
        if(i==8){auto wrong=received;wrong.receiver=10;bad.commissionMail[1]=eventRow(wrong,"commission_customer_received");}
        if(i==9)bad.commissionMail.push_back(eventRow(returned,"commission_parcel_returned"));
        assert(!InspectCommissionDelivery(owner,bad,delivered,why));
    }
    auto returnedHistory=history;returnedHistory.commissionMail={sendRow,eventRow(returned,"commission_parcel_returned")};
    assert(InspectCommissionDelivery(owner,returnedHistory,delivered,why) && delivered.state==CommissionDeliveryState::Returned);
    ResourceClaim returnedClaim;
    assert(ReturnedCommissionClaim(owner,returnedHistory,returnedClaim,why));
    {
        const auto mailClaim=returnedClaim;
        auto travelling=restarted;travelling.phase=Phase::Traveling;travelling.retryAtMs=2100000;
        UnsettledClaimBatch resumeClaims;resumeClaims.complete=true;resumeClaims.bookRevision=2;resumeClaims.claims={mailClaim};
        ProfessionPreparation resumed;
        assert(PrepareCommissionReturnResume(travelling,owner.context,returnedHistory,resumeClaims,{},2002000,settlement,resumed,why));
        assert(resumed.task.id==owner.id && resumed.task.phase==Phase::Preparing && resumed.task.retryAtMs==2100000);
        assert(resumed.task.context==owner.context && resumed.task.checkpoint.data==owner.checkpoint.data);
        assert(!PrepareCommissionReturnResume(owner,owner.context,returnedHistory,resumeClaims,{},2002000,settlement,resumed,why));
        auto pendingHistory=returnedHistory;pendingHistory.unresolvedOperation=true;
        assert(!PrepareCommissionReturnResume(travelling,owner.context,pendingHistory,resumeClaims,{},2002000,settlement,resumed,why));
        auto held=mailClaim;held.location="bags";held.nativeReference=0;++held.revision;
        UnsettledClaimBatch returnedClaims;returnedClaims.complete=true;returnedClaims.bookRevision=2;returnedClaims.claims={held};
        std::vector<NativeResourceBalance> stock={{held.actor,held.itemGuid,held.itemEntry,uint32_t(held.quantity),0,"bags"}};
        CommissionReturnClosure closed;
        assert(PrepareCommissionReturnClosure(owner,owner.context,returnedHistory,returnedClaims,stock,2002000,settlement,closed,why));
        assert(closed.task.phase==Phase::Failed && closed.task.checkpoint.step=="commission_returned");
        assert(closed.claims.size()==1 && closed.claims[0].after.state=="released");
        assert(closed.plan.statements[1].find("native_mail_attachment_collected")!=std::string::npos);
        assert(PrepareCommissionReturnClosure(restarted,owner.context,returnedHistory,returnedClaims,stock,2002000,settlement,closed,why));
        // A merged stack retains the exact claim quantity, not the whole stack.
        returnedClaims.claims[0].itemGuid=999;stock[0].itemGuid=999;stock[0].quantity=4;
        assert(PrepareCommissionReturnClosure(owner,owner.context,returnedHistory,returnedClaims,stock,2002000,settlement,closed,why));
        assert(closed.claims[0].after.quantity==held.quantity);
        returnedClaims.claims={mailClaim};
        assert(!PrepareCommissionReturnClosure(owner,owner.context,returnedHistory,returnedClaims,stock,2002000,settlement,closed,why));
        returnedClaims.claims={held};returnedClaims.complete=false;
        assert(!PrepareCommissionReturnClosure(owner,owner.context,returnedHistory,returnedClaims,stock,2002000,settlement,closed,why));
        returnedClaims.complete=true;
        assert(!PrepareCommissionReturnClosure(owner,owner.context,returnedHistory,returnedClaims,{},2002000,settlement,closed,why));
    }
    assert(returnedClaim.id==delivered.returned && returnedClaim.task==owner.id && returnedClaim.actor==owner.actor);
    assert(returnedClaim.itemGuid==q.item && returnedClaim.itemEntry==q.entry && returnedClaim.quantity==q.quantity);
    assert(returnedClaim.nativeReference==9834 && returnedClaim.location=="mail" && returnedClaim.state=="held");
    auto staleReturn=returnedHistory;staleReturn.revision--;
    assert(!ReturnedCommissionClaim(owner,staleReturn,returnedClaim,why));
    assert(!ReturnedCommissionClaim(owner,history,returnedClaim,why));
    assert(PrepareCommissionSettlement(owner,owner.context,returnedHistory,claims,2002000,settlement,settled,why));
    assert(settled.task.phase==Phase::Reconciling && settled.task.checkpoint.blocker=="commission_returned_parcel_reconciliation_required");
    claims.claims={item};assert(!PrepareCommissionSettlement(owner,owner.context,history,claims,2002000,settlement,settled,why));
    auto unsent=restarted;unsent.phase=Phase::Executing;
    auto interrupted=history;interrupted.commissionMail={sendRow};interrupted.unresolvedOperation=true;
    auto& intent=interrupted.commissionMail.front();intent.receipt.taskRevision=unsent.revision;
    intent.receipt.state=OperationState::Intent;intent.receipt.evidence.clear();intent.receipt.nativeReference.clear();intent.afterState="{}";
    claims.claims={money,item}; // Claim reader order is not intent order.
    assert(DecodeUnsentCommission(unsent,interrupted,claims,decoded,why));
    assert(PrepareUnsentCommission(unsent,owner.context,interrupted,claims,q,0,2002000,settlement,settled,why));
    assert(settled.task.phase==Phase::Verifying && settled.task.id==unsent.id && settled.task.checkpoint.step=="commission_mail_prepare");
    for(unsigned i=0;i<9;++i) {
        auto h=interrupted;auto c=claims;auto t=unsent;auto native=q;
        if(i==0)h.commissionMail.front().receipt.state=OperationState::Reconciling;
        if(i==1)h.commissionMail.front().afterState="{\"partial\":true}";
        if(i==2)h.commissionMail.push_back(receivedRow);
        if(i==3)c.claims.front().copper=31;
        if(i==4)t.context=owner.context;
        if(i==5)native.moneyBefore=99;
        if(i==6)h.unresolvedOperation=false;
        if(i==7)h.commissionMail.front().receipt.taskRevision--;
        if(i==8)c.claims.clear();
        assert(!PrepareUnsentCommission(t,owner.context,h,c,native,0,2002000,settlement,settled,why));
        assert(!why.empty());
    }
    auto uncertain=unsent;uncertain.phase=Phase::Reconciling;++uncertain.revision;
    auto capturedHistory=interrupted;capturedHistory.revision=uncertain.revision;
    auto& capture=capturedHistory.commissionMail.front();capture.receipt.state=OperationState::Reconciling;
    capture.receipt.evidence="native_save_capture_requires_reconciliation";capture.receipt.nativeReference=sendRow.receipt.nativeReference;
    capture.afterState="{\"mail\":9832,\"item\":103,\"receiver\":9,\"cod\":120,\"delivered_at\":1100,\"expires_at\":3000,\"postage\":30,\"customer_received\":false,\"fee_paid\":false}";
    AuctionMail captured;CommissionSendRecovery recovered;
    assert(DecodeInterruptedCommission(uncertain,capturedHistory,claims,decoded,captured,why) && captured.id==9832);
    assert(PrepareCapturedCommissionSend(uncertain,owner.context,capturedHistory,claims,2002000,settlement,recovered,why));
    assert(recovered.task.phase==Phase::Verifying && recovered.task.id==owner.id && recovered.claims.size()==2);
    for(const auto& c:recovered.claims)assert(c.after.state=="consumed");
    assert(PrepareUnsentCommission(uncertain,owner.context,capturedHistory,claims,q,0,2002000,settlement,settled,why));
    const auto preserved=SqlValue("{\"recovery\":\"atomic_send_absent\",\"unchanged\":"+EncodeCommissionMailQuote(q)+
        ",\"prior_observation\":"+capture.afterState+'}');
    assert(std::any_of(settled.plan.statements.begin(),settled.plan.statements.end(),[&](const std::string& sql){return sql.find(preserved)!=std::string::npos;}));
    for(unsigned i=0;i<5;++i) {
        auto bad=capturedHistory;
        if(i==0)bad.commissionMail.front().receipt.evidence="unknown_failure";
        if(i==1)bad.commissionMail.front().receipt.nativeReference="mail:9999:item:103";
        if(i==2)bad.commissionMail.front().afterState="{\"mail\":9832}";
        if(i==3)bad.commissionMail.front().receipt.taskRevision--;
        if(i==4)bad.commissionMail.push_back(receivedRow);
        assert(!PrepareCapturedCommissionSend(uncertain,owner.context,bad,claims,2002000,settlement,recovered,why));
    }
    capture.afterState="{}";capture.receipt.nativeReference.clear();
    assert(PrepareUnsentCommission(uncertain,owner.context,capturedHistory,claims,q,0,2002000,settlement,settled,why));
    assert(!PrepareCapturedCommissionSend(uncertain,owner.context,capturedHistory,claims,2002000,settlement,recovered,why));
    {
        // Two actual craft receipts may reserve parts of the same five-item
        // stack. The one postage claim and agreed fee apply to the order once.
        auto batchRecipe=recipe;batchRecipe.outputQuantity=5;
        auto batchJob=job;batchJob.agreement.recipe=EncodeProfessionJob(batchRecipe);batchJob.craft=batchJob.agreement.recipe;
        auto batchTask=owner;batchTask.checkpoint.data=EncodeCommissionJob(batchJob);
        auto batchQuote=q;batchQuote.quantity=batchQuote.count=5;
        auto first=item;first.quantity=2;
        auto second=item;second.id="d1879146-6e96-4e71-827d-6d12d965edb9";second.quantity=3;
        const std::vector<ClaimConsumption> batchUses={{first,2},{second,3},{money,30}};
        assert(ExactCommissionMailConsumption(batchTask,batchQuote,batchUses));
        for(unsigned i=0;i<5;++i) {
            auto bad=batchUses;
            if(i==0)bad[1].before.id=first.id;
            if(i==1)bad[1].before.itemGuid++;
            if(i==2)bad[1].used=2;
            if(i==3){bad[1].before.quantity=4;bad[1].used=4;}
            if(i==4)bad.pop_back();
            assert(!ExactCommissionMailConsumption(batchTask,batchQuote,bad));
        }
        auto batchSent=sent;batchSent.quantity=5;
        assert(VerifyCommissionMailSent(batchQuote,batchSent,operation));
        auto batchHistory=history;
        auto& batchSend=batchHistory.commissionMail.front();
        batchSend.beforeState="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(batchQuote),batchUses,8192)+'}';
        const std::string native="{\"mail\":9832,\"item\":103,\"receiver\":9,\"cod\":120,\"delivered_at\":1100,\"expires_at\":3000,\"postage\":30,\"customer_received\":false,\"fee_paid\":false}";
        batchSend.afterState=ClaimedNativeState(native,batchUses,8192);
        auto batchReceived=received;batchReceived.quantity=5;batchReceived.inventoryAfter=batchReceived.inventoryBefore+5;
        batchHistory.commissionMail[1]=eventRow(batchReceived,"commission_customer_received");
        assert(!CommissionMailObservationWrite(batchReceived).empty());
        assert(InspectCommissionDelivery(batchTask,batchHistory,delivered,why) && delivered.state==CommissionDeliveryState::Complete);
        auto batchReturn=returned;batchReturn.quantity=batchReturn.generated.quantity=5;
        assert(!CommissionMailObservationWrite(batchReturn).empty());
        batchHistory.commissionMail={batchSend,eventRow(batchReturn,"commission_parcel_returned")};
        ResourceClaim batchReturned;
        assert(ReturnedCommissionClaim(batchTask,batchHistory,batchReturned,why) && batchReturned.quantity==5);
        auto batchInterrupted=batchTask;batchInterrupted.phase=Phase::Reconciling;++batchInterrupted.revision;
        batchInterrupted.context.boot.clear();batchInterrupted.context.actorGeneration=batchInterrupted.context.mapGeneration=0;
        batchHistory.revision=batchInterrupted.revision;batchHistory.unresolvedOperation=true;batchHistory.commissionMail.resize(1);
        auto& partial=batchHistory.commissionMail.front();partial.receipt.taskRevision=batchTask.revision;
        partial.receipt.state=OperationState::Reconciling;partial.receipt.evidence="native_save_capture_requires_reconciliation";
        partial.afterState=native;
        UnsettledClaimBatch batchClaims;batchClaims.complete=true;batchClaims.bookRevision=1;batchClaims.claims={money,second,first};
        assert(PrepareCapturedCommissionSend(batchInterrupted,batchTask.context,batchHistory,batchClaims,2002000,settlement,recovered,why));
        assert(recovered.claims.size()==3);
        for(const auto& c:recovered.claims)assert(c.after.state=="consumed");
        // Keep the ordered GUID; only surplus is split off. Historical exact
        // quotes still round-trip without any optional split fields.
        auto split=batchQuote;split.count=8;split.splitPosition=uint16_t(255u<<8|24u);
        assert(ValidCommissionMailQuote(split));
        assert(DecodeCommissionMailQuote(EncodeCommissionMailQuote(split),decoded) && decoded.count==8);
        assert(ExactCommissionMailConsumption(batchTask,split,batchUses));
        assert(CommissionMailSentProof(batchTask,split,batchSent,operation).empty());
        assert(CommissionMailSentProof(batchTask,split,batchSent,operation,split.item).empty());
        assert(CommissionMailSentProof(batchTask,split,batchSent,operation,104).find("s.count=3")!=std::string::npos);
        for(unsigned i=0;i<5;++i) {
            auto bad=split;
            if(i==0)bad.splitPosition=0;
            if(i==1)bad.splitPosition=bad.position;
            if(i==2)bad.splitBagGuid=88;
            if(i==3)bad.count=5;
            if(i==4)bad.count=10001;
            assert(!ValidCommissionMailQuote(bad));
        }
        partial.beforeState="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(split),batchUses,8192)+'}';
        partial.afterState=native.substr(0,native.size()-1)+",\"surplus_item\":104,\"surplus_count\":3}";
        assert(PrepareCapturedCommissionSend(batchInterrupted,batchTask.context,batchHistory,batchClaims,2002000,settlement,recovered,why));
        partial.receipt.evidence="native_consumption_delta_mismatch";
        assert(PrepareCapturedCommissionSend(batchInterrupted,batchTask.context,batchHistory,batchClaims,2002000,settlement,recovered,why));
        assert(recovered.plan.statements[1].find("s.count=3")!=std::string::npos);
        partial.afterState=native;
        assert(!PrepareCapturedCommissionSend(batchInterrupted,batchTask.context,batchHistory,batchClaims,2002000,settlement,recovered,why));
        partial.afterState="{\"mail_not_sent\":true,\"surplus_item\":104,\"surplus_count\":3}";
        partial.receipt.nativeReference="item:103";partial.receipt.evidence="commission_mail_native_split_only";
        auto splitNative=split;splitNative.count=5;
        assert(PrepareUnsentCommission(batchInterrupted,batchTask.context,batchHistory,batchClaims,splitNative,0,2002000,settlement,settled,why));
        assert(settled.task.phase==Phase::Verifying && settled.plan.statements[1].find("i.count=3")!=std::string::npos);
        assert(!PrepareCapturedCommissionSend(batchInterrupted,batchTask.context,batchHistory,batchClaims,2002000,settlement,recovered,why));
        // Rolled-back split: original stack is intact and the selected slot
        // must be empty. Both cases keep the order claims, never consume them.
        assert(PrepareUnsentCommission(batchInterrupted,batchTask.context,batchHistory,batchClaims,split,0,2002000,settlement,settled,why));
        assert(settled.plan.statements[1].find("AND slot=24")!=std::string::npos);
        partial.afterState="{\"mail_not_sent\":true,\"surplus_item\":104,\"surplus_count\":4}";
        assert(!PrepareUnsentCommission(batchInterrupted,batchTask.context,batchHistory,batchClaims,splitNative,0,2002000,settlement,settled,why));
    }
}
