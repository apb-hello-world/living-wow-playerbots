// Real MariaDB journal/settlement SQL only. These supplied receipts are NOT
// evidence of native mail or bank gameplay; the copied realm tests that path.
static void GuildMailDatabase(Connection& db) {
    unsigned sequence=0;
    auto receipt=[&](){return "1a4d884b-5fe4-4b63-8f29-"+std::to_string(100000000000ull+ ++sequence);};
    Task task;task.id=task.root="b9828fa2-6010-49ac-a309-d2898cab49dc";
    task.actor=task.context.actor=9798;task.source="guild_delivery";
    GuildDepositQuote q;q.job={13,31,9797,2770,4,90013,"guild_mail_database_fixture",false};
    q.actor=task.actor;q.item=9798001;q.itemCount=q.bagCount=q.amount=4;
    q.bankCount=6;q.goalTarget=10;q.money=500;q.bank=321;q.position=65303;
    task.sourceKey=GuildDeliverySourceKey(q.job,task.actor);task.kind=Kind::GuildDelivery;
    task.mode=Mode::Active;task.priority=Priority::Delivery;task.accepted=true;task.phase=Phase::Queued;
    task.createdAtMs=task.updatedAtMs=1000;task.context.boot=receipt();
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.checkpoint.step="guild_mail_prepare";task.checkpoint.data=EncodeGuildDeliveryJob(q.job);
    assert(db.Write(TaskWrite(task,0,receipt(),"fixture_created")));
    auto prepare=[&]() {
        auto expected=task.revision++;task.phase=Phase::Preparing;
        assert(db.Write(TaskWrite(task,expected,receipt(),"fixture_preparing")));
    };
    prepare();
    auto reserve=[&](uint32_t guid,uint32_t entry,uint32_t quantity,const char* location,uint64_t reference=0) {
        ResourceClaim c;c.id=receipt();c.task=task.id;c.actor=task.actor;c.itemGuid=guid;c.itemEntry=entry;
        c.quantity=quantity;c.location=location;c.nativeReference=reference;c.state="held";
        auto expected=task.revision++;
        assert(db.Write(ResourceReservationWrite(task,expected,receipt(),{{c,0}},
            {{task.actor,guid,entry,quantity,0,location,reference}})));
        return c;
    };
    auto incoming=reserve(q.item,2770,4,"mail",90013);
    auto stored=reserve(9798002,2447,3,"bags");
    auto junk=reserve(9798003,7073,1,"bags");
    auto operation=[&](const char* kind,const std::string& quote,ResourceClaim& claim,bool consume,const char* evidence) {
        OperationRequest r;r.kind=kind;r.effects=Mask(Effect::Inventory);
        if(r.kind=="capacity_vendor_sale")r.effects|=Mask(Effect::Money);
        if(r.kind=="guild_bank_deposit")r.effects|=Mask(Effect::Guild);
        r.persistence=NativePersistence::Inventory;r.beforeState=quote;
        if(consume)r.consumption={{claim,claim.quantity}};else r.itemTransfer=claim;
        r.transition.task=task;r.transition.expectedRevision=task.revision;
        ++r.transition.task.revision;r.transition.task.phase=Phase::Executing;r.transition.receipt=receipt();
        assert(db.Write(OperationRequestWrite(r)));
        task=r.transition.task;++task.revision;task.phase=Phase::Verifying;
        OperationResult native;native.id=r.transition.receipt;native.task=task.id;
        native.taskRevision=r.transition.task.revision;native.kind=kind;native.state=OperationState::Verified;
        native.nativeReference="fixture-metadata";native.evidence=evidence;
        auto outcome=consume?ConsumedOperationWrite(task,task.revision-1,native,receipt(),"{}",{{claim,claim.quantity}}):
            ItemTransferWrite(task,task.revision-1,native,receipt(),"{}",claim);
        assert(db.Write(outcome.journal));claim=outcome.changes.front().after;
        return native.id;
    };
    const auto sale=operation("capacity_vendor_sale",
        R"({"actor":9798,"guid":9798003,"entry":7073,"quantity":1,"unit_copper":6,"money":494,"copper":6,"count_before":1,"vendor":123,"vendor_entry":1,"from":65304,"capacity_entry":2770,"capacity_quantity":4})",
        junk,true,"native_capacity_sale_money_item_and_slot_observed");
    prepare();
    const auto bank=operation("bank_deposit",
        R"({"actor":9798,"guid":9798002,"entry":2447,"quantity":3,"banker":124,"banker_entry":2,"from":65305,"to":65319,"bag_before":3,"total_before":3,"deposit":true,"money_before":500})",
        stored,false,"native_bank_stack_deposited");
    prepare();
    const auto mail=operation("mail_collect",
        R"({"actor":9798,"mail":90013,"guid":9798001,"entry":2770,"quantity":4,"mailbox":125,"mailbox_entry":3,"delivered_at":1,"expires_at":10000,"money_before":500,"mail_money":0,"attachments_before":1,"bag_before":0,"total_before":0,"to":65303})",
        incoming,false,"native_mail_attachment_collected");
    assert(incoming.location=="bags" && incoming.nativeReference==0);
    assert(stored.location=="bank" && stored.state=="held");
    assert(db.Execute("INSERT INTO guild_society_supply_delivery VALUES(13,31,'guild_mail_database_fixture',9797,9798,2770,4,90013,'carried',0,9798001)"));
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=5;claims.claims={incoming,stored};
    GuildDeliverySettlement settled;std::string why;
    const std::vector<NativeResourceBalance> owned{{9798,9798002,2447,3,0,"bank"}};
    assert(!PrepareGuildDeliverySettlement(task,task.context,claims,1001,receipt(),settled,why,owned));
    // Receipt collection is not a delivery, even if one pretended its claim gone.
    claims.claims={stored};const auto terminalReceipt=receipt();
    assert(PrepareGuildDeliverySettlement(task,task.context,claims,1001,terminalReceipt,settled,why,owned));
    assert(!db.Write(settled.plan));
    prepare();operation("guild_bank_deposit",EncodeGuildDepositQuote(q),incoming,true,"native_guild_deposit_items_and_stock_observed");
    assert(db.Execute("UPDATE guild_society_supply_delivery SET phase='completed',deposited_quantity=4 WHERE delivery_id=13"));
    assert(PrepareGuildDeliverySettlement(task,task.context,claims,1001,terminalReceipt,settled,why,owned));
    assert(settled.claims.size()==1 && settled.claims.front().after.state=="released");
    auto mutate=[&](const std::string& id,const std::string& change) {
        assert(db.Execute("UPDATE living_activity_operation SET "+change+" WHERE operation_id="+SqlValue(id)));
        assert(!db.Write(settled.plan));
    };
    mutate(mail,"evidence_code='pretend'");
    assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_mail_attachment_collected' WHERE operation_id="+SqlValue(mail)));
    mutate(mail,"before_state=JSON_SET(before_state,'$.native.native.mail',90014)");
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.mail',90013) WHERE operation_id="+SqlValue(mail)));
    mutate(mail,"before_state=JSON_SET(before_state,'$.native.native.guid',9798002)");
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.guid',9798001) WHERE operation_id="+SqlValue(mail)));
    mutate(mail,"state='reconciling'");
    assert(db.Execute("UPDATE living_activity_operation SET state='verified' WHERE operation_id="+SqlValue(mail)));
    mutate(bank,"before_state=JSON_SET(before_state,'$.native.native.entry',2770)");
    assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.native.entry',2447) WHERE operation_id="+SqlValue(bank)));
    mutate(sale,"kind='unclassified_sale'");
    assert(db.Execute("UPDATE living_activity_operation SET kind='capacity_vendor_sale' WHERE operation_id="+SqlValue(sale)));
    const auto duplicate=receipt();
    assert(db.Execute("INSERT INTO living_activity_operation SELECT "+SqlValue(duplicate)+",task_id,task_revision,kind,request_hash,state,native_reference,before_state,after_state,evidence_code,created_at_ms,updated_at_ms FROM living_activity_operation WHERE operation_id="+SqlValue(mail)));
    assert(!db.Write(settled.plan)); // Two collection receipts must not double-credit.
    assert(db.Execute("UPDATE living_activity_operation SET state='rejected',evidence_code='no_effect' WHERE operation_id="+SqlValue(duplicate)));
    assert(!db.Write(settled.plan,true)); // Fault rolls back completion AND release.
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="held");
    assert(db.Execute("UPDATE living_activity_claim SET revision=revision+1 WHERE claim_id="+SqlValue(stored.id)));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_claim SET revision=revision-1 WHERE claim_id="+SqlValue(stored.id)));
    assert(db.Write(settled.plan) && db.Write(settled.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="released");
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="3");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+SqlValue(task.id)+" AND phase='completed'")=="1");
}
