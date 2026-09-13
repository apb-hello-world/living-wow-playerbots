// Isolated SQL/journal fixtures, not claims of native gameplay. Each supplied
// operation is explicitly tested against wrong proof, changed state and replay.
static void GuildCancellationDatabase(Connection& db) {
    assert(db.Execute("ALTER TABLE guild_society_supply_goal ADD COLUMN updated_at INT NOT NULL DEFAULT 1"));
    assert(db.Execute("ALTER TABLE guild_society_supply_delivery ADD COLUMN blocker VARCHAR(100) NOT NULL DEFAULT '', ADD COLUMN updated_at INT NOT NULL DEFAULT 1"));
    unsigned sequence=0;auto rid=[&](){return "f891b1a2-5e4b-4d33-9901-"+std::to_string(100000000000ull+ ++sequence);};
    for(unsigned variant=0;variant<3;++variant) {
        const bool incoming=variant>0,partial=variant==2;
        const uint32_t actor=9881+variant,original=9881000+variant*10,survivor=original+1,entry=2772;
        GuildDeliveryJob j{41+variant,31,actor,entry,4,incoming?91000+variant:0,"cancel_fixture_"+std::to_string(variant),false};
        if(incoming)--j.donor;
        Task t;t.id=t.root=rid();t.actor=t.context.actor=actor;t.source="guild_delivery";
        t.sourceKey=GuildDeliverySourceKey(j,actor);t.kind=Kind::GuildDelivery;t.priority=Priority::Delivery;
        t.mode=Mode::Active;t.accepted=true;t.phase=Phase::Queued;t.createdAtMs=t.updatedAtMs=1000;
        t.context.boot=rid();t.context.actorGeneration=t.context.mapGeneration=t.context.policyRevision=1;
        t.checkpoint.step="guild_delivery_prepare";t.checkpoint.data=EncodeGuildDeliveryJob(j);
        assert(db.Write(TaskWrite(t,0,rid(),"fixture_created")));
        auto prepare=[&](){const auto before=t.revision++;t.phase=Phase::Preparing;t.checkpoint.step="guild_delivery_prepare";
            assert(db.Write(TaskWrite(t,before,rid(),"fixture_preparing")));};
        prepare();
        ResourceClaim c;c.id=rid();c.task=t.id;c.actor=actor;c.itemGuid=original;c.itemEntry=entry;c.quantity=4;
        c.state="held";c.location=incoming?"mail":"bags";c.nativeReference=j.incomingMail;
        auto before=t.revision++;
        assert(db.Write(ResourceReservationWrite(t,before,rid(),{{c,0}},{{actor,original,entry,12,0,c.location,c.nativeReference}})));
        std::string collection;
        if(incoming) {
            OperationRequest op;op.kind="mail_collect";op.effects=Mask(Effect::Inventory);op.persistence=NativePersistence::Inventory;
            op.itemTransfer=c;op.beforeState="{\"actor\":"+std::to_string(actor)+",\"mail\":"+std::to_string(j.incomingMail)+
                ",\"guid\":"+std::to_string(original)+",\"entry\":2772,\"quantity\":4,\"merge_guid\":"+
                std::to_string(survivor)+",\"merge_count\":8}";
            op.transition.task=t;op.transition.expectedRevision=t.revision;++op.transition.task.revision;
            op.transition.task.phase=Phase::Executing;op.transition.receipt=rid();
            assert(db.Write(OperationRequestWrite(op)));t=op.transition.task;++t.revision;t.phase=Phase::Verifying;
            OperationResult proof;proof.id=collection=op.transition.receipt;proof.task=t.id;proof.taskRevision=t.revision-1;
            proof.kind=op.kind;proof.state=OperationState::Verified;proof.nativeReference="isolated-metadata";
            proof.evidence="native_mail_attachment_collected";
            auto outcome=ItemTransferWrite(t,t.revision-1,proof,rid(),"{\"surviving_guid\":"+std::to_string(survivor)+'}',c,survivor);
            assert(db.Write(outcome.journal));c=outcome.changes.front().after;prepare();
        }
        if(partial) {
            GuildDepositQuote q;q.job=j;q.actor=actor;q.item=c.itemGuid;q.itemCount=q.bagCount=12;q.amount=1;
            q.bankCount=19;q.goalTarget=50;q.money=500;q.bank=321;q.position=65303;
            OperationRequest op;op.kind="guild_bank_deposit";op.effects=Mask(Effect::Inventory)|Mask(Effect::Guild);
            op.persistence=NativePersistence::Inventory;op.beforeState=EncodeGuildDepositQuote(q);op.consumption={{c,1}};
            op.transition.task=t;op.transition.expectedRevision=t.revision;++op.transition.task.revision;
            op.transition.task.phase=Phase::Executing;op.transition.receipt=rid();assert(db.Write(OperationRequestWrite(op)));
            t=op.transition.task;++t.revision;t.phase=Phase::Verifying;
            OperationResult proof;proof.id=op.transition.receipt;proof.task=t.id;proof.taskRevision=t.revision-1;
            proof.kind=op.kind;proof.state=OperationState::Verified;proof.nativeReference="isolated-metadata";
            proof.evidence="native_guild_deposit_items_and_stock_observed";
            auto outcome=ConsumedOperationWrite(t,t.revision-1,proof,rid(),"{}",{{c,1}});
            assert(db.Write(outcome.journal));c=outcome.changes.front().after;prepare();
        }
        const auto n=[](uint64_t v){return std::to_string(v);};
        assert(db.Execute("INSERT INTO characters(guid,race,money) VALUES("+n(actor)+",5,500)"));
        assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES("+n(c.itemGuid)+",2772,"+n(actor)+','+n(partial?11:12)+')'));
        assert(db.Execute("INSERT INTO character_inventory(guid,item,bag,slot) VALUES("+n(actor)+','+n(c.itemGuid)+",0,23)"));
        assert(db.Execute("INSERT INTO guild_society_supply_delivery(delivery_id,guild_id,goal_id,donor_guid,carrier_guid,item_entry,quantity,mail_id,phase,deposited_quantity,item_guid) VALUES("+
            n(j.delivery)+",31,"+SqlValue(j.goal)+','+n(j.donor)+','+n(actor)+",2772,4,"+n(j.incomingMail)+",'carried',"+n(partial?1:0)+','+n(original)+')'));
        assert(db.Execute("INSERT INTO guild_society_supply_goal(guild_id,goal_id,state,request_kind,item_entry,required_quantity,reserved_quantity) VALUES(31,"+
            SqlValue(j.goal)+','+SqlValue(partial?"active":"cancelled")+",'item',2772,20,0)"));
        if(partial) {
            assert(db.Execute("INSERT INTO item_instance(guid,itemEntry,owner_guid,count) VALUES(9881999,2772,0,20)"));
            assert(db.Execute("INSERT INTO guild_bank_item VALUES(31,9881999,2772)"));
        }
        GuildDeliveryClosure snapshot;snapshot.goalState=partial?"active":"cancelled";snapshot.target=20;
        snapshot.goalUpdated=1;snapshot.deposited=partial?1:0;snapshot.originalItem=original;snapshot.banked=partial?20:0;
        assert(db.Scalar(GuildDeliveryClosureQuery(j,actor))==snapshot.goalState);
        UnsettledClaimBatch claims;claims.bookRevision=10;claims.complete=true;claims.claims={c};
        NativeResourceBalance parcel{actor,c.itemGuid,entry,partial?11u:12u,0,"bags"};
        GuildDeliverySettlement closed;std::string why;const auto receipt=rid();
        assert(PrepareGuildDeliveryCancellation(t,t.context,snapshot,parcel,claims,{parcel},1001,receipt,closed,why));
        const auto taskWhere=" WHERE task_id="+SqlValue(t.id),goalWhere=" WHERE goal_id="+SqlValue(j.goal);
        auto unchanged=[&]() {
            assert(db.Scalar("SELECT phase FROM living_activity_task"+taskWhere)=="preparing");
            assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(c.id))=="held");
            assert(db.Scalar("SELECT phase FROM guild_society_supply_delivery WHERE delivery_id="+n(j.delivery))=="carried");
            assert(db.Scalar("SELECT count FROM item_instance WHERE guid="+n(c.itemGuid))==n(parcel.quantity));
            assert(db.Scalar("SELECT money FROM characters WHERE guid="+n(actor))=="500");
        };
        assert(db.Execute("UPDATE guild_society_supply_goal SET required_quantity=21"+goalWhere));
        assert(!db.Write(closed.plan));unchanged();
        assert(db.Execute("UPDATE guild_society_supply_goal SET required_quantity=20,state='paused'"+goalWhere));
        assert(!db.Write(closed.plan));unchanged();
        assert(db.Execute("UPDATE guild_society_supply_goal SET state="+SqlValue(snapshot.goalState)+goalWhere));
        assert(db.Execute("UPDATE item_instance SET owner_guid=1 WHERE guid="+n(c.itemGuid)));
        assert(!db.Write(closed.plan));assert(db.Execute("UPDATE item_instance SET owner_guid="+n(actor)+" WHERE guid="+n(c.itemGuid)));unchanged();
        assert(db.Execute("UPDATE living_activity_claim SET revision=revision+1 WHERE claim_id="+SqlValue(c.id)));
        assert(!db.Write(closed.plan));assert(db.Execute("UPDATE living_activity_claim SET revision=revision-1 WHERE claim_id="+SqlValue(c.id)));unchanged();
        if(incoming) {
            assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(collection)));
            assert(!db.Write(closed.plan));assert(db.Execute("UPDATE living_activity_operation SET state='verified',evidence_code='pretend' WHERE operation_id="+SqlValue(collection)));
            assert(!db.Write(closed.plan));assert(db.Execute("UPDATE living_activity_operation SET evidence_code='native_mail_attachment_collected' WHERE operation_id="+SqlValue(collection)));
            assert(db.Execute("INSERT INTO mail_items VALUES("+n(j.incomingMail)+','+n(original)+",2772,"+n(actor)+')'));
            assert(!db.Write(closed.plan));assert(db.Execute("DELETE FROM mail_items WHERE mail_id="+n(j.incomingMail)));unchanged();
        }
        if(partial) {
            assert(db.Execute("UPDATE item_instance SET count=19 WHERE guid=9881999"));
            assert(!db.Write(closed.plan));unchanged();
            assert(db.Execute("UPDATE item_instance SET count=20 WHERE guid=9881999"));
        }
        assert(!db.Write(closed.plan,true));unchanged(); // Atomic failure leaves all references held.
        assert(db.Write(closed.plan) && db.Write(closed.plan)); // Lost acknowledgement does not close twice.
        assert(db.Scalar("SELECT phase FROM living_activity_task"+taskWhere)=="cancelled");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition"+taskWhere+" AND phase='cancelled'")=="1");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition"+taskWhere+" AND phase='completed'")=="0");
        assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(c.id))=="released");
        assert(db.Scalar("SELECT deposited_quantity FROM guild_society_supply_delivery WHERE delivery_id="+n(j.delivery))==n(snapshot.deposited));
        assert(db.Scalar("SELECT count FROM item_instance WHERE guid="+n(c.itemGuid))==n(parcel.quantity));
        assert(db.Scalar("SELECT money FROM characters WHERE guid="+n(actor))=="500");
        assert(db.Scalar("SELECT state FROM guild_society_supply_goal"+goalWhere)==snapshot.goalState);
    }
}
