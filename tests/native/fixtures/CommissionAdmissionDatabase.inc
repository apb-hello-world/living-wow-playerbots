// Actual transactional SQL, including lost acknowledgements and conflicts.
// Fixtures here never imply a native craft, item transfer or payment occurred.
static void CommissionAdmissionDatabase(Connection& db) {
    assert(db.Execute("ALTER TABLE organic_economy_commission ADD recipe_spell_id INT,ADD output_item_entry INT,"
        "ADD quantity INT,ADD materials_source VARCHAR(8),ADD service_fee_copper INT,ADD expires_at DATETIME"));
    ProfessionJob recipe;recipe.recipe=2329;recipe.skill=171;recipe.initialSkill=25;
    recipe.purpose=ProfessionPurpose::RequestedItem;recipe.outputEntry=2454;recipe.outputQuantity=3;
    recipe.reagents={{765,1},{2449,1},{3371,1}};
    CommissionJob job;job.agreement={"lwc-9988001","wow-tx-native-admission","mail",EncodeProfessionJob(recipe),9988,9989,60,2000};
    job.craft=job.agreement.recipe;
    Task task;task.id=task.root="ef8c546c-25c2-50b6-a7f6-60d87a379631";task.actor=task.context.actor=9988;
    task.source="commission_job";task.sourceKey=job.agreement.id;task.kind=Kind::Commission;
    task.mode=Mode::Active;task.accepted=true;task.phase=Phase::Queued;task.revision=1;
    task.createdAtMs=task.updatedAtMs=2000;task.context.policyRevision=1;
    task.checkpoint.step="profession_prepare";task.checkpoint.data=EncodeCommissionJob(job);
    const auto receipt="ef8c546c-25c2-50b6-a7f6-60d87a379632";
    const auto plan=CommissionAdmissionWrite(task,receipt);
    assert(!db.Write(plan,true)); // A failed transaction creates neither half.
    assert(db.Scalar("SELECT COUNT(*) FROM organic_economy_commission WHERE commission_id='lwc-9988001'")=="0");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="0");
    assert(db.Write(plan));assert(db.Write(plan)); // Lost acknowledgement is not a second acceptance.
    assert(db.Scalar("SELECT quantity FROM organic_economy_commission WHERE commission_id='lwc-9988001'")=="3");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+SqlValue(task.id))=="1");
    for(unsigned fault=0;fault<5;++fault) {
        auto wrong=job;auto changed=task;
        if(fault==0)wrong.agreement.feeCopper++;
        if(fault==1)wrong.agreement.recipient++;
        if(fault==2) {auto r=recipe;r.outputQuantity=4;wrong.craft=wrong.agreement.recipe=EncodeProfessionJob(r);}
        if(fault==3) {wrong.agreement.actor+=2;changed.actor=changed.context.actor=wrong.agreement.actor;}
        if(fault==4) {changed.id=changed.root="ef8c546c-25c2-50b6-a7f6-60d87a379633";}
        changed.checkpoint.data=EncodeCommissionJob(wrong);
        assert(!db.Write(CommissionAdmissionWrite(changed,receipt)));
    }
    assert(db.Scalar("SELECT service_fee_copper FROM organic_economy_commission WHERE commission_id='lwc-9988001'")=="60");
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id="+SqlValue(task.id))=="queued");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation WHERE task_id="+SqlValue(task.id))=="0");
    for(const auto* delivery:{"direct","meeting"}) {
        auto agreed=job;auto admitted=task;
        const bool direct=std::string(delivery)=="direct";
        agreed.agreement.delivery=delivery;agreed.agreement.id=direct?"lwc-9988002":"lwc-9988003";
        agreed.agreement.transaction=std::string("wow-tx-native-")+delivery;
        admitted.sourceKey=agreed.agreement.id;
        admitted.id=admitted.root=direct?"ef8c546c-25c2-50b6-a7f6-60d87a379634":"ef8c546c-25c2-50b6-a7f6-60d87a379635";
        admitted.checkpoint.data=EncodeCommissionJob(agreed);
        const auto acknowledged=direct?"ef8c546c-25c2-50b6-a7f6-60d87a379636":"ef8c546c-25c2-50b6-a7f6-60d87a379637";
        const auto write=CommissionAdmissionWrite(admitted,acknowledged);
        assert(!db.Write(write,true));assert(db.Write(write));assert(db.Write(write));
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition WHERE task_id="+SqlValue(admitted.id))=="1");
        auto changed=agreed;changed.agreement.delivery="mail";
        admitted.checkpoint.data=EncodeCommissionJob(changed);
        assert(!db.Write(CommissionAdmissionWrite(admitted,acknowledged)));
    }
}
