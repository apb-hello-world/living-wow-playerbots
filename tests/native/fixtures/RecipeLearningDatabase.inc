// Isolated journal metadata proof, not a native learning/gameplay receipt.
static void RecipeLearningDatabase(Connection& db) {
    const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984797900")+std::to_string(n);};
    RecipeLearningJob job{6326,7752,185,483};std::string blocker;
    Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f96";
    task.actor=task.context.actor=796;task.source="recipe_learning";task.sourceKey="recipe_database_fixture";
    task.kind=Kind::Profession;task.mode=Mode::Active;task.phase=Phase::Queued;task.createdAtMs=task.updatedAtMs=1000;
    task.context.boot="e72efbdf-f0ec-4539-b840-299847979001";
    task.context.actorGeneration=task.context.mapGeneration=task.context.policyRevision=1;
    task.checkpoint.step="recipe_prepare";task.checkpoint.data=EncodeRecipeLearningJob(job);
    assert(db.Write(TaskWrite(task,0,rid(0),"fixture_created")));
    task.phase=Phase::Preparing;++task.revision;assert(db.Write(TaskWrite(task,1,rid(1),"fixture_preparing")));
    ResourceClaim book;book.id=rid(2);book.task=task.id;book.actor=796;book.itemGuid=796001;
    book.itemEntry=6326;book.quantity=1;book.location="bags";book.state="held";
    ++task.revision;
    assert(db.Write(ResourceReservationWrite(task,2,rid(3),{{book,0}},{{796,796001,6326,1,0,"bags"}})));
    OperationRequest intent;intent.kind="recipe_learning";intent.effects=Mask(Effect::Inventory)|Mask(Effect::Spell);
    intent.persistence=NativePersistence::Profession;intent.beforeState=EncodeRecipeLearningJob(job);intent.consumption={{book,1}};
    intent.transition.task=task;intent.transition.expectedRevision=3;++intent.transition.task.revision;
    intent.transition.task.phase=Phase::Executing;intent.transition.receipt=rid(4);
    assert(db.Write(OperationRequestWrite(intent)));
    task=intent.transition.task;++task.revision;task.phase=Phase::Verifying;
    OperationResult native;native.id=rid(4);native.task=task.id;native.taskRevision=4;native.kind="recipe_learning";
    native.state=OperationState::Verified;native.nativeReference="spell:7752:book:796001";
    native.evidence="native_recipe_book_consumed_and_spell_learned";
    const auto outcome=ConsumedOperationWrite(task,4,native,rid(5),
        "{\"recipe\":7752,\"skill_id\":185,\"before\":{\"entry\":6326}}",{{book,1}});
    assert(db.Write(outcome.journal));
    assert(db.Execute("CREATE TABLE IF NOT EXISTS character_spell(guid INT,spell INT,disabled TINYINT DEFAULT 0,PRIMARY KEY(guid,spell))"));
    UnsettledClaimBatch claims;claims.complete=true;claims.bookRevision=2;RecipeLearningSettlement settled;
    assert(PrepareRecipeLearningSettlement(task,task.context,claims,1001,rid(6),settled,blocker));
    assert(!db.Write(settled.plan)); // No native spellbook row.
    assert(db.Execute("INSERT INTO character_spell(guid,spell,disabled) VALUES(796,7752,0)"));
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(rid(4))));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_operation SET state='verified',after_state=JSON_SET(after_state,'$.native.recipe',7751) WHERE operation_id="+SqlValue(rid(4))));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_operation SET after_state=JSON_SET(after_state,'$.native.recipe',7752) WHERE operation_id="+SqlValue(rid(4))));
    assert(db.Execute("UPDATE living_activity_claim SET state='held' WHERE claim_id="+SqlValue(book.id)));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_claim SET state='consumed' WHERE claim_id="+SqlValue(book.id)));
    auto current=task.context;++current.actorGeneration;RecipeLearningSettlement resume;
    assert(PrepareRecipeLearningResumption(task,current,claims,{},1001,rid(7),resume,blocker));
    assert(!db.Write(resume.plan)); // Any historical attempt forbids automatic replay.
    assert(!db.Write(settled.plan,true));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="verifying");
    assert(db.Write(settled.plan));assert(db.Write(settled.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+SqlValue(task.id))=="1");
}
