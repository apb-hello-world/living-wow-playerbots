// Explicit source fixture within the existing finite service observation.
// Supplies ONE native corpse, never loot, skill, XP, gold or task completion.
// Does not prove hunting/killing; normal native Skinning still rolls its loot.
template<class FixtureState>
void PrepareIsolatedSkinningSource(FixtureState& state) {
    const auto* selected=std::getenv("LIVING_WOW_NATIVE_FIXTURE_TASK");
    if(!selected || std::string(selected)!="skinning-source-v1")return;
    RequireIsolatedNativeFixtureEnvironment();
    auto& setup=state.guildDepositFixture.setup;
    if(setup.template get<bool>("skinning_source_attempted",false))return;
    const auto* explicitActor=std::getenv("LIVING_WOW_NATIVE_FIXTURE_ACTOR");
    if(!explicitActor || std::string(explicitActor)!="377")throw std::runtime_error("explicit_skinning_actor_required");
    auto* bot=sRandomPlayerbotMgr.GetPlayerBot(377);
    if(!bot || !bot->IsInWorld() || !bot->GetMap() || !bot->GetPlayerbotAI())return;
    if(NativeSafety(bot) || LivingServiceExecution::Busy(bot) || bot->GetGroup() || bot->GetTradeData() ||
        bot->GetMap()->IsDungeon() || !state.bindings.count(377))return;
    if(!bot->GetSession() || bot->isRealPlayer() || !sPlayerbotAIConfig.IsInRandomAccountList(bot->GetSession()->GetAccountId()) ||
        bot->GetGuildId()!=4 || !bot->HasSpell(8613) || !bot->HasItemCount(7005,1) || bot->GetItemCount(2318,true) ||
        bot->GetSkillValue(SKILL_SKINNING)<10 || bot->GetSkillMax(SKILL_SKINNING)<bot->GetSkillValue(SKILL_SKINNING))
        throw std::runtime_error("existing_native_skinner_and_empty_output_required");
    for(const auto& row:state.cache)if(row.second.actor==377 && row.second.mode==Mode::Active && row.second.accepted && !Terminal(row.second.phase))
        throw std::runtime_error("preserve_existing_skinner_obligation");
    std::ifstream existing("/isolated/evidence/skinning-source-intent-v1.json");
    if(existing)throw std::runtime_error("preserve_prior_skinning_source_fixture");
    const auto* creature=sObjectMgr.GetCreatureTemplate(119); // Longsnout, native level10-11 skinning table.
    if(!creature || !creature->SkinningLootId || creature->GetRequiredLootSkill()!=SKILL_SKINNING)
        throw std::runtime_error("native_skinning_template_required");
    setup.put("skinning_source_attempted",true);setup.put("source_fixture_boot",state.boot);
    setup.put("source_fixture_actor",377);setup.put("source_fixture_entry",119);
    setup.put("source_fixture_at_ms",NowMs());setup.put("source_fixture_goal","isolated_requested_skinning_v1");
    setup.put("source_fixture_money_before",bot->GetMoney());setup.put("source_fixture_xp_before",bot->GetUInt32Value(PLAYER_XP));
    setup.put("source_fixture_skill_before",bot->GetSkillValuePure(SKILL_SKINNING));
    setup.put("source_fixture_stops",1);setup.put("source_fixture_relocations",0);
    setup.put("source_fixture_item_grants",0);setup.put("source_fixture_skill_grants",0);
    setup.put("source_fixture_corpse_grants",1);setup.put("source_fixture_native_kill_proof",false);
    std::ofstream intent("/isolated/evidence/skinning-source-intent-v1.json");intent<<Json(setup);intent.close();
    if(!intent)throw std::runtime_error("skinning_source_intent_save_failed");
    bot->StopMoving();
    auto* corpse=bot->SummonCreature(119,bot->GetPositionX(),bot->GetPositionY(),bot->GetPositionZ(),
        bot->GetOrientation(),TEMPSPAWN_TIMED_DESPAWN,600000);
    if(!corpse)throw std::runtime_error("skinning_source_spawn_failed");
    corpse->SetDeathState(JUST_DIED);corpse->SetLootRecipient(bot);
    corpse->SetLootStatus(CREATURE_LOOT_STATUS_LOOTED);
    setup.put("source_fixture_guid",corpse->GetObjectGuid().GetRawValue());
    setup.put("source_fixture_level",corpse->GetLevel());setup.put("source_fixture_has_loot",corpse->m_loot!=nullptr);
    if(corpse->IsAlive() || corpse->m_loot || !corpse->HasFlag(UNIT_FIELD_FLAGS,UNIT_FLAG_SKINNABLE) ||
        bot->GetMoney()!=setup.template get<uint32_t>("source_fixture_money_before") ||
        bot->GetUInt32Value(PLAYER_XP)!=setup.template get<uint32_t>("source_fixture_xp_before"))
        throw std::runtime_error("skinning_fixture_native_source_invalid");
    std::ofstream record("/isolated/evidence/skinning-source-setup-v1.json");record<<Json(setup);record.close();
    if(!record)throw std::runtime_error("skinning_source_setup_save_failed");
    // Explicit native admission prevents an ordinary opportunistic skin from
    // racing the scenario before the durable owner exists. No task is injected.
    bot->GetPlayerbotAI()->GetAiObjectContext()->ClearValues("nearest corpses");
    setup.put("source_fixture_admission_calls",1);
    const auto admitted=sLivingActivityCoordinator.AdmitGuildProcurement(377,4,"isolated_requested_skinning_v1",2318);
    setup.put("source_fixture_task",admitted.task);
    setup.put("source_fixture_admission_blocker",admitted.blocker);
    std::ofstream admission("/isolated/evidence/skinning-source-admission-v1.json");admission<<Json(setup);admission.close();
    if(!admission || (admitted.code!=AdmissionCode::Pending && admitted.code!=AdmissionCode::Saved))
        throw std::runtime_error("skinning_source_admission:"+admitted.blocker);
}
