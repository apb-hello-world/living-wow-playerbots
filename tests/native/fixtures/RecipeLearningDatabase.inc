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
    auto stored=book;stored.id=rid(8);stored.itemGuid=796002;stored.itemEntry=4470;stored.quantity=8;stored.location="bank";
    ++task.revision;
    assert(db.Write(ResourceReservationWrite(task,2,rid(3),{{book,0},{stored,0}},
        {{796,796001,6326,1,0,"bags"},{796,796002,4470,8,0,"bank"}})));
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
    const std::vector<NativeResourceBalance> stock{{796,796002,4470,8,0,"bank"}};claims.claims={stored};
    assert(PrepareRecipeLearningSettlement(task,task.context,claims,1001,rid(6),settled,blocker,stock));
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
    auto noClaims=claims;noClaims.claims.clear();
    assert(PrepareRecipeLearningResumption(task,current,noClaims,{},1001,rid(7),resume,blocker));
    assert(!db.Write(resume.plan)); // Any historical attempt forbids automatic replay.
    assert(!db.Write(settled.plan,true));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="verifying");
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="held");
    assert(db.Execute("UPDATE living_activity_claim SET revision=2 WHERE claim_id="+SqlValue(stored.id)));
    assert(!db.Write(settled.plan));
    assert(db.Execute("UPDATE living_activity_claim SET revision=1 WHERE claim_id="+SqlValue(stored.id)));
    assert(db.Write(settled.plan));assert(db.Write(settled.plan));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="released");
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id="+SqlValue(stored.id))=="8");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="completed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+SqlValue(task.id))=="1");

    // Metadata-only service resumption: a verified collection is not a cast.
    // Physical gameplay receipts are independently tested in the copied realm.
    const auto sid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984798000")+std::to_string(n);};
    task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f97";task.actor=task.context.actor=797;
    task.sourceKey="recipe_service_database_fixture";task.revision=1;task.phase=Phase::Queued;
    assert(db.Write(TaskWrite(task,0,sid(0),"service_fixture_created")));
    task.phase=Phase::Preparing;++task.revision;assert(db.Write(TaskWrite(task,1,sid(1),"service_fixture_preparing")));
    book.id=sid(2);book.task=task.id;book.actor=797;book.itemGuid=797001;
    ++task.revision;const std::vector<NativeResourceBalance> collected{{797,797001,6326,1,0,"bags"}};
    assert(db.Write(ResourceReservationWrite(task,2,sid(3),{{book,0}},collected)));
    ++task.revision;task.phase=Phase::Executing;task.checkpoint.step="profession_mail_collect";
    assert(db.Write(OperationIntentWrite(task,3,sid(4),"mail_collect","{}")));
    native.id=sid(4);native.task=task.id;native.kind="mail_collect";native.taskRevision=4;
    native.nativeReference="fixture-mail:797";native.evidence="fixture_service_metadata_only";
    ++task.revision;task.phase=Phase::Verifying;
    assert(db.Write(OperationOutcomeWrite(task,4,native,sid(5),"{}")));
    assert(db.Execute("CREATE TABLE IF NOT EXISTS item_instance(guid INT PRIMARY KEY,itemEntry INT,owner_guid INT,count INT)"));
    assert(db.Execute("CREATE TABLE IF NOT EXISTS character_inventory(guid INT,item INT PRIMARY KEY,bag INT,slot INT)"));
    assert(db.Execute("INSERT INTO item_instance VALUES(797001,6326,797,1)"));
    assert(db.Execute("INSERT INTO character_inventory VALUES(797,797001,0,23)"));
    claims.claims={book};current=task.context;++current.actorGeneration;
    assert(PrepareRecipeLearningResumption(task,current,claims,collected,1002,sid(6),resume,blocker));
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(sid(4))));
    assert(!db.Write(resume.plan));
    assert(db.Execute("UPDATE living_activity_operation SET state='verified' WHERE operation_id="+SqlValue(sid(4))));
    assert(db.Execute("UPDATE item_instance SET count=0 WHERE guid=797001"));
    assert(!db.Write(resume.plan));
    assert(db.Execute("UPDATE item_instance SET count=1 WHERE guid=797001"));
    assert(db.Execute("UPDATE living_activity_claim SET state='released' WHERE claim_id="+SqlValue(book.id)));
    assert(!db.Write(resume.plan));
    assert(db.Execute("UPDATE living_activity_claim SET state='held' WHERE claim_id="+SqlValue(book.id)));
    assert(!db.Write(resume.plan,true));
    assert(db.Write(resume.plan));assert(db.Write(resume.plan));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="preparing");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+SqlValue(task.id))=="1");
}
