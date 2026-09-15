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
}
