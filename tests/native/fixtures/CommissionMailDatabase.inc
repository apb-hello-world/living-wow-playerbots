// Supplied rows exercise the actual custody predicate against MariaDB. This is
// database-boundary proof, not a native send, customer receipt or payment.
static void CommissionMailDatabase(Connection& db) {
    Task task;task.id=task.root="d1ca4b79-94ed-49fb-8c06-f54fa826c131";
    task.actor=9981;task.revision=7;
    CommissionMailQuote quote{"lwc-987",9981,9982,9981001,2454,1,1,500,30,120,120,23,123456789};
    const std::string operation="2f5efaed-b5d7-4f02-8743-1af0ea9e1604";
    AuctionMail sent;sent.id=90031;sent.sender=quote.sender;sent.receiver=quote.receiver;
    sent.cod=120;sent.itemGuid=quote.item;sent.itemEntry=quote.entry;sent.quantity=1;sent.attachments=1;
    sent.deliveredAt=1120;sent.expiresAt=200000;sent.subject=CommissionMailSubject(operation);
    assert(db.Execute("ALTER TABLE mail ADD COLUMN subject VARCHAR(255) NOT NULL DEFAULT ''"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject) VALUES(90031,0,9981,9982,0,120,1120,200000,"+SqlValue(sent.subject)+')'));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(9981,5,470),(9982,5,1000)"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90031,9981001,2454,9982)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981001,2454,9982,1)"));
    const auto proof=CommissionMailSentProof(task,quote,sent,operation);
    assert(db.Scalar(proof)==task.id);
    const std::vector<std::string> mismatches={
        "UPDATE mail SET receiver=9983 WHERE id=90031",
        "UPDATE mail SET sender=9983 WHERE id=90031",
        "UPDATE mail SET cod=0 WHERE id=90031",
        "UPDATE mail SET money=120 WHERE id=90031",
        "UPDATE mail SET messageType=2 WHERE id=90031",
        "UPDATE mail SET subject='unrelated parcel' WHERE id=90031",
        "UPDATE mail SET deliver_time=1121 WHERE id=90031",
        "UPDATE mail SET expire_time=200001 WHERE id=90031",
        "UPDATE mail_items SET receiver=9983 WHERE mail_id=90031",
        "UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001",
        "UPDATE item_instance SET count=2 WHERE guid=9981001",
        "UPDATE item_instance SET itemEntry=2455 WHERE guid=9981001",
        "UPDATE characters SET money=500 WHERE guid=9981",
        "INSERT INTO mail_items VALUES(90031,9981002,2454,9982)",
        "INSERT INTO mail_items VALUES(90032,9981001,2454,9982)",
        "INSERT INTO guild_bank_item VALUES(31,9981001,2454)",
        "INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9982,0,23,9981001,2454)"
    };
    for(const auto& mutation:mismatches) {
        assert(db.Execute("START TRANSACTION"));assert(db.Execute(mutation));
        assert(db.Scalar(proof).empty());assert(db.Execute("ROLLBACK"));
        assert(db.Scalar(proof)==task.id);
    }
    // Mailing never spends the customer's fee or credits it to the crafter.
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="1000");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="470");

    // Separate supplied journal/native records exercise post-send SQL. The
    // native mailbox handler itself still needs the copied-realm gameplay gate.
    task.revision=1;task.source="commission_job";task.sourceKey=quote.commission;task.kind=Kind::Commission;
    task.mode=Mode::Active;task.phase=Phase::Queued;task.createdAtMs=task.updatedAtMs=1000;
    task.context.actor=task.actor;task.checkpoint.step="commission_mail_prepare";
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=1;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={quote.commission,"wow-tx-receipt-fixture","mail",EncodeProfessionJob(recipe),9981,9982,120,1000};
    job.craft=job.agreement.recipe;task.checkpoint.data=EncodeCommissionJob(job);
    assert(db.Write(TaskWrite(task,0,"769b8ae6-3123-4587-8ad7-fb51b9c01905","fixture_created")));
    ResourceClaim parcel;parcel.id="d1879146-6e96-4e71-827d-6d12d965edb7";parcel.task=task.id;parcel.actor=9981;
    parcel.itemGuid=9981001;parcel.itemEntry=2454;parcel.quantity=1;parcel.state="held";parcel.location="bags";
    ResourceClaim postage;postage.id="f90f9bb6-58c8-4d82-a3be-a25d3fec47b7";postage.task=task.id;postage.actor=9981;
    postage.copper=30;postage.state="held";postage.location="money";
    const std::vector<ClaimConsumption> uses={{parcel,1},{postage,30}};
    const auto before="{\"effects\":"+std::to_string(Mask(Effect::Inventory)|Mask(Effect::Money))+",\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(quote),uses,8192)+'}';
    const auto sentAfter=ClaimedNativeState("{\"mail\":90031,\"item\":9981001,\"receiver\":9982,\"cod\":120,\"delivered_at\":1120,\"expires_at\":200000,\"postage\":30,\"customer_received\":false,\"fee_paid\":false}",uses,8192);
    assert(db.Execute("INSERT INTO living_activity_operation VALUES("+SqlValue(operation)+','+SqlValue(task.id)+
        ",1,'commission_mail_send',REPEAT('a',64),'verified','mail:90031:item:9981001',"+SqlValue(before)+
        ','+SqlValue(sentAfter)+",'native_commission_parcel_and_postage_observed',1000,1000)"));
    assert(db.Execute("ALTER TABLE mail ADD COLUMN checked INT UNSIGNED NOT NULL DEFAULT 0"));
    CommissionMailObservation received;received.sendOperation=operation;received.mail=90031;
    received.sender=9981;received.receiver=9982;received.item=9981001;received.entry=2454;received.quantity=1;
    received.copper=120;received.moneyBefore=1000;received.moneyAfter=880;
    received.inventoryBefore=0;received.inventoryAfter=1;received.atMs=2000000;
    auto& payment=received.generated;payment.id=90032;payment.sender=9982;payment.receiver=9981;payment.money=120;
    payment.subject=sent.subject;payment.deliveredAt=2000;payment.expiresAt=200000;
    const auto receiveId=CommissionMailReceiptId(operation,CommissionMailEvent::CustomerReceived);
    const auto receiptCount=[&](const std::string& id){return db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE operation_id="+SqlValue(id));};
    const auto collection=[&](){
        assert(db.Execute("UPDATE mail SET cod=0 WHERE id=90031"));
        assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90031"));
        assert(db.Execute("UPDATE characters SET money=880 WHERE guid=9982"));
        assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9982,0,23,9981001,2454)"));
        assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90032,0,9982,9981,120,0,2000,200000,"+SqlValue(sent.subject)+",8)"));
    };
    const auto receiptSql=CommissionMailObservationWrite(received);
    assert(!receiptSql.empty());
    if(!db.Execute(receiptSql))throw std::runtime_error(mysql_error(db.db));
    assert(receiptCount(receiveId)=="0");
    for(const auto& mutation:std::vector<std::string>{
        "UPDATE mail SET checked=0 WHERE id=90032",
        "UPDATE mail SET money=119 WHERE id=90032",
        "UPDATE mail SET receiver=9983 WHERE id=90032",
        "UPDATE mail SET subject='copied subject' WHERE id=90032",
        "UPDATE characters SET money=1000 WHERE guid=9982",
        "UPDATE living_activity_operation SET state='intent' WHERE operation_id="+SqlValue(operation),
        "UPDATE living_activity_operation SET native_reference='mail:99999:item:9981001' WHERE operation_id="+SqlValue(operation),
        "INSERT INTO mail_items VALUES(90031,9981001,2454,9982)"
    }) {
        assert(db.Execute("START TRANSACTION"));collection();assert(db.Execute(mutation));
        assert(db.Execute(receiptSql));assert(receiptCount(receiveId)=="0");assert(db.Execute("ROLLBACK"));
    }
    // Crash/rollback before commit loses both the effect and receipt together.
    assert(db.Execute("START TRANSACTION"));collection();assert(db.Execute(receiptSql));
    assert(receiptCount(receiveId)=="1");assert(db.Execute("ROLLBACK"));assert(receiptCount(receiveId)=="0");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="1000");

    // Stopping admission/cancelling after dispatch cannot erase a subsequent
    // native customer collection. It does not reopen or complete that task.
    assert(db.Execute("START TRANSACTION"));collection();
    assert(db.Execute("UPDATE living_activity_task SET phase='cancelled' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute(receiptSql));assert(receiptCount(receiveId)=="1");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="cancelled");
    assert(db.Execute("ROLLBACK"));

    CommissionMailObservation returned;returned.event=CommissionMailEvent::ParcelReturned;returned.sendOperation=operation;
    returned.mail=90033;returned.sender=9982;returned.receiver=9981;returned.item=9981001;returned.entry=2454;
    returned.quantity=1;returned.atMs=2000000;returned.generated=sent;returned.generated.id=90033;
    returned.generated.sender=9982;returned.generated.receiver=9981;returned.generated.cod=0;
    const auto returnSql=CommissionMailObservationWrite(returned);
    const auto returnId=CommissionMailReceiptId(operation,CommissionMailEvent::ParcelReturned);
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("DELETE FROM mail WHERE id=90031"));assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90031"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90033,0,9982,9981,0,0,1120,200000,"+SqlValue(sent.subject)+",2)"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90033,9981001,2454,9981)"));
    assert(db.Execute(returnSql));assert(receiptCount(returnId)=="1");assert(receiptCount(receiveId)=="0");
    assert(db.Execute("ROLLBACK"));assert(receiptCount(returnId)=="0");

    assert(db.Execute("START TRANSACTION"));collection();assert(db.Execute(receiptSql));assert(db.Execute("COMMIT"));
    const auto first=db.Scalar("SELECT after_state FROM living_activity_operation WHERE operation_id="+SqlValue(receiveId));
    auto duplicate=received;duplicate.atMs+=100;assert(db.Execute(CommissionMailObservationWrite(duplicate)));
    assert(receiptCount(receiveId)=="1");
    assert(db.Scalar("SELECT after_state FROM living_activity_operation WHERE operation_id="+SqlValue(receiveId))==first);
    // A new process/connection can read the durable receipt; deleting the now
    // empty customer envelope does not erase receipt/payment eligibility.
    assert(db.Execute("DELETE FROM mail WHERE id=90031"));
    {Connection restarted;assert(restarted.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE operation_id="+SqlValue(receiveId))=="1");}
    CommissionMailObservation fee;fee.event=CommissionMailEvent::FeeCollected;fee.sendOperation=operation;
    fee.mail=90032;fee.sender=9982;fee.receiver=9981;fee.copper=120;fee.moneyBefore=470;fee.moneyAfter=590;fee.atMs=2001000;
    const auto feeSql=CommissionMailObservationWrite(fee);
    const auto feeId=CommissionMailReceiptId(operation,CommissionMailEvent::FeeCollected);
    assert(db.Execute(feeSql));assert(receiptCount(feeId)=="0");
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("UPDATE mail SET money=0 WHERE id=90032"));assert(db.Execute("UPDATE characters SET money=590 WHERE guid=9981"));
    assert(db.Execute(feeSql));assert(receiptCount(feeId)=="1");assert(db.Execute("COMMIT"));
    assert(db.Execute("DELETE FROM mail WHERE id=90032"));
    {Connection restarted;assert(restarted.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE operation_id="+SqlValue(feeId))=="1");}
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="590");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="880");

    // Settlement consumes the actual acknowledged journal query, not caller
    // booleans or current inventory (the envelopes above are already gone).
    job.craftFinishedRevision=6;task.checkpoint.data=EncodeCommissionJob(job);task.revision=9;
    task.phase=Phase::Verifying;task.checkpoint.step="commission_mail_send";
    task.context.boot="769b8ae6-3123-4587-8ad7-fb51b9c01906";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    assert(db.Execute("UPDATE living_activity_task SET revision=9,phase='verifying',step='commission_mail_send',checkpoint="+
        SqlValue(task.checkpoint.data)+" WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=8 WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=7 WHERE operation_id="+SqlValue(operation)));
    assert(db.Execute("INSERT INTO living_activity_transition(transition_id,task_id,task_revision,actor_guid,phase,code,request_hash,occurred_at_ms) VALUES"
        "('769b8ae6-3123-4587-8ad7-fb51b9c01907',"+SqlValue(task.id)+",6,9981,'preparing','commission_craft_verified',REPEAT('c',64),1000)"));
    assert(db.Execute("CREATE TABLE organic_economy_commission(commission_id VARCHAR(36) PRIMARY KEY,bot_guid INT,player_guid INT,"
        "state VARCHAR(32),authoritative_payload TEXT,failure_reason VARCHAR(255))"));
    assert(db.Execute("INSERT INTO organic_economy_commission VALUES('lwc-987',9981,9982,'traveling',"+
        SqlValue(EncodeCommissionContract(job.agreement))+",'')"));
    const auto readHistory=[&](const Task& owner) {
        ProfessionHistoryCursor cursor;std::string why;
        assert(cursor.Begin(owner,db.History(owner),why));
        while(!cursor.Result().complete)assert(cursor.Advance(why));
        return cursor.Result();
    };
    auto history=readHistory(task);assert(history.commissionMail.size()==3 && !history.unresolvedOperation);
    CommissionDeliveryProof delivery;std::string why;
    assert(InspectCommissionDelivery(task,history,delivery,why) && delivery.state==CommissionDeliveryState::Complete);
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;
    ProfessionPreparation settlement;const std::string settledId="769b8ae6-3123-4587-8ad7-fb51b9c01908";
    assert(PrepareCommissionSettlement(task,task.context,history,claims,2002000,settledId,settlement,why));
    assert(settlement.task.phase==Phase::Completed && ValidateCommissionTask(settlement.task,why));
    // A changed receipt between read and CAS cannot finalize or project legacy completion.
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='changed' WHERE operation_id="+SqlValue(feeId)));
    assert(!db.Write(settlement.plan));
    assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="traveling");
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_mail_transaction_observed' WHERE operation_id="+SqlValue(feeId)));
    // Waiting snapshots do not call a timer completion. They keep the same ID.
    for(bool customerOnly:{true,false}) {
        assert(db.Execute("START TRANSACTION"));
        assert(db.Execute("DELETE FROM living_activity_operation WHERE operation_id="+SqlValue(feeId)));
        if(!customerOnly)assert(db.Execute("DELETE FROM living_activity_operation WHERE operation_id="+SqlValue(receiveId)));
        const auto pending=readHistory(task);
        assert(InspectCommissionDelivery(task,pending,delivery,why));
        assert(delivery.state==(customerOnly?CommissionDeliveryState::WaitingFee:CommissionDeliveryState::WaitingCustomer));
        ProfessionPreparation wait;
        assert(PrepareCommissionSettlement(task,task.context,pending,claims,2002000,settledId,wait,why));
        assert(wait.task.id==task.id && wait.task.phase==Phase::WaitingExternal && wait.task.retryAtMs==2062000);
        assert(db.Execute("ROLLBACK"));
    }
    auto restored=task;restored.context.boot.clear();restored.context.actorGeneration=restored.context.mapGeneration=0;
    assert(PrepareCommissionSettlement(restored,task.context,history,claims,2002000,settledId,settlement,why));
    assert(db.Write(settlement.plan));assert(db.Write(settlement.plan));
    assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="completed");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="590");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="880");

    // Separate supplied crash-before-commit state. No mail handler is invoked.
    auto unsent=restored;unsent.phase=Phase::Executing;unsent.revision=11;
    assert(db.Execute("UPDATE living_activity_task SET revision=11,phase='executing',step='commission_mail_send' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("DELETE FROM living_activity_operation WHERE operation_id IN ("+SqlValue(receiveId)+','+SqlValue(feeId)+')'));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=11,state='intent',native_reference='',evidence_code='',after_state='{}' WHERE operation_id="+SqlValue(operation)));
    assert(db.Execute("UPDATE organic_economy_commission SET state='traveling' WHERE commission_id='lwc-987'"));
    assert(db.Execute("UPDATE characters SET money=500 WHERE guid=9981"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001"));
    assert(db.Execute("UPDATE character_inventory SET guid=9981,bag=0,slot=23 WHERE item=9981001"));
    for(const auto& c:{parcel,postage})assert(db.Execute(
        "INSERT INTO living_activity_claim(claim_id,task_id,actor_guid,item_guid,item_entry,quantity,copper,location,state,revision,updated_at_ms) VALUES("+
        SqlValue(c.id)+','+SqlValue(c.task)+",9981,"+std::to_string(c.itemGuid)+','+std::to_string(c.itemEntry)+','+
        std::to_string(c.quantity)+','+std::to_string(c.copper)+','+SqlValue(c.location)+",'held',1,1000)"));
    claims.claims={postage,parcel};auto pendingSend=readHistory(unsent);CommissionMailQuote decoded;
    assert(DecodeUnsentCommission(unsent,pendingSend,claims,decoded,why));
    const std::string recoveryId="769b8ae6-3123-4587-8ad7-fb51b9c01909";
    ProfessionPreparation recovery;
    assert(PrepareUnsentCommission(unsent,task.context,pendingSend,claims,quote,0,2003000,recoveryId,recovery,why));
    const std::vector<std::pair<std::string,std::string>> altered={
        {"UPDATE characters SET money=470 WHERE guid=9981","UPDATE characters SET money=500 WHERE guid=9981"},
        {"UPDATE item_instance SET owner_guid=9982 WHERE guid=9981001","UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001"},
        {"UPDATE living_activity_claim SET quantity=2 WHERE claim_id="+SqlValue(parcel.id),"UPDATE living_activity_claim SET quantity=1 WHERE claim_id="+SqlValue(parcel.id)},
        {"INSERT INTO mail(id,subject) VALUES(90039,"+SqlValue(sent.subject)+')',"DELETE FROM mail WHERE id=90039"},
        {"UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(operation),"UPDATE living_activity_operation SET state='intent' WHERE operation_id="+SqlValue(operation)},
        {"UPDATE organic_economy_commission SET state='cancelled' WHERE commission_id='lwc-987'","UPDATE organic_economy_commission SET state='traveling' WHERE commission_id='lwc-987'"}
    };
    for(const auto& change:altered) {
        assert(db.Execute(change.first));assert(!db.Write(recovery.plan));assert(db.Execute(change.second));
        assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(operation))=="intent");
    }
    assert(db.Write(recovery.plan));assert(db.Write(recovery.plan));
    assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(operation))=="rejected");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="verifying");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND state='held'")=="2");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="500");
    assert(db.Scalar("SELECT guid FROM character_inventory WHERE item=9981001")=="9981");

    // A supplied later return creates a collection reservation, never bag stock
    // or delivery completion. Exercise the existing reservation SQL unchanged.
    auto awaiting=task;awaiting.revision=15;awaiting.phase=Phase::Preparing;awaiting.checkpoint.step="commission_return_collect";
    assert(db.Execute("UPDATE living_activity_task SET revision=15,phase='preparing',step='commission_return_collect' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=13,state='verified',native_reference='mail:90031:item:9981001',"
        "evidence_code='native_commission_parcel_and_postage_observed',after_state="+SqlValue(sentAfter)+" WHERE operation_id="+SqlValue(operation)));
    assert(db.Execute("UPDATE living_activity_claim SET state='consumed' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("DELETE FROM character_inventory WHERE item=9981001"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90033,0,9982,9981,0,0,1120,200000,"+SqlValue(sent.subject)+",2)"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90033,9981001,2454,9981)"));
    assert(db.Execute(returnSql));assert(receiptCount(returnId)=="1");
    const auto returnedRows=readHistory(awaiting);ResourceClaim returnedClaim;
    assert(ReturnedCommissionClaim(awaiting,returnedRows,returnedClaim,why));
    auto reserved=awaiting;++reserved.revision;reserved.updatedAtMs=2004000;
    NativeResourceBalance returnedBalance{9981,9981001,2454,1,0,"mail",90033};
    auto reservation=ResourceReservationWrite(reserved,15,"769b8ae6-3123-4587-8ad7-fb51b9c01910",{{returnedClaim,0}},{returnedBalance});
    reservation.statements.at(1)+=ReturnedCommissionClaimGuard(awaiting,returnedRows,returnedClaim);
    assert(db.Execute("UPDATE mail SET receiver=9982 WHERE id=90033"));assert(!db.Write(reservation));
    assert(db.Execute("UPDATE mail SET receiver=9981 WHERE id=90033"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9982 WHERE guid=9981001"));assert(!db.Write(reservation));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001"));
    assert(db.Write(reservation));assert(db.Write(reservation));
    assert(db.Scalar("SELECT location FROM living_activity_claim WHERE claim_id="+SqlValue(returnedClaim.id))=="mail");
    assert(db.Scalar("SELECT COUNT(*) FROM character_inventory WHERE item=9981001")=="0");
    assert(db.Scalar("SELECT COUNT(*) FROM mail_items WHERE item_guid=9981001 AND mail_id=90033")=="1");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="500");

    // Supplied native post-collection rows, including a whole-stack merge.
    // Bag presence without the exact collection receipt must not close work.
    auto travelReturn=reserved;travelReturn.phase=Phase::Traveling;travelReturn.context.boot.clear();
    travelReturn.context.actorGeneration=travelReturn.context.mapGeneration=0;
    assert(db.Execute("UPDATE living_activity_task SET phase='traveling' WHERE task_id="+SqlValue(task.id)));
    UnsettledClaimBatch resumeClaims;resumeClaims.complete=true;resumeClaims.bookRevision=2;resumeClaims.claims={returnedClaim};
    ProfessionPreparation resumed;
    assert(PrepareCommissionReturnResume(travelReturn,reserved.context,readHistory(travelReturn),resumeClaims,{},2004500,
        "769b8ae6-3123-4587-8ad7-fb51b9c01913",resumed,why));
    assert(db.Execute("UPDATE mail SET receiver=9982 WHERE id=90033"));assert(!db.Write(resumed.plan));
    assert(db.Execute("UPDATE mail SET receiver=9981 WHERE id=90033"));
    assert(db.Write(resumed.plan));assert(db.Write(resumed.plan));
    assert(db.Scalar("SELECT location FROM living_activity_claim WHERE claim_id="+SqlValue(returnedClaim.id))=="mail");
    assert(db.Scalar("SELECT COUNT(*) FROM mail_items WHERE mail_id=90033")=="1");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="500");
    auto collected=reserved;collected.revision=19;collected.phase=Phase::Verifying;
    collected.checkpoint.step="profession_mail_collect";collected.updatedAtMs=2005000;
    assert(db.Execute("UPDATE living_activity_task SET revision=19,phase='verifying',step='profession_mail_collect' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90033"));
    assert(db.Execute("DELETE FROM item_instance WHERE guid=9981001"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981002,2454,9981,4)"));
    assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,23,9981002,2454)"));
    auto heldReturn=returnedClaim;heldReturn.itemGuid=9981002;heldReturn.location="bags";heldReturn.nativeReference=0;heldReturn.revision=2;
    assert(db.Execute("UPDATE living_activity_claim SET item_guid=9981002,location='bags',native_reference=0,revision=2 WHERE claim_id="+SqlValue(heldReturn.id)));
    UnsettledClaimBatch returnedClaims;returnedClaims.complete=true;returnedClaims.bookRevision=3;returnedClaims.claims={heldReturn};
    const std::vector<NativeResourceBalance> returnedStock={{9981,9981002,2454,4,0,"bags"}};
    auto collectedHistory=readHistory(collected);CommissionReturnClosure closed;
    const std::string closureId="769b8ae6-3123-4587-8ad7-fb51b9c01911",collectionId="769b8ae6-3123-4587-8ad7-fb51b9c01912";
    auto restartedReturn=collected;restartedReturn.context.boot.clear();restartedReturn.context.actorGeneration=restartedReturn.context.mapGeneration=0;
    assert(PrepareCommissionReturnClosure(restartedReturn,collected.context,collectedHistory,returnedClaims,returnedStock,2006000,closureId,closed,why));
    assert(!db.Write(closed.plan));
    assert(db.Execute("INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,request_hash,state,before_state,after_state,evidence_code,native_reference,created_at_ms,updated_at_ms) VALUES("+
        SqlValue(collectionId)+','+SqlValue(task.id)+",18,'mail_collect',REPEAT('d',64),'verified',"
        "'{\"native\":{\"native\":{\"actor\":9981,\"mail\":90033,\"guid\":9981001,\"entry\":2454,\"quantity\":1}}}',"
        "'{\"native\":{\"mail\":90033,\"guid\":9981001,\"surviving_guid\":9981002}}',"
        "'native_mail_attachment_collected','mail:90033:item:9981001',2005000,2005000)"));
    const std::vector<std::pair<std::string,std::string>> invalidClosure={
        {"UPDATE item_instance SET owner_guid=9982 WHERE guid=9981002","UPDATE item_instance SET owner_guid=9981 WHERE guid=9981002"},
        {"UPDATE item_instance SET count=3 WHERE guid=9981002","UPDATE item_instance SET count=4 WHERE guid=9981002"},
        {"UPDATE living_activity_operation SET evidence_code='not_verified' WHERE operation_id="+SqlValue(collectionId),
         "UPDATE living_activity_operation SET evidence_code='native_mail_attachment_collected' WHERE operation_id="+SqlValue(collectionId)},
        {"UPDATE living_activity_operation SET native_reference='mail:90034:item:9981001' WHERE operation_id="+SqlValue(collectionId),
         "UPDATE living_activity_operation SET native_reference='mail:90033:item:9981001' WHERE operation_id="+SqlValue(collectionId)},
        {"UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(collectionId),
         "UPDATE living_activity_operation SET state='verified' WHERE operation_id="+SqlValue(collectionId)},
        {"UPDATE organic_economy_commission SET state='cancelled' WHERE commission_id='lwc-987'",
         "UPDATE organic_economy_commission SET state='traveling' WHERE commission_id='lwc-987'"},
        {"INSERT INTO mail_items VALUES(90033,9981001,2454,9981)","DELETE FROM mail_items WHERE mail_id=90033"}
    };
    for(const auto& mutation:invalidClosure) {
        assert(db.Execute(mutation.first));assert(!db.Write(closed.plan));assert(db.Execute(mutation.second));
        assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(heldReturn.id))=="held");
        assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="traveling");
    }
    assert(db.Write(closed.plan));assert(db.Write(closed.plan));
    assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="failed");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="failed");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(heldReturn.id))=="released");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND state NOT IN ('released','consumed')")=="0");
    assert(db.Scalar("SELECT count FROM item_instance WHERE guid=9981002")=="4");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="500");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="880");

    // Separate supplied capture-failure snapshots: native transaction either
    // rolled back or committed. Recovery itself must never transfer anything.
    const std::string captureAfter="{\"mail\":90041,\"item\":9981001,\"receiver\":9982,\"cod\":120,\"delivered_at\":1120,\"expires_at\":200000,\"postage\":30,\"customer_received\":false,\"fee_paid\":false}";
    for(bool committed:{false,true}) {
        auto interrupted=task;interrupted.phase=Phase::Reconciling;interrupted.revision=committed?26:22;
        interrupted.checkpoint.step="commission_mail_send";interrupted.context.boot.clear();
        interrupted.context.actorGeneration=interrupted.context.mapGeneration=0;
        assert(db.Execute("UPDATE living_activity_task SET revision="+std::to_string(interrupted.revision)+
            ",phase='reconciling',step='commission_mail_send' WHERE task_id="+SqlValue(task.id)));
        assert(db.Execute("DELETE FROM living_activity_operation WHERE task_id="+SqlValue(task.id)+" AND operation_id<>"+SqlValue(operation)));
        assert(db.Execute("UPDATE living_activity_operation SET task_revision="+std::to_string(interrupted.revision-1)+
            ",state='reconciling',evidence_code='native_save_capture_requires_reconciliation',native_reference='mail:90041:item:9981001',after_state="+
            SqlValue(captureAfter)+" WHERE operation_id="+SqlValue(operation)));
        assert(db.Execute("UPDATE organic_economy_commission SET state='traveling',failure_reason='' WHERE commission_id='lwc-987'"));
        for(const auto& c:{parcel,postage})assert(db.Execute("UPDATE living_activity_claim SET state='held',revision=1 WHERE claim_id="+SqlValue(c.id)));
        assert(db.Execute("DELETE FROM mail WHERE subject="+SqlValue(sent.subject)));
        assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981001,2454,9981,1) ON DUPLICATE KEY UPDATE owner_guid=9981,count=1"));
        assert(db.Execute("UPDATE characters SET money=500 WHERE guid=9981"));
        if(!committed)assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,24,9981001,2454)"));
        else {
            assert(db.Execute("DELETE FROM character_inventory WHERE item=9981001"));
            assert(db.Execute("UPDATE characters SET money=470 WHERE guid=9981"));
            assert(db.Execute("UPDATE item_instance SET owner_guid=9982 WHERE guid=9981001"));
            assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject) VALUES(90041,0,9981,9982,0,120,1120,200000,"+SqlValue(sent.subject)+')'));
            assert(db.Execute("INSERT INTO mail_items VALUES(90041,9981001,2454,9982)"));
        }
        auto read=readHistory(interrupted);claims.claims={parcel,postage};CommissionSendRecovery recovered;
        const auto recoveryReceipt=committed?"769b8ae6-3123-4587-8ad7-fb51b9c01915":"769b8ae6-3123-4587-8ad7-fb51b9c01914";
        WritePlan recoveryPlan;
        if(committed) {
            assert(PrepareCapturedCommissionSend(interrupted,task.context,read,claims,2008000,recoveryReceipt,recovered,why));
            recoveryPlan=recovered.plan;
            assert(db.Execute("UPDATE mail SET cod=121 WHERE id=90041"));assert(!db.Write(recoveryPlan));
            assert(db.Execute("UPDATE mail SET cod=120 WHERE id=90041"));
            assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981001"));assert(!db.Write(recoveryPlan));
            assert(db.Execute("UPDATE item_instance SET owner_guid=9982 WHERE guid=9981001"));
            assert(db.Execute("UPDATE characters SET money=469 WHERE guid=9981"));assert(!db.Write(recoveryPlan));
            assert(db.Execute("UPDATE characters SET money=470 WHERE guid=9981"));
            assert(db.Execute("INSERT INTO mail(id,subject) VALUES(90042,"+SqlValue(sent.subject)+')'));assert(!db.Write(recoveryPlan));
            assert(db.Execute("DELETE FROM mail WHERE id=90042"));
            assert(db.Execute("UPDATE living_activity_claim SET quantity=2 WHERE claim_id="+SqlValue(parcel.id)));assert(!db.Write(recoveryPlan));
            assert(db.Execute("UPDATE living_activity_claim SET quantity=1 WHERE claim_id="+SqlValue(parcel.id)));
        } else {
            assert(PrepareUnsentCommission(interrupted,task.context,read,claims,quote,0,2008000,recoveryReceipt,recovery,why));
            recoveryPlan=recovery.plan;
            assert(!db.Write(recoveryPlan)); // Original slot is 23, not 24.
            assert(db.Execute("UPDATE character_inventory SET slot=23 WHERE item=9981001"));
        }
        assert(db.Execute("UPDATE living_activity_operation SET evidence_code='changed' WHERE operation_id="+SqlValue(operation)));
        assert(!db.Write(recoveryPlan));
        assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_save_capture_requires_reconciliation' WHERE operation_id="+SqlValue(operation)));
        assert(db.Write(recoveryPlan));assert(db.Write(recoveryPlan));
        assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(operation))==(committed?"verified":"rejected"));
        assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(parcel.id))==(committed?"consumed":"held"));
        assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="traveling");
        assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")== (committed?"470":"500"));
        assert(db.Scalar("SELECT count FROM item_instance WHERE guid=9981001")=="1");
    }
    // Five-item order backed by two native craft claims on one shared stack.
    // Restore a captured send, then verify collection uses the full quantity
    // while charging the customer's agreed COD once, not once per claim.
    auto batchRecipe=recipe;batchRecipe.outputQuantity=5;
    auto batchJob=job;batchJob.agreement.recipe=EncodeProfessionJob(batchRecipe);batchJob.craft=batchJob.agreement.recipe;
    auto batchTask=task;batchTask.revision=30;batchTask.phase=Phase::Reconciling;batchTask.checkpoint.data=EncodeCommissionJob(batchJob);
    batchTask.context.boot.clear();batchTask.context.actorGeneration=batchTask.context.mapGeneration=0;
    auto batchQuote=quote;batchQuote.quantity=batchQuote.count=5;
    auto partA=parcel;partA.quantity=2;
    auto partB=parcel;partB.id="769b8ae6-3123-4587-8ad7-fb51b9c01921";partB.quantity=3;
    std::vector<ClaimConsumption> batchUses={{partA,2},{partB,3},{postage,30}};
    const auto batchBefore="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(batchQuote),batchUses,8192)+'}';
    assert(db.Execute("UPDATE living_activity_task SET revision=30,phase='reconciling',checkpoint="+SqlValue(batchTask.checkpoint.data)+" WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE organic_economy_commission SET authoritative_payload="+SqlValue(EncodeCommissionContract(batchJob.agreement))+" WHERE commission_id='lwc-987'"));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=29,state='reconciling',before_state="+SqlValue(batchBefore)+
        ",after_state="+SqlValue(captureAfter)+",evidence_code='native_save_capture_requires_reconciliation' WHERE operation_id="+SqlValue(operation)));
    assert(db.Execute("UPDATE living_activity_claim SET state='held',revision=1,quantity=2 WHERE claim_id="+SqlValue(partA.id)));
    assert(db.Execute("UPDATE living_activity_claim SET state='held',revision=1 WHERE claim_id="+SqlValue(postage.id)));
    assert(db.Execute("INSERT INTO living_activity_claim(claim_id,task_id,actor_guid,item_guid,item_entry,quantity,copper,location,state,revision,updated_at_ms) VALUES("+
        SqlValue(partB.id)+','+SqlValue(task.id)+",9981,9981001,2454,3,0,'bags','held',1,2009000)"));
    assert(db.Execute("UPDATE item_instance SET count=5 WHERE guid=9981001"));
    claims.claims={partB,postage,partA};auto batchHistory=readHistory(batchTask);CommissionSendRecovery batchRecovered;
    assert(PrepareCapturedCommissionSend(batchTask,task.context,batchHistory,claims,2010000,
        "769b8ae6-3123-4587-8ad7-fb51b9c01922",batchRecovered,why));
    assert(db.Execute("UPDATE item_instance SET count=4 WHERE guid=9981001"));assert(!db.Write(batchRecovered.plan));
    assert(db.Execute("UPDATE item_instance SET count=5 WHERE guid=9981001"));
    assert(db.Write(batchRecovered.plan));assert(db.Write(batchRecovered.plan));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(partA.id))=="consumed");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(partB.id))=="consumed");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="470");
    auto batchReceived=received;batchReceived.mail=90041;batchReceived.quantity=5;batchReceived.inventoryBefore=4;batchReceived.inventoryAfter=9;
    batchReceived.generated.id=90043;batchReceived.generated.subject=sent.subject;
    assert(db.Execute("UPDATE mail SET cod=0 WHERE id=90041"));
    assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90041"));
    assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9982,0,23,9981001,2454)"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90043,0,9982,9981,120,0,"+
        std::to_string(batchReceived.generated.deliveredAt)+','+std::to_string(batchReceived.generated.expiresAt)+','+SqlValue(sent.subject)+",8)"));
    auto shortReceipt=batchReceived;shortReceipt.quantity=4;shortReceipt.inventoryAfter=8;
    assert(db.Execute(CommissionMailObservationWrite(shortReceipt)));assert(receiptCount(receiveId)=="0");
    assert(db.Execute(CommissionMailObservationWrite(batchReceived)));assert(receiptCount(receiveId)=="1");
    auto collectedBatch=readHistory(batchRecovered.task);
    assert(InspectCommissionDelivery(batchRecovered.task,collectedBatch,delivery,why) && delivery.state==CommissionDeliveryState::WaitingFee);
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9982")=="880");
    assert(db.Scalar("SELECT count FROM item_instance WHERE guid=9981001")=="5");
    // Native SplitItem preserves the ordered GUID and creates a separate
    // owned surplus stack. Rehearse all transaction boundaries in MariaDB.
    for(unsigned scenario=0;scenario<3;++scenario) {
        auto splitTask=batchTask;splitTask.revision=34+scenario*4;
        auto splitQuote=batchQuote;splitQuote.count=8;splitQuote.splitPosition=uint16_t(255u<<8|24u);
        const auto splitBefore="{\"effects\":12,\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(splitQuote),batchUses,8192)+'}';
        const auto splitAfter=scenario==2 ? captureAfter.substr(0,captureAfter.size()-1)+",\"surplus_item\":9981003,\"surplus_count\":3}" :
            std::string("{\"mail_not_sent\":true,\"surplus_item\":9981003,\"surplus_count\":3}");
        assert(db.Execute("UPDATE living_activity_task SET revision="+std::to_string(splitTask.revision)+",phase='reconciling' WHERE task_id="+SqlValue(task.id)));
        assert(db.Execute("DELETE FROM living_activity_operation WHERE task_id="+SqlValue(task.id)+" AND operation_id<>"+SqlValue(operation)));
        assert(db.Execute("UPDATE living_activity_operation SET task_revision="+std::to_string(splitTask.revision-1)+
            ",state='reconciling',before_state="+SqlValue(splitBefore)+",after_state="+SqlValue(splitAfter)+
            ",evidence_code='native_save_capture_requires_reconciliation',native_reference="+
            SqlValue(scenario==2?"mail:90041:item:9981001":"item:9981001")+" WHERE operation_id="+SqlValue(operation)));
        for(const auto& c:{partA,partB,postage})assert(db.Execute("UPDATE living_activity_claim SET state='held',revision=1 WHERE claim_id="+SqlValue(c.id)));
        assert(db.Execute("DELETE FROM mail WHERE subject="+SqlValue(sent.subject)));
        assert(db.Execute("DELETE FROM mail_items WHERE item_guid IN (9981001,9981003)"));
        assert(db.Execute("DELETE FROM character_inventory WHERE item IN (9981001,9981003)"));
        assert(db.Execute("DELETE FROM item_instance WHERE guid=9981003"));
        assert(db.Execute("UPDATE item_instance SET owner_guid="+std::to_string(scenario==2?9982:9981)+",count="+
            std::to_string(scenario?5:8)+" WHERE guid=9981001"));
        assert(db.Execute("UPDATE characters SET money="+std::to_string(scenario==2?470:500)+" WHERE guid=9981"));
        if(scenario) {
            assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981003,2454,9981,3)"));
            assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,24,9981003,2454)"));
        }
        if(scenario==2) {
            assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject) VALUES(90041,0,9981,9982,0,120,1120,200000,"+SqlValue(sent.subject)+')'));
            assert(db.Execute("INSERT INTO mail_items VALUES(90041,9981001,2454,9982)"));
        } else assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,23,9981001,2454)"));
        claims.claims={partB,postage,partA};auto splitHistory=readHistory(splitTask);
        const auto receipt=std::string("769b8ae6-3123-4587-8ad7-fb51b9c0193")+char('0'+scenario);
        WritePlan plan;
        if(scenario==2) {
            assert(PrepareCapturedCommissionSend(splitTask,task.context,splitHistory,claims,2015000,receipt,batchRecovered,why));
            plan=batchRecovered.plan;
        } else {
            auto native=splitQuote;if(scenario==1)native.count=5;
            assert(PrepareUnsentCommission(splitTask,task.context,splitHistory,claims,native,0,2015000,receipt,recovery,why));
            plan=recovery.plan;
        }
        if(scenario) {
            assert(db.Execute("UPDATE item_instance SET count=4 WHERE guid=9981003"));assert(!db.Write(plan));
            assert(db.Execute("UPDATE item_instance SET count=3,owner_guid=9982 WHERE guid=9981003"));assert(!db.Write(plan));
            assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981003"));
            assert(db.Execute("UPDATE character_inventory SET slot=25 WHERE item=9981003"));assert(!db.Write(plan));
            assert(db.Execute("UPDATE character_inventory SET slot=24 WHERE item=9981003"));
        } else {
            assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,24,9981003,2454)"));
            assert(!db.Write(plan));assert(db.Execute("DELETE FROM character_inventory WHERE item=9981003"));
        }
        assert(db.Write(plan));assert(db.Write(plan));
        assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(partA.id))==(scenario==2?"consumed":"held"));
        assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(partB.id))==(scenario==2?"consumed":"held"));
        assert(db.Scalar("SELECT SUM(count) FROM item_instance WHERE guid IN (9981001,9981003)")=="8");
        assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")== (scenario==2?"470":"500"));
        assert(db.Scalar("SELECT state FROM organic_economy_commission WHERE commission_id='lwc-987'")=="traveling");
    }
    // Banking unrelated personal material to free a parcel slot must not leave
    // a permanent extra claim on the order. Release only after native proof.
    auto preparedTask=batchTask;preparedTask.context=task.context;preparedTask.revision=50;preparedTask.phase=Phase::Preparing;
    ResourceClaim auxiliary=partA;auxiliary.id="769b8ae6-3123-4587-8ad7-fb51b9c01940";
    auxiliary.itemGuid=9981004;auxiliary.itemEntry=765;auxiliary.quantity=7;auxiliary.location="bank";auxiliary.revision=2;
    assert(db.Execute("UPDATE living_activity_task SET revision=50,phase='preparing' WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("DELETE FROM living_activity_operation WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("DELETE FROM mail_items WHERE item_guid=9981001"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9981,count=8 WHERE guid=9981001"));
    assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,23,9981001,2454)"));
    assert(db.Execute("UPDATE characters SET money=500 WHERE guid=9981"));
    for(const auto& c:{partA,partB,postage})assert(db.Execute("UPDATE living_activity_claim SET state='held',revision=1 WHERE claim_id="+SqlValue(c.id)));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9981004,765,9981,7)"));
    assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9981,0,39,9981004,765)"));
    assert(db.Execute("INSERT INTO living_activity_claim(claim_id,task_id,actor_guid,item_guid,item_entry,quantity,copper,location,state,revision,updated_at_ms) VALUES("+
        SqlValue(auxiliary.id)+','+SqlValue(task.id)+",9981,9981004,765,7,0,'bank','held',2,2016000)"));
    claims.claims={partA,partB,postage,auxiliary};
    const std::vector<NativeResourceBalance> balances={{9981,9981001,2454,8,0,"bags"},{9981,0,0,0,500,"money"},{9981,9981004,765,7,0,"bank"}};
    CommissionReturnClosure prepared;
    assert(PrepareCommissionParcelClaims(preparedTask,task.context,claims,balances,2017000,
        "769b8ae6-3123-4587-8ad7-fb51b9c01941",prepared,why));
    assert(!db.Write(prepared.plan)); // The bank receipt is not optional.
    const auto bankBefore="{\"native\":{\"native\":{\"guid\":9981004,\"entry\":765,\"quantity\":7}}}";
    assert(db.Execute("INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,state,request_hash,before_state,after_state,evidence_code,native_reference,created_at_ms,updated_at_ms) VALUES("+
        SqlValue("769b8ae6-3123-4587-8ad7-fb51b9c01942")+','+SqlValue(task.id)+",49,'bank_deposit','verified',"+SqlValue(std::string(64,'d'))+','+
        SqlValue(bankBefore)+",'{}','native_bank_stack_deposited','item:9981004',2016000,2016001)"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9982 WHERE guid=9981004"));assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9981 WHERE guid=9981004"));
    assert(db.Execute("UPDATE living_activity_claim SET quantity=4 WHERE claim_id="+SqlValue(partB.id)));assert(!db.Write(prepared.plan));
    assert(db.Execute("UPDATE living_activity_claim SET quantity=3 WHERE claim_id="+SqlValue(partB.id)));
    assert(db.Write(prepared.plan));assert(db.Write(prepared.plan));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(auxiliary.id))=="released");
    for(const auto& c:{partA,partB,postage})assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(c.id))=="held");
    assert(db.Scalar("SELECT count FROM item_instance WHERE guid=9981001")=="8");
    assert(db.Scalar("SELECT count FROM item_instance WHERE guid=9981004")=="7");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9981")=="500");
}
