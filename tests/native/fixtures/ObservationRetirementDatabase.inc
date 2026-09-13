// Real MariaDB predicate races; these rows are isolated fixture metadata only.
void ObservationRetirementDatabase(Connection& db) {
    auto id=[](unsigned n){return std::string("b0bb4425-4e82-53d3-8295-0000000000")+(n<10?"0":"")+std::to_string(n);};
    for(unsigned scenario=0;scenario<13;++scenario) {
        Task task;task.id=task.root=id(scenario);task.source="economy_goal";
        task.sourceKey=std::to_string(53100+scenario);task.actor=task.context.actor=899;
        task.accepted=false;task.mode=Mode::Observe;task.phase=Phase::Reconciling;
        task.createdAtMs=task.updatedAtMs=1000;
        if(scenario==12)task.ownerGeneration=143; // Prior restart invalidations, never accepted/executed.
        const auto goal=task.sourceKey,where=" WHERE task_id="+SqlValue(task.id);
        if(scenario!=8) assert(db.Execute("INSERT INTO organic_economy_goal(goal_id,character_guid,state) VALUES("+
            goal+",899,'expired')"));
        assert(db.Write(TaskWrite(task,0,id(20+scenario),"legacy_observed")));
        Task retired;assert(PrepareEconomyObservationRetirement(task,2000,retired));
        const auto plan=TaskWrite(retired,1,id(40+scenario),"observation_retired");
        // Change authority AFTER constructing the optimistic plan. The native
        // transaction must revalidate it, even with an unchanged task revision.
        if(scenario==1 || scenario==11)
            assert(db.Execute("UPDATE organic_economy_goal SET state='active' WHERE goal_id="+goal));
        if(scenario==2) assert(db.Execute("UPDATE living_activity_task SET accepted=1"+where));
        if(scenario==3) assert(db.Execute("UPDATE living_activity_task SET mode='active'"+where));
        if(scenario==9) assert(db.Execute("UPDATE living_activity_task SET owner_generation=1"+where));
        if(scenario==10) assert(db.Execute("UPDATE living_activity_task SET source='profession_job'"+where));
        if(scenario==4 || scenario==5) assert(db.Execute(
            "INSERT INTO living_activity_claim(claim_id,task_id,actor_guid,item_entry,quantity,location,state,revision,updated_at_ms) VALUES("+
            SqlValue(id(60+scenario))+','+SqlValue(task.id)+",899,765,1,'bags',"+
            SqlValue(scenario==4?"proposed":"consumed")+",1,1000)"));
        if(scenario==6) assert(db.Execute(
            "INSERT INTO living_activity_operation(operation_id,task_id,task_revision,kind,request_hash,state,"
            "before_state,after_state,created_at_ms,updated_at_ms) VALUES("+SqlValue(id(66))+','+SqlValue(task.id)+
            ",1,'fixture',REPEAT('a',64),'rejected','{}','{}',1000,1000)"));
        if(scenario==7) {
            auto child=task;child.id=id(67);child.parent=task.id;child.sourceKey="child:53107";
            assert(db.Write(TaskWrite(child,0,id(68),"fixture_child")));
            ++child.revision;child.phase=Phase::Cancelled;
            assert(db.Write(TaskWrite(child,1,id(69),"fixture_child_cancelled")));
        }
        const bool empty= scenario==0 || scenario==8 || scenario==12;
        assert(db.Write(plan)==empty);
        if(empty) {
            assert(db.Write(plan)); // Acknowledgement retry, not a second mutation.
            assert(db.Scalar("SELECT phase FROM living_activity_task"+where)=="cancelled");
            assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition"+where+" AND code='observation_retired'")=="1");
        } else {
            assert(db.Scalar("SELECT phase FROM living_activity_task"+where)=="reconciling");
            assert(!db.ReceiptPresent(plan));
        }
        if(scenario==11) {
            assert(db.Execute("UPDATE organic_economy_goal SET state='expired' WHERE goal_id="+goal));
            assert(!db.Write(plan,true)); // Rollback preserves candidate and source.
            assert(db.Scalar("SELECT phase FROM living_activity_task"+where)=="reconciling");
            assert(db.Write(plan) && db.Write(plan));
        }
        if(scenario!=8) assert(db.Scalar("SELECT COUNT(*) FROM organic_economy_goal WHERE goal_id="+goal)=="1");
    }
}
