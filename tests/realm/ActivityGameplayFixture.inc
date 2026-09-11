// Built only into a marked copied realm. Native spell progression continues;
// only one bot's competing AI decisions are held for at most fifteen seconds.
// A known direct heal or learned druid form is used. No spell/talent/item grant.
// Fixture health setup/restoration is NOT organic gameplay/economy evidence.
bool LivingActivityCoordinator::IsolatedGameplayActor(uint32_t actor) const {
    return actor && state->gameplayFixtureActor.load(std::memory_order_acquire) == actor;
}

void LivingActivityCoordinator::ObserveIsolatedGameplayPacket(PlayerbotAI& ai, const WorldPacket& packet) {
    if (!IsolatedGameplayActor(ai.GetBot()->GetGUIDLow())) return;
    if (packet.GetOpcode() != SMSG_SPELLHEALLOG && packet.GetOpcode() != SMSG_SPELL_FAILURE &&
        packet.GetOpcode() != SMSG_CAST_RESULT) return;
    // Pinned TBC Unit::SendHealSpellLog writes packed victim, packed caster,
    // spell, amount, critical, unused, AFTER DealHeal modifies native health.
    // Observe only; never synthesize or submit a packet.
    try {
        WorldPacket copy(packet); copy.rpos(0);
        if (packet.GetOpcode() == SMSG_CAST_RESULT) {
            uint32 spell; uint8 result;
            copy >> spell >> result;
            if (spell == state->gameplayFixtureSpell.load(std::memory_order_acquire))
                state->gameplayCastResult.store(uint32(result) + 1, std::memory_order_release);
            return;
        }
        if (packet.GetOpcode() == SMSG_SPELL_FAILURE) {
            ObjectGuid caster; uint32 spell; uint8 result;
            copy >> caster.ReadAsPacked() >> spell >> result;
            if (caster == ai.GetBot()->GetObjectGuid() && spell == state->gameplayFixtureSpell.load(std::memory_order_acquire))
                state->gameplayCastFailure.store(uint32(result) + 1, std::memory_order_release);
            return;
        }
        ObjectGuid victim, caster; uint32 spell, amount; uint8 critical, unused;
        copy >> victim.ReadAsPacked() >> caster.ReadAsPacked() >> spell >> amount >> critical >> unused;
        if (copy.rpos() != copy.size() || victim != ai.GetBot()->GetObjectGuid() || caster != victim || !amount ||
            spell != state->gameplayFixtureSpell.load(std::memory_order_acquire)) return;
        state->gameplayHealAmount.store(amount, std::memory_order_release);
        state->gameplayHealPackets.fetch_add(1, std::memory_order_acq_rel);
    } catch (...) { /* Invalid native packet cannot become test success. */ }
}

