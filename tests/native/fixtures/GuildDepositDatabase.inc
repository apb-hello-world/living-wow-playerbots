// Uses the production SQL builders against separate fixture tables, not game
// inventory. Native bank execution is verified separately in the copied realm.
static void GuildDepositDatabase(Connection& db) {
    const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984798200")+std::to_string(n);};
    GuildDepositQuote q;q.job={12,31,798,2770,4,0,"guild_database_fixture",false};
    q.actor=798;q.item=798001;q.itemCount=8;q.amount=4;q.bankCount=2;q.bagCount=8;
    q.money=500;q.goalTarget=6;q.position=255*256+23;q.tab=0;q.bank=321;
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f98";
    task.actor=task.context.actor=q.actor;task.source="guild_delivery";task.sourceKey=GuildDeliverySourceKey(q.job,q.actor);
    task.kind=Kind::GuildDelivery;task.mode=Mode::Active;task.priority=Priority::Delivery;task.accepted=true;
    task.phase=Phase::Queued;task.createdAtMs=task.updatedAtMs=1000;task.context.boot=rid(9);
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.checkpoint.step="guild_delivery_prepare";task.checkpoint.data=EncodeGuildDeliveryJob(q.job);
    assert(db.Write(TaskWrite(task,0,rid(0),"fixture_created")));
    task.phase=Phase::Preparing;++task.revision;assert(db.Write(TaskWrite(task,1,rid(1),"fixture_preparing")));
    ResourceClaim item;item.id=rid(2);item.task=task.id;item.actor=q.actor;item.itemGuid=q.item;
    item.itemEntry=q.job.entry;item.quantity=4;item.location="bags";item.state="held";
    ++task.revision;assert(db.Write(ResourceReservationWrite(task,2,rid(3),{{item,0}},{{q.actor,q.item,q.job.entry,8,0,"bags"}})));
    OperationRequest intent;intent.kind="guild_bank_deposit";intent.effects=Mask(Effect::Inventory)|Mask(Effect::Guild);
    intent.persistence=NativePersistence::Inventory;intent.beforeState=EncodeGuildDepositQuote(q);intent.consumption={{item,4}};
    intent.transition.task=task;intent.transition.expectedRevision=3;++intent.transition.task.revision;
    intent.transition.task.phase=Phase::Executing;intent.transition.task.checkpoint.step="guild_bank_deposit";intent.transition.receipt=rid(4);
    assert(db.Write(OperationRequestWrite(intent)));
    task=intent.transition.task;++task.revision;task.phase=Phase::Verifying;
    OperationResult native;native.id=rid(4);native.task=task.id;native.taskRevision=4;native.kind="guild_bank_deposit";
    native.state=OperationState::Verified;native.nativeReference="guild_delivery:12";
    native.evidence="native_guild_deposit_items_and_stock_observed";
    assert(db.Write(ConsumedOperationWrite(task,4,native,rid(5),"{\"source_count\":4,\"bank_count\":6}",{{item,4}}).journal));
    assert(db.Execute("CREATE TABLE guild_society_supply_delivery(delivery_id BIGINT PRIMARY KEY,guild_id INT,goal_id VARCHAR(64),donor_guid INT,carrier_guid INT,item_entry INT,quantity INT,mail_id INT,phase VARCHAR(24),deposited_quantity INT)"));
    assert(db.Execute("CREATE TABLE guild_society_supply_goal(guild_id INT,goal_id VARCHAR(64) PRIMARY KEY,state VARCHAR(24),request_kind VARCHAR(16),item_entry INT,required_quantity INT,reserved_quantity INT)"));
    assert(db.Execute("CREATE TABLE guild_society_supply_execution(guild_id INT PRIMARY KEY,enabled INT)"));
    assert(db.Execute("CREATE TABLE guild_bank_item(guildid INT,item_guid INT PRIMARY KEY,item_entry INT)"));
    assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES(798,5,500)"));
    assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(798001,2770,798,4),(798002,2770,0,6)"));
    assert(db.Execute("INSERT INTO character_inventory(guid,item,bag,slot) VALUES(798,798001,0,23)"));
    assert(db.Execute("INSERT INTO guild_bank_item VALUES(31,798002,2770)"));
    assert(db.Execute("INSERT INTO guild_society_supply_goal VALUES(31,'guild_database_fixture','active','item',2770,6,0)"));
    assert(db.Execute("INSERT INTO guild_society_supply_execution VALUES(31,1)"));
    const auto proof=GuildDepositNativeProof(q,task,4,4,6,500);
    assert(db.Scalar(proof).empty()); // No native delivery receipt.
    assert(db.Execute("INSERT INTO guild_society_supply_delivery VALUES(12,31,'guild_database_fixture',798,798,2770,4,0,'completed',4)"));
    assert(db.Scalar(proof)==task.id);
    assert(db.Execute("UPDATE guild_society_supply_goal SET state='cancelled'"));assert(db.Scalar(proof).empty());
    assert(db.Execute("UPDATE guild_society_supply_delivery SET phase='cancelled',deposited_quantity=0"));
    assert(db.Execute("UPDATE item_instance SET count=8 WHERE guid=798001"));
    assert(db.Execute("UPDATE item_instance SET count=2 WHERE guid=798002"));
    assert(db.Scalar(GuildDepositNativeProof(q,task,8,8,2,500))==task.id); // Rejection after cancellation is safe.
    assert(db.Execute("UPDATE item_instance SET count=4 WHERE guid=798001"));
    assert(db.Execute("UPDATE item_instance SET count=6 WHERE guid=798002"));
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=2;GuildDeliverySettlement settled;std::string blocker;
    assert(PrepareGuildDeliverySettlement(task,task.context,claims,1001,rid(6),settled,blocker));
    assert(!db.Write(settled.plan)); // Cancellation is not delivery completion.
    assert(db.Execute("UPDATE guild_society_supply_delivery SET phase='completed',deposited_quantity=4"));
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='pretend' WHERE operation_id="+SqlValue(rid(4))));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_guild_deposit_items_and_stock_observed',before_state=JSON_SET(before_state,'$.native.native.amount',3) WHERE operation_id="+SqlValue(rid(4))));
    assert(!db.Write(settled.plan)); // Under-delivery cannot satisfy a four-item obligation.
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.amount',4) WHERE operation_id="+SqlValue(rid(4))));
    // An earlier acknowledged no-effect rejection neither counts as a deposit
    // nor prevents the later exact receipt from completing the same task.
    assert(db.Execute("INSERT INTO living_activity_operation SELECT "+SqlValue(rid(7))+",task_id,task_revision,kind,request_hash,'rejected',native_reference,before_state,after_state,'native_guild_deposit_rejected_without_effect',created_at_ms,updated_at_ms FROM living_activity_operation WHERE operation_id="+SqlValue(rid(4))));
    assert(!db.Write(settled.plan,true));
    assert(db.Write(settled.plan) && db.Write(settled.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+SqlValue(task.id)+" AND phase='completed'")=="1");
}
