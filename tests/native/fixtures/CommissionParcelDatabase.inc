// Supplied database fixtures, not native gameplay proof. Exercise the actual
// multi-attachment transaction predicates and durable per-item receipts.
static void CommissionParcelDatabase(Connection& db) {
    Task task;task.id=task.root="cf067019-3045-50c8-9241-e32da633c845";
    task.actor=9984;task.revision=1;task.source="commission_job";task.sourceKey="lwc-988";
    task.kind=Kind::Commission;task.mode=Mode::Active;task.phase=Phase::Queued;
    task.createdAtMs=task.updatedAtMs=1000;task.context.actor=task.actor;
    task.checkpoint.step="commission_mail_prepare";
    CommissionMailQuote q{"lwc-988",9984,9985,9984001,2454,1,1,500,60,120,120,23,123456789};
    q.additional={{9984002,1,24}};
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=75;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=2;
    recipe.reagents={{765,2},{2449,2},{3371,2}};
    CommissionJob job;job.agreement={q.commission,"wow-tx-parcel-fixture","mail",EncodeProfessionJob(recipe),9984,9985,120,1000};
    job.craft=job.agreement.recipe;task.checkpoint.data=EncodeCommissionJob(job);
    assert(db.Write(TaskWrite(task,0,"5a3f56f2-9b28-5882-bb85-d95f4d952168","fixture_created")));
    const std::string send="8d5fd4b9-7954-5cbf-adf5-c2163f47c0c2";
    const auto subject=CommissionMailSubject(send);
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(9984,5,440),(9985,5,1000)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9984001,2454,9985,1),(9984002,2454,9985,1)"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject) VALUES(90141,0,9984,9985,0,120,1120,200000,"+SqlValue(subject)+')'));
    assert(db.Execute("INSERT INTO mail_items VALUES(90141,9984001,2454,9985),(90141,9984002,2454,9985)"));
    std::vector<ClaimConsumption> uses;
    for(uint32_t i=0;i<2;++i){ResourceClaim c;c.id=i?"8dd46e20-7004-526a-a623-9fbdad2a1189":"cd4d58f8-81f9-5ea3-89d2-a1839e7b216e";
        c.task=task.id;c.actor=9984;c.itemGuid=9984001+i;c.itemEntry=2454;c.quantity=1;c.state="held";c.location="bags";uses.push_back({c,1});}
    ResourceClaim postage;postage.id="062cc4f3-7948-5a64-86c1-c18fe663915b";postage.task=task.id;postage.actor=9984;
    postage.copper=60;postage.state="held";postage.location="money";uses.push_back({postage,60});
    const auto before="{\"effects\":"+std::to_string(Mask(Effect::Inventory)|Mask(Effect::Money))+",\"persistence\":1,\"native\":"+ClaimedNativeState(EncodeCommissionMailQuote(q),uses,8192)+'}';
    const auto after=ClaimedNativeState("{\"mail\":90141,\"item\":9984001,\"receiver\":9985,\"cod\":120,\"delivered_at\":1120,\"expires_at\":200000,\"postage\":60,\"customer_received\":false,\"fee_paid\":false,\"attachment_count\":2}",uses,8192);
    assert(db.Execute("INSERT INTO living_activity_operation VALUES("+SqlValue(send)+','+SqlValue(task.id)+
        ",1,'commission_mail_send',REPEAT('a',64),'verified','mail:90141:item:9984001',"+SqlValue(before)+','+SqlValue(after)+
        ",'native_commission_parcel_and_postage_observed',1000,1000)"));
    AuctionMail sent;sent.id=90141;sent.sender=q.sender;sent.receiver=q.receiver;sent.cod=q.cod;sent.attachments=2;
    sent.subject=subject;sent.deliveredAt=1120;sent.expiresAt=200000;
    assert(db.Scalar(CommissionMailSentProof(task,q,sent,send))==task.id);
    const auto count=[&](const std::string& id){return db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE operation_id="+SqlValue(id));};
    CommissionMailObservation first;first.sendOperation=send;first.mail=90141;first.sender=9984;first.receiver=9985;
    first.item=9984001;first.entry=2454;first.quantity=1;first.copper=120;first.moneyBefore=1000;first.moneyAfter=880;
    first.inventoryBefore=0;first.inventoryAfter=1;first.atMs=2000000;first.parcelQuote=EncodeCommissionMailQuote(q);
    first.attachmentsBefore=2;first.attachmentsAfter=1;
    first.generated.id=90142;first.generated.sender=9985;first.generated.receiver=9984;first.generated.money=120;
    first.generated.subject=subject;first.generated.deliveredAt=2000;first.generated.expiresAt=200000;
    const auto firstId=CommissionParcelReceiptId(send,first.item),firstSql=CommissionMailObservationWrite(first);
    const auto collectFirst=[&](){
        assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90141 AND item_guid=9984001"));
        assert(db.Execute("UPDATE mail SET cod=0 WHERE id=90141"));
        assert(db.Execute("UPDATE characters SET money=880 WHERE guid=9985"));
        assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9985,0,23,9984001,2454)"));
        assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90142,0,9985,9984,120,0,2000,200000,"+SqlValue(subject)+",8)"));
    };
    assert(!firstSql.empty());if(!db.Execute(firstSql))throw std::runtime_error(mysql_error(db.db));assert(count(firstId)=="0");
    for(const auto& mutation:std::vector<std::string>{
        "UPDATE item_instance SET count=2 WHERE guid=9984002",
        "UPDATE mail_items SET receiver=9984 WHERE item_guid=9984002",
        "INSERT INTO mail_items VALUES(90149,9984002,2454,9985)",
        "INSERT INTO guild_bank_item VALUES(31,9984002,2454)",
        "UPDATE mail SET money=119 WHERE id=90142",
        "UPDATE mail SET checked=0 WHERE id=90142",
        "UPDATE living_activity_operation SET state='intent' WHERE operation_id="+SqlValue(send)}){
        assert(db.Execute("START TRANSACTION"));collectFirst();assert(db.Execute(mutation));
        assert(db.Execute(firstSql));assert(count(firstId)=="0");assert(db.Execute("ROLLBACK"));
    }
    assert(db.Execute("START TRANSACTION"));collectFirst();assert(db.Execute(firstSql));assert(count(firstId)=="1");
    assert(db.Execute("ROLLBACK"));assert(count(firstId)=="0");
    assert(db.Execute("START TRANSACTION"));collectFirst();assert(db.Execute(firstSql));assert(db.Execute("COMMIT"));
    {Connection restarted;assert(restarted.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE operation_id="+SqlValue(firstId))=="1");}
    assert(db.Execute(firstSql));assert(count(firstId)=="1");
    // The sender may take the once-only fee while another attachment waits.
    CommissionMailObservation fee;fee.event=CommissionMailEvent::FeeCollected;fee.sendOperation=send;fee.parcelQuote=first.parcelQuote;
    fee.mail=90142;fee.sender=9985;fee.receiver=9984;fee.copper=120;fee.moneyBefore=440;fee.moneyAfter=560;fee.atMs=2001000;
    assert(db.Execute("UPDATE mail SET money=0 WHERE id=90142"));assert(db.Execute("UPDATE characters SET money=560 WHERE guid=9984"));
    assert(db.Execute(CommissionMailObservationWrite(fee)));assert(count(CommissionMailReceiptId(send,fee.event))=="1");
    CommissionMailObservation second=first;second.item=9984002;second.copper=0;second.moneyBefore=second.moneyAfter=880;
    second.inventoryBefore=1;second.inventoryAfter=2;second.generated={};second.attachmentsBefore=1;second.attachmentsAfter=0;
    const auto secondId=CommissionParcelReceiptId(send,second.item),secondSql=CommissionMailObservationWrite(second);
    assert(db.Execute(secondSql));assert(count(secondId)=="0");
    // Returning the uncollected remainder records exactly that remainder, not
    // a second copy of the item already taken. Roll back to test both endings.
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90141"));assert(db.Execute("DELETE FROM mail WHERE id=90141"));
    assert(db.Execute("INSERT INTO mail(id,messageType,sender,receiver,money,cod,deliver_time,expire_time,subject,checked) VALUES(90143,0,9985,9984,0,0,2000,200000,"+SqlValue(subject)+",2)"));
    assert(db.Execute("INSERT INTO mail_items VALUES(90143,9984002,2454,9984)"));
    assert(db.Execute("UPDATE item_instance SET owner_guid=9984 WHERE guid=9984002"));
    CommissionMailObservation returned;returned.event=CommissionMailEvent::ParcelReturned;returned.sendOperation=send;returned.parcelQuote=first.parcelQuote;
    returned.mail=90143;returned.sender=9985;returned.receiver=9984;returned.item=9984002;returned.entry=2454;returned.quantity=1;
    returned.atMs=2002000;returned.attachmentsBefore=returned.attachmentsAfter=1;
    returned.generated=sent;returned.generated.id=90143;returned.generated.sender=9985;returned.generated.receiver=9984;
    returned.generated.cod=0;returned.generated.attachments=1;returned.generated.itemGuid=9984002;returned.generated.itemEntry=2454;returned.generated.quantity=1;returned.generated.deliveredAt=2000;
    const auto returnSql=CommissionMailObservationWrite(returned),returnId=CommissionMailReceiptId(send,returned.event);
    assert(!returnSql.empty());if(!db.Execute(returnSql))throw std::runtime_error(mysql_error(db.db));assert(count(returnId)=="1");
    assert(db.Scalar("SELECT JSON_EXTRACT(after_state,'$.returned_items[0].item') FROM living_activity_operation WHERE operation_id="+SqlValue(returnId))=="9984002");
    assert(db.Execute("ROLLBACK"));
    assert(db.Execute("DELETE FROM mail_items WHERE mail_id=90141"));
    assert(db.Execute("INSERT INTO character_inventory(guid,bag,slot,item,item_template) VALUES(9985,0,24,9984002,2454)"));
    assert(db.Execute(secondSql));assert(count(secondId)=="1");assert(db.Execute(secondSql));assert(count(secondId)=="1");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9984")=="560");
    assert(db.Scalar("SELECT money FROM characters WHERE guid=9985")=="880");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+SqlValue(task.id)+" AND kind='commission_customer_received'")=="2");
    // Decode the real paginated query, then close through the same guarded
    // settlement used by the coordinator. Payment alone must not finish it.
    task.revision=9;task.phase=Phase::Verifying;task.checkpoint.step="commission_mail_send";
    job.craftFinishedRevision=6;task.checkpoint.data=EncodeCommissionJob(job);
    task.context.boot="4f116aae-9c15-5480-bb34-e4fd6d1d2a2c";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    assert(db.Execute("UPDATE living_activity_task SET revision=9,phase='verifying',step='commission_mail_send',checkpoint="+
        SqlValue(task.checkpoint.data)+" WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=8 WHERE task_id="+SqlValue(task.id)));
    assert(db.Execute("UPDATE living_activity_operation SET task_revision=7 WHERE operation_id="+SqlValue(send)));
    assert(db.Execute("INSERT INTO living_activity_transition(transition_id,task_id,task_revision,actor_guid,phase,code,request_hash,occurred_at_ms) VALUES"
        "('3d8fb904-60d3-5649-8a63-1cfcfce914f4',"+SqlValue(task.id)+",6,9984,'preparing','commission_craft_verified',REPEAT('c',64),1000)"));
    assert(db.Execute("INSERT INTO organic_economy_commission VALUES('lwc-988',9984,9985,'traveling',"+
        SqlValue(EncodeCommissionContract(job.agreement))+",'')"));
    ProfessionHistoryCursor cursor;std::string why;assert(cursor.Begin(task,db.History(task),why));
    while(!cursor.Result().complete)assert(cursor.Advance(why));
    auto history=cursor.Result();assert(history.commissionMail.size()==4 && !history.unresolvedOperation);
    CommissionDeliveryProof proof;assert(InspectCommissionDelivery(task,history,proof,why) && proof.state==CommissionDeliveryState::Complete);
    auto partial=history;partial.commissionMail.erase(std::remove_if(partial.commissionMail.begin(),partial.commissionMail.end(),
        [&](const auto& row){return row.receipt.id==secondId;}),partial.commissionMail.end());
    assert(InspectCommissionDelivery(task,partial,proof,why) && proof.state==CommissionDeliveryState::WaitingCustomer);
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=1;ProfessionPreparation closed;
    assert(PrepareCommissionSettlement(task,task.context,history,claims,2003000,"f4d35c03-c0f2-5658-83c3-3272a8ee9d52",closed,why));
    assert(db.Write(closed.plan));assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
}