void LivingActivityCoordinator::RunIsolatedGameplayFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished) return;
    RequireIsolatedNativeFixtureEnvironment();
    const std::string suite = std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    const bool support = suite == "activity-support-v1";
    const uint32 actor = state->gameplayFixtureActor.load(std::memory_order_acquire);
    auto finish = [&](bool passed, const std::string& failure) {
        const uint32 selected = state->gameplayFixtureActor.load(std::memory_order_acquire);
        auto* bot = selected ? sRandomPlayerbotMgr.GetPlayerBot(selected) : nullptr;
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        const uint32 spell = state->gameplayFixtureSpell.load(std::memory_order_acquire);
        boost::property_tree::ptree report;
        report.put("suite", suite); report.put("boot", state->boot);
        report.put("test_only", true); report.put("fixture_health_setup", selected != 0 && !support);
        report.put("actor", selected); report.put(support ? "known_form_spell" : "known_healing_spell", spell);
        if (support) {
            report.put("native_form_aura", bot && bot->HasAura(spell));
            report.put("native_form",bot ? uint32(bot->GetShapeshiftForm()) : 0);
            report.put("fixture_existing_aura_removed",false); // Selected only absent forms.
        }
        report.put("native_heal_packets", state->gameplayHealPackets.load(std::memory_order_acquire));
        report.put("native_heal_amount", state->gameplayHealAmount.load(std::memory_order_acquire));
        report.put("simulated_pending_atomic_receipt", !state->gameplayAtomicFixture.empty());
        report.put("health_before", state->gameplaySetupHealth);
        report.put("health_after", bot ? bot->GetHealth() : 0);
        report.put("mana_before", state->gameplayOriginalMana);
        report.put("mana_after", bot ? bot->GetPower(POWER_MANA) : 0);
        report.put("minimum_mana_observed", state->gameplayMinimumMana);
        report.put("native_failure_code_plus_one", state->gameplayCastFailure.load(std::memory_order_acquire));
        report.put("native_cast_result_plus_one", state->gameplayCastResult.load(std::memory_order_acquire));
        report.add_child("native_cast_trace", state->gameplayTrace);
        bool restored = selected == 0;
        if (bot && ai && ai->GetActivityActorEpoch() == state->gameplayContext.actorGeneration) {
            if (const auto* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL))
                if (cast->m_spellInfo->Id == spell) bot->InterruptSpell(CURRENT_GENERIC_SPELL, false);
            if (support) {
                bot->RemoveAurasDueToSpell(spell); // Only the fixture's previously absent form.
                bot->SetPower(POWER_RAGE,state->gameplayOriginalRage);
                bot->SetPower(POWER_ENERGY,state->gameplayOriginalEnergy);
            }
            bot->SetHealth(state->gameplayOriginalHealth);
            bot->SetPower(POWER_MANA, state->gameplayOriginalMana);
            bot->SetSelectionGuid(state->gameplayOriginalSelection);
            ai->aiInternalUpdateDelay = state->gameplayOriginalDelay;
            restored = bot->GetHealth() == state->gameplayOriginalHealth &&
                bot->GetPower(POWER_MANA) == state->gameplayOriginalMana &&
                bot->GetMoney() == state->gameplayOriginalMoney;
            if (support) restored = restored && bot->GetShapeshiftForm() == FORM_NONE && !bot->HasAura(spell) &&
                bot->GetPower(POWER_RAGE) == state->gameplayOriginalRage &&
                bot->GetPower(POWER_ENERGY) == state->gameplayOriginalEnergy;
        }
        if (state->gameplayLease.actor) {
            if (!state->gameplayAtomicFixture.empty())
                restored = state->authority.FinishAtomic(state->gameplayLease, state->gameplayAtomicFixture).code == AuthorityCode::Allowed && restored;
            restored = state->authority.Release(state->gameplayLease).code == AuthorityCode::Released && restored;
            const auto binding = state->bindings.find(selected);
            if (binding != state->bindings.end()) binding->second.publisher.Publish(state->authority.Read(selected));
        }
        state->gameplayFixtureActor.store(0, std::memory_order_release);
        state->fixtureFinished = true;
        report.put("fixture_state_restored", restored);
        report.put("outcome", passed && restored ? "passed" : "failed"); report.put("failure", failure);
        report.put("native_inventory_operations", 0); report.put("persistent_task_completions", 0);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        std::ofstream evidence("/isolated/evidence/" + (support ? std::string("activity-support-") : std::string("activity-gameplay-")) +
            state->boot + ".json");
        evidence << Json(report); evidence.close();
        if (!evidence) sLog.outError("Living isolated gameplay evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
    };
    if (actor) {
        auto* bot = sRandomPlayerbotMgr.GetPlayerBot(actor);
        if (sWorld.GetActiveAndQueuedSessionCount() || !bot || !bot->GetPlayerbotAI() ||
            !(ReadNativeContext(*bot, state->policyRevision, state->boot) == state->gameplayContext) || NativeSafety(bot)) {
            finish(false, "fixture_native_context_or_safety_changed"); return;
        }
        state->gameplayMinimumMana = std::min<uint32>(state->gameplayMinimumMana, bot->GetPower(POWER_MANA));
        if (NowMs() >= state->gameplayNextSample && state->gameplayTrace.size() < 20) {
            state->gameplayNextSample = NowMs() + 250;
            boost::property_tree::ptree sample;
            sample.put("elapsed_ms", NowMs() + 15000 - state->gameplayDeadline);
            sample.put("health", bot->GetHealth()); sample.put("mana", bot->GetPower(POWER_MANA));
            sample.put("stopped", bot->IsStopped());
            sample.put("movement_type", bot->GetMotionMaster()->GetCurrentMovementGeneratorType());
            if (support) sample.put("native_form",uint32(bot->GetShapeshiftForm()));
            if (const auto* cast = bot->GetCurrentSpell(CURRENT_GENERIC_SPELL)) {
                sample.put("spell", cast->m_spellInfo->Id); sample.put("state", cast->getState());
                sample.put("casted_ms", cast->GetCastedTime());
            } else sample.put("spell", 0);
            state->gameplayTrace.push_back({"", sample});
        }
        const bool effectObserved = support ?
            (bot->HasAura(state->gameplayFixtureSpell.load(std::memory_order_acquire)) && bot->GetShapeshiftForm() != FORM_NONE) :
            (state->gameplayHealPackets.load(std::memory_order_acquire) == 1 && bot->GetHealth() > state->gameplaySetupHealth);
        if (effectObserved && !bot->GetCurrentSpell(CURRENT_GENERIC_SPELL) &&
            state->gameplayMinimumMana < state->gameplayOriginalMana) {
            finish(true, ""); return;
        }
        if (NowMs() > state->gameplayDeadline)
            finish(false, support ? "native_form_result_not_observed" : "native_healing_result_not_observed");
        return;
    }
    if (sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty() || NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    if (!state->gameplayDeadline) state->gameplayDeadline = NowMs() + 60000;
    if (NowMs() > state->gameplayDeadline) {
        finish(false, support ? "no_safely_available_native_form_caster" : "no_safely_available_native_healer"); return;
    }
    for (auto& binding : state->bindings) {
        auto* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        if (!ai || !bot->IsInWorld() || bot->GetGroup() || NativeSafety(bot) || !bot->IsStopped() ||
            bot->IsMounted() || bot->getStandState() != UNIT_STAND_STATE_STAND ||
            bot->IsNonMeleeSpellCasted(true) || bot->GetHealth() < 4 ||
            state->authority.Read(binding.first).lease.actor) continue;
        if (support && (bot->getClass() != CLASS_DRUID || bot->GetShapeshiftForm() != FORM_NONE)) continue;
        uint32 spellId = 0;
        for (const auto& known : bot->GetSpellMap()) {
            if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled) continue;
            const auto* spell = sServerFacade.LookupSpellInfo(known.first);
            if (!spell) continue;
            bool directHeal = false, form = false, other = false, reagent = false;
            for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i) {
                directHeal = directHeal || spell->Effect[i] == SPELL_EFFECT_HEAL;
                form = form || (spell->Effect[i] == SPELL_EFFECT_APPLY_AURA && spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SHAPESHIFT);
                other = other || (spell->Effect[i] && spell->Effect[i] != (support ? SPELL_EFFECT_APPLY_AURA : SPELL_EFFECT_HEAL));
            }
            for (uint8 i = 0; i < MAX_SPELL_REAGENTS; ++i) reagent = reagent || spell->Reagent[i] > 0;
            if ((support ? (form && !bot->HasAura(known.first)) : directHeal) && !other && !reagent && spell->powerType == POWER_MANA &&
                (spell->manaCost || spell->ManaCostPercentage) && NativeSpellEffects(*ai, known.first).mask == SpellEffectMask(true) &&
                NativeSpellPermit(*ai, known.first, bot).validated) {
                spellId = known.first; break;
            }
        }
        if (!spellId) continue;
        const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
        Task task; task.id = task.root = NewId(); task.actor = binding.first; task.context = current;
        task.source = "isolated_gameplay_fixture"; task.sourceKey = state->boot;
        task.mode = Mode::Active; task.phase = Phase::Traveling; task.kind = Kind::Maintenance;
        task.priority = Priority::Optional;
        task.checkpoint.step = support ? "native_form_with_other_owner" : "native_heal_with_other_owner";
        task.createdAtMs = task.updatedAtMs = NowMs();
        const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        state->authority.Observe(current, 0);
        const auto grant = state->authority.Acquire(task, Mask(Effect::TravelTarget), monotonic, 30000);
        if (!grant.Granted()) continue;
        binding.second.publisher.Publish(state->authority.Read(binding.first));
        state->gameplayContext = current; state->gameplayLease = grant.lease;
        state->gameplayOriginalHealth = bot->GetHealth(); state->gameplaySetupHealth = support ? bot->GetHealth() : bot->GetHealth() / 2;
        state->gameplayOriginalMana = bot->GetPower(POWER_MANA); state->gameplayOriginalMoney = bot->GetMoney();
        state->gameplayOriginalRage = bot->GetPower(POWER_RAGE);
        state->gameplayOriginalEnergy = bot->GetPower(POWER_ENERGY);
        state->gameplayMinimumMana = state->gameplayOriginalMana;
        state->gameplayOriginalSelection = bot->GetSelectionGuid(); state->gameplayOriginalDelay = ai->aiInternalUpdateDelay;
        state->gameplayDeadline = NowMs() + 15000;
        state->gameplayFixtureSpell.store(spellId, std::memory_order_release);
        state->gameplayFixtureActor.store(binding.first, std::memory_order_release);
        bot->SetHealth(state->gameplaySetupHealth);
        // Test-only fault injection: a held native operation receipt, not an
        // actual purchase or a fabricated operation journal completion.
        state->gameplayAtomicFixture = NewId();
        if (state->authority.BeginAtomic(grant.lease, state->gameplayAtomicFixture, monotonic).code != AuthorityCode::Allowed) {
            finish(false, "simulated_atomic_hold_rejected"); return;
        }
        binding.second.publisher.Publish(state->authority.Read(binding.first));
        const bool original = state->enforceEffects.load(std::memory_order_acquire);
        bool started = false;
        {
            struct Restore { State& state; bool value; ~Restore() { state.enforceEffects.store(value, std::memory_order_release); } } restore{*state, original};
            state->enforceEffects.store(true, std::memory_order_release);
            try { started = ai->CastSpell(spellId, bot); }
            catch (...) { started = false; }
        }
        if (!started) finish(false, support ? "native_form_cast_rejected" : "native_healing_cast_rejected");
        return;
    }
}
