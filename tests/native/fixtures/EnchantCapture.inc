namespace {
    void EnchantCaptureCases() {
        CraftIdentity identity{"137e6854-06ea-5e21-8371-6b40c8c19f4e","2c60183b-4a76-41f4-a62e-01d8c65e273d",4,9,{},7443,333};
        identity.world.actor=235;identity.world.policyRevision=1;identity.world.actorGeneration=3;identity.world.mapGeneration=1;
        identity.world.boot="37cfba40-31fe-4664-86db-fb2e87036cb8";
        ProfessionJob job;job.recipe=7443;job.skill=333;job.subjectItem=901;
        job.operation=ProfessionOperation::EnchantItem;job.initialSkill=20;job.targetSkill=21;job.reagents={{10940,2}};
        const EnchantSpec spec{24,0};
        EnchantSubject before{{235,901,2576,1,0,4},{{0,0,0},{7,80,1},{8,0,0}}};
        auto after=before;after.enchantments[0]={24,0,0};
        CraftFrame start{235,20,500,{{235,100,10940,3,0,23}}};
        auto finish=start;finish.skill=21;finish.stacks[0].count=1;
        CraftCapture cast(identity);assert(cast.Start(identity,start));assert(cast.EnterEffect(identity,start));
        assert(cast.Finish(identity,true,finish));const auto observed=*cast.ReadFinished();
        auto verified=VerifyEnchantCapture(identity,job,spec,observed,before,after);
        assert(verified.result==CraftEvidence::Verified && verified.blocker.empty() && verified.gains.empty());
        assert(verified.attempt.subjectItem==901 && verified.attempt.recipe==7443 && verified.attempt.produced.empty());
        assert(verified.attempt.nativeEffectVerified && !verified.attempt.committed && verified.attempt.receipt.id.empty());
        const auto physical=VerifyEnchantResources(235,job,spec,start,finish,before,after);
        assert(physical.result==CraftEvidence::Verified && !physical.attempt.nativeEffectVerified && !physical.attempt.committed);
        auto fail=[&](CraftCaptureResult value,EnchantSubject target,const char* code) {
            const auto r=VerifyEnchantCapture(identity,job,spec,value,before,target);
            assert(r.result==CraftEvidence::Reconciling && r.blocker==code);
            assert(!r.attempt.nativeEffectVerified && !r.attempt.committed && r.gains.empty());
        };
        auto changed=observed;++changed.identity.revision;fail(changed,after,"native_enchant_identity_mismatch");
        changed=observed;++changed.identity.world.mapGeneration;fail(changed,after,"native_enchant_identity_mismatch");
        changed=observed;changed.nativeFinished=false;fail(changed,after,"native_enchant_completion_proof_missing");
        changed=observed;changed.effectEntered=false;fail(changed,after,"native_enchant_effect_callback_missing");
        changed=observed;changed.nativeSucceeded=false;fail(changed,after,"native_enchant_cancelled_after_possible_effect");
        changed=observed;changed.createdCalls=1;fail(changed,after,"native_enchant_unexpected_item_creation");
        changed=observed;changed.createdEntry=10940;fail(changed,after,"native_enchant_unexpected_item_creation");
        changed=observed;changed.createdQuantity=1;fail(changed,after,"native_enchant_unexpected_item_creation");
        changed=observed;--changed.after.money;fail(changed,after,"native_craft_wallet_or_skill_changed_unexpectedly");
        changed=observed;changed.after.skill=19;fail(changed,after,"native_craft_wallet_or_skill_changed_unexpectedly");
        changed=observed;++changed.after.stacks[0].count;fail(changed,after,"native_craft_consumption_mismatch");
        changed=observed;++changed.after.stacks[0].guid;fail(changed,after,"native_craft_input_identity_changed");
        changed=observed;changed.after.stacks[0].slot=24;fail(changed,after,"native_craft_input_identity_changed");
        changed=observed;changed.after.stacks[0].guid=901;fail(changed,after,"native_enchant_subject_input_collision");
        auto target=after;target.item.slot=23;fail(observed,target,"native_enchant_subject_identity_changed");
        target=after;target.item.guid=902;fail(observed,target,"native_enchant_completion_proof_missing");
        target=after;target.item.actor=236;fail(observed,target,"native_enchant_completion_proof_missing");
        target=after;target.item.count=2;fail(observed,target,"native_enchant_completion_proof_missing");
        target=after;target.enchantments[0].id=99;fail(observed,target,"native_enchant_expected_effect_missing");
        target=after;target.enchantments[0].duration=60;fail(observed,target,"native_enchant_expected_effect_missing");
        target=after;target.enchantments[0].charges=1;fail(observed,target,"native_enchant_expected_effect_missing");
        target=after;++target.enchantments[1].id;fail(observed,target,"native_enchant_unrelated_effect_changed");
        target=after;target.enchantments.pop_back();fail(observed,target,"native_enchant_subject_identity_changed");
        target=after;target.enchantments.resize(17);fail(observed,target,"native_enchant_completion_proof_missing");
        // A native enchant with no skill gain is an operation, not a skill-goal completion.
        changed=observed;changed.after.skill=20;
        assert(VerifyEnchantCapture(identity,job,spec,changed,before,after).result==CraftEvidence::Verified);
        // Reapplying an enchant for skill practice is valid; unchanged equipment
        // cannot independently satisfy an equipment-improvement purpose.
        assert(VerifyEnchantCapture(identity,job,spec,observed,after,after).result==CraftEvidence::Verified);
        auto equipment=job;equipment.purpose=ProfessionPurpose::Equipment;equipment.targetSkill=0;
        assert(VerifyEnchantResources(235,equipment,spec,start,finish,after,after).blocker=="native_enchant_equipment_unchanged");
        CraftCapture cancelled(identity);assert(cancelled.Start(identity,start));assert(cancelled.Finish(identity,false,start));
        assert(VerifyEnchantCapture(identity,job,spec,*cancelled.ReadFinished(),before,before).result==CraftEvidence::RejectedWithoutEffect);
        fail(*cancelled.ReadFinished(),after,"native_enchant_cancelled_after_possible_effect");
        // Wholly consumed stacks disappear; partial stacks retain identity.
        changed=observed;changed.before.stacks[0].count=2;changed.after.stacks.clear();
        assert(VerifyEnchantCapture(identity,job,spec,changed,before,after).result==CraftEvidence::Verified);
        changed=observed;changed.before.stacks.push_back(changed.before.stacks[0]);
        fail(changed,after,"native_enchant_completion_proof_missing");
        std::string blocker;
        assert(!VerifyCraftReagents(235,{{10940,2},{10940,2}},start,finish,blocker));
        assert(!VerifyCraftReagents(235,{{10940,0}},start,finish,blocker));
        assert(!VerifyCraftReagents(235,{},start,finish,blocker));
    }
}
