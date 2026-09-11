// Journal metadata only. This does not create native game items or skill-ups.
static void ProfessionSettlementDatabase(Connection& db) {
    using namespace LivingActivityTest;
    const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-2998479750")+(n<10 ? "0" : "")+std::to_string(n);};
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f92";
    task.actor=task.context.actor=703;task.source="profession_job";task.sourceKey="settlement_fixture";
    task.mode=Mode::Active;task.phase=Phase::Queued;task.kind=Kind::Profession;
    task.createdAtMs=task.updatedAtMs=1000;task.context.boot="fixture_boot";
    ProfessionJob job;job.recipe=2329;job.skill=171;job.initialSkill=1;job.targetSkill=2;
    job.outputEntry=2454;job.outputQuantity=1;job.reagents={{765,1},{2449,1},{3371,1}};
    task.checkpoint.data=EncodeProfessionJob(job);
    assert(db.Write(TaskWrite(task,0,rid(0),"fixture_created")));
    task.phase=Phase::Preparing;++task.revision;
    assert(db.Write(TaskWrite(task,1,rid(1),"fixture_preparing")));
    CraftFrame before;before.actor=703;before.money=100;before.skill=1;
    before.stacks={{703,100,765,1,0,23},{703,101,2449,1,0,24},{703,102,3371,5,0,25}};
    auto after=before;after.skill=2;after.stacks={{703,102,3371,4,0,25},{703,103,2454,1,0,23}};
    auto row=SavedCraft(task,job,before,after,false,true);
    auto verifiedTask=task;verifiedTask.phase=Phase::Verifying;verifiedTask.revision=5;
    StoredCraftProof proof;std::string blocker;assert(DecodeStoredCraftProof(verifiedTask,row,proof,blocker));
    std::vector<ClaimReceiptChange> reserve;std::vector<ResourceClaim> held;
    for (const auto& use : proof.inputs) {reserve.push_back({use.before,0});held.push_back(use.before);}
    std::vector<NativeResourceBalance> initial={{703,100,765,1,0,"bags"},{703,101,2449,1,0,"bags"},{703,102,3371,5,0,"bags"}};
    ++task.revision;assert(db.Write(ResourceReservationWrite(task,2,rid(2),reserve,initial)));
    ResourceClaimBook book;assert(book.FinishRestore());assert(book.InstallReceipt(reserve)==ClaimInstall::Installed);
    task.phase=Phase::Executing;++task.revision;
    assert(db.Write(OperationIntentWrite(task,3,row.receipt.id,"profession_craft",row.beforeState)));
    boost::property_tree::ptree parsed;std::istringstream stream(row.afterState);boost::property_tree::read_json(stream,parsed);
    std::ostringstream capture;boost::property_tree::write_json(capture,parsed.get_child("native.result"),false);
    task=verifiedTask;
    const auto outcome=AcquiredOperationWrite(task,4,row.receipt,rid(3),capture.str(),proof.inputs,{2454,1},proof.gains);
    assert(db.Write(outcome.journal));assert(book.InstallReceipt(outcome.changes)==ClaimInstall::Installed);
    auto snapshot=[&]() {
        ProfessionHistoryCursor history;assert(history.Begin(task,db.History(task),blocker));
        while (!history.Result().complete) assert(history.Advance(blocker));
        ProfessionSnapshot view;view.task=task.id;view.revision=task.revision;view.context=task.context;
        view.complete=true;view.unresolvedOperation=history.Result().unresolvedOperation;
        view.attempts=history.Result().attempts;view.stock={{765},{2449},{3371}};return view;
    };
    const auto view=snapshot();UnsettledClaimBatch batch;
    assert(book.ReadUnsettled(task.id,batch,blocker) && batch.complete && batch.claims.size()==2);
    std::vector<NativeResourceBalance> remaining={{703,102,3371,4,0,"bags"},{703,103,2454,1,0,"bags"}};
    ProfessionSettlement settlement;
    assert(PrepareProfessionSettlement(task,view,batch,remaining,1001,rid(4),settlement,blocker));
    assert(!db.Write(settlement.plan,true));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="verifying");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND state='held'")=="2");
    // A changed persisted result cannot hide behind a previously decoded copy.
    const auto exactAfter=db.Scalar("SELECT after_state FROM living_activity_operation WHERE operation_id="+SqlValue(row.receipt.id));
    assert(db.Execute("UPDATE living_activity_operation SET after_state=JSON_SET(after_state,'$.native.result.after.skill',1) WHERE operation_id="+SqlValue(row.receipt.id)));
    assert(!db.Write(settlement.plan));
    assert(db.Execute("UPDATE living_activity_operation SET after_state="+SqlValue(exactAfter)+" WHERE operation_id="+SqlValue(row.receipt.id)));
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(row.receipt.id)));
    assert(!db.Write(settlement.plan));
    assert(db.Execute("UPDATE living_activity_operation SET state='verified' WHERE operation_id="+SqlValue(row.receipt.id)));
    // New, unobserved root obligations prohibit premature completion.
    const std::string proposal="ff2efbdf-f0ec-4539-b840-299847976000";
    assert(db.Execute("INSERT INTO living_activity_claim (claim_id,task_id,actor_guid,item_entry,quantity,location,state,revision,updated_at_ms) VALUES ("+
        SqlValue(proposal)+','+SqlValue(task.id)+",703,765,1,'bags','proposed',1,1000)"));
    assert(!db.Write(settlement.plan));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND state='released'")=="0");
    ResourceClaim candidate;candidate.id=proposal;candidate.task=task.id;candidate.actor=703;candidate.itemEntry=765;
    candidate.quantity=1;candidate.location="bags";candidate.state="proposed";
    std::vector<ClaimReceiptChange> demands={{candidate,0}};
    for (unsigned i=1;i<17;++i) {
        auto p=candidate;p.id="ff2efbdf-f0ec-4539-b840-2998479760"+std::string(i<10 ? "0" : "")+std::to_string(i);
        assert(db.Execute("INSERT INTO living_activity_claim (claim_id,task_id,actor_guid,item_entry,quantity,location,state,revision,updated_at_ms) VALUES ("+
            SqlValue(p.id)+','+SqlValue(task.id)+",703,765,1,'bags','proposed',1,1000)"));
        demands.push_back({p,0});
    }
    // Native receipt batches remain <=16; no synthetic 'all settled' flag.
    assert(book.InstallReceipt({demands.begin(),demands.begin()+16})==ClaimInstall::Installed);
    assert(book.InstallReceipt({demands.begin()+16,demands.end()})==ClaimInstall::Installed);
    assert(book.ReadUnsettled(task.id,batch,blocker) && !batch.complete && batch.claims.size()==16);
    assert(PrepareProfessionSettlement(task,view,batch,remaining,1001,rid(5),settlement,blocker));
    assert(settlement.task.phase==Phase::Verifying);
    assert(db.Write(settlement.plan));assert(db.Write(settlement.plan));
    const auto first=settlement;task=settlement.task;
    assert(book.InstallReceipt(settlement.claims)==ClaimInstall::Installed);
    assert(book.ReadUnsettled(task.id,batch,blocker) && batch.complete && batch.claims.size()==3);
    const auto refreshed=snapshot();
    assert(PrepareProfessionSettlement(task,refreshed,batch,remaining,1002,rid(6),settlement,blocker));
    assert(settlement.task.phase==Phase::Completed);
    assert(!db.Write(settlement.plan,true));
    {Connection restarted;assert(restarted.ReceiptPresent(first.plan));assert(restarted.Write(settlement.plan));}
    assert(db.Write(settlement.plan)); // Lost completion acknowledgement, no duplicate release.
    assert(book.InstallReceipt(settlement.claims)==ClaimInstall::Installed);
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND state NOT IN ('consumed','released')")=="0");
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND item_guid=102")=="4");
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE task_id="+SqlValue(task.id)+" AND item_guid=103")=="1");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+SqlValue(task.id)+" AND code='profession_completed'")=="1");
    ProfessionSettlement changed;
    assert(PrepareProfessionSettlement(task,refreshed,batch,remaining,1003,rid(6),changed,blocker));
    assert(!db.Write(changed.plan)); // Same receipt ID with different content.
    assert(db.Scalar("SELECT updated_at_ms FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="1002");
}
