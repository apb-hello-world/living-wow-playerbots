// Marked copied realm only. ONE instant, already-known native pet attack in an
// existing encounter. No enemy, spell, money or item is supplied. The native
// damage and resource expenditure remain normal copied-world effects; only
// the simulated noncombat owner and packet observation are removed afterwards.
#include "Entities/Pet.h"

void LivingActivityCoordinator::RunIsolatedPetFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    RequireIsolatedNativeFixtureEnvironment();
    state->fixtureNext = NowMs() + 1000;
    if (sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty()) return;
    if (!state->petFixtureDeadline) state->petFixtureDeadline = NowMs() + 90000;
    auto emit = [&](boost::property_tree::ptree& report) {
        report.put("suite", "activity-pet-v1"); report.put("boot", state->boot);
        report.put("test_only", true); report.put("native_inventory_operations", 0);
        report.put("persistent_task_completions", 0); report.put("manufactured_encounter", false);
        report.put("fixture_spells_granted", 0); report.put("native_effects_rewound", false);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        report.add_child("candidate_diagnostics", state->petFixtureCandidates);
        std::ofstream evidence("/isolated/evidence/activity-pet-" + state->boot + ".json");
        evidence << Json(report); evidence.close(); state->fixtureFinished = true;
        if (!evidence) sLog.outError("Living isolated pet evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
    };
    if (NowMs() > state->petFixtureDeadline) {
        boost::property_tree::ptree report;
        report.put("outcome", "blocked"); report.put("failure", "no_safe_existing_instant_pet_attack");
        emit(report); return;
    }
    for (auto& binding : state->bindings) {
        auto* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        auto* pet = bot ? bot->GetPet() : nullptr;
        if (!ai || !pet) continue;
        const auto roster = NativePartyProtection(*bot);
        auto* enemy = pet->GetVictim();
        auto candidate = [&](const char* reason) {
            const auto key = std::to_string(binding.first);
            if (!state->petFixtureCandidates.get_child_optional(key) && state->petFixtureCandidates.size() >= 50) return;
            boost::property_tree::ptree detail;
            detail.put("reason", reason); detail.put("observed_at_ms", NowMs());
            detail.put("pet_entry", pet->GetEntry()); detail.put("roster", Name(roster));
            detail.put("native_safety", NativeSafety(bot)); detail.put("pet_has_victim", enemy != nullptr);
            state->petFixtureCandidates.put_child(key, detail);
        };
        if (!bot->IsInWorld() || !bot->IsAlive() || !pet->IsInWorld() || !pet->IsAlive() ||
            ai->GetActivityActorEpoch() != binding.second.actorEpoch ||
            bot->GetMapId() != pet->GetMapId() || bot->GetInstanceId() != pet->GetInstanceId() ||
            pet->GetOwnerGuid() != bot->GetObjectGuid()) { candidate("native_actor_or_pet_not_ready"); continue; }
        if (roster != PartyProtection::None && roster != PartyProtection::BotOnly) { candidate("native_party_protected"); continue; }
        if (NativeSafety(bot) != uint32_t(Safety::Combat) || bot->IsMounted() ||
            bot->IsNonMeleeSpellCasted(true) || pet->IsNonMeleeSpellCasted(true) ||
            state->authority.Read(binding.first).lease.actor) { candidate("native_safety_or_commitment"); continue; }
        if (!enemy || !enemy->IsInWorld() || !enemy->IsAlive() ||
            !sServerFacade.IsHostileTo(bot, enemy) || !pet->CanReachWithMeleeAttack(enemy) ||
            !NativeEngagedAttackPermit(*ai, enemy).validated) { candidate("no_existing_native_pet_encounter"); continue; }
        auto* travel = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
        if (!travel) { candidate("native_travel_value_missing"); continue; }
        uint32 spellId = 0;
        for (const auto& known : pet->m_spells) {
            if (known.second.state == PETSPELL_REMOVED) continue;
            const auto* info = sServerFacade.LookupSpellInfo(known.first);
            if (!info || IsPassiveSpell(info) || IsPositiveSpell(info) || info->speed != 0 ||
                GetSpellCastTime(info, pet) != 0 || !(info->manaCost || info->ManaCostPercentage) ||
                !HasOnlyNativeEffects(*info, [](uint32 effect) { return effect == SPELL_EFFECT_SCHOOL_DAMAGE; }) ||
                NativeSpellEffects(*ai, known.first).mask != SpellEffectMask(true) ||
                !NativePetSpellPermit(*ai, known.first, enemy).validated) continue;
            // Require actual CheckCast success, not the normal pet handler's
            // path-to-target fallback. This fixture must complete synchronously.
            Spell probe(pet, info, TRIGGERED_NORMAL_COMBAT_CAST | TRIGGERED_PET_CAST);
            probe.m_targets.setUnitTarget(enemy);
            if (probe.CheckCast(true) != SPELL_CAST_OK) continue;
            spellId = known.first; break;
        }
        if (!spellId) { candidate("no_known_instant_damage_spell_ready"); continue; }
        candidate("native_pet_candidate_validated");
        const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
        Task task; task.id = task.root = NewId(); task.actor = binding.first; task.context = current;
        task.source = "isolated_pet_fixture"; task.sourceKey = state->boot;
        task.mode = Mode::Active; task.phase = Phase::Traveling; task.kind = Kind::Maintenance;
        task.priority = Priority::Optional; task.checkpoint.step = "native_pet_with_other_owner";
        task.createdAtMs = task.updatedAtMs = NowMs();
        const auto monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool original = state->enforceEffects.load(std::memory_order_acquire);
        const auto originalSelection = bot->GetSelectionGuid();
        const auto oldRelevance = travel->GetRelevance(); const bool oldForced = travel->IsForced();
        const auto originalMoney = bot->GetMoney();
        const auto power = pet->GetPowerType(); const auto resourceBefore = pet->GetPower(power);
        const auto enemyHealthBefore = enemy->GetHealth();
        boost::property_tree::ptree report;
        report.put("actor", binding.first); report.put("pet_guid", pet->GetObjectGuid().GetRawValue());
        report.put("enemy_guid", enemy->GetObjectGuid().GetRawValue()); report.put("known_spell", spellId);
        report.put("simulated_prior_noncombat_owner", true); report.put("resource_type", uint32(power));
        report.put("resource_before", resourceBefore); report.put("enemy_health_before", enemyHealthBefore);
        bool submitted = false, preserved = false;
        struct Restore { std::function<void()> fn; ~Restore() { fn(); } } restore{[&] {
            state->enforceEffects.store(false, std::memory_order_release);
            state->gameplayFixtureActor.store(0, std::memory_order_release);
            state->petFixtureCaster.store(0, std::memory_order_release);
            state->petFixtureTarget.store(0, std::memory_order_release);
            state->authority.Forget(binding.first); state->authority.Observe(current, NativeSafety(bot));
            binding.second.publisher.Publish(state->authority.Read(binding.first));
            state->enforceEffects.store(original, std::memory_order_release);
        }};
        try {
            // A previously held ordinary lease is paused by combat. It is not
            // permission to admit a real maintenance job during this encounter.
            state->authority.Observe(current, 0);
            const auto grant = state->authority.Acquire(task, Mask(Effect::TravelTarget), monotonic, 30000);
            if (!grant.Granted()) throw std::runtime_error("fixture_prior_owner_rejected");
            state->authority.Observe(current, NativeSafety(bot));
            binding.second.publisher.Publish(state->authority.Read(binding.first));
            state->gameplayFixtureSpell.store(spellId, std::memory_order_release);
            state->petFixtureCaster.store(pet->GetObjectGuid().GetRawValue(), std::memory_order_release);
            state->petFixtureTarget.store(enemy->GetObjectGuid().GetRawValue(), std::memory_order_release);
            state->gameplayFixtureActor.store(binding.first, std::memory_order_release);
            state->enforceEffects.store(true, std::memory_order_release);
            submitted = ai->CastPetSpell(spellId, enemy); // Native HandlePetAction and SpellStart.
            preserved = SameLease(state->authority.Read(binding.first).lease, grant.lease) &&
                travel->GetRelevance() == oldRelevance && travel->IsForced() == oldForced &&
                bot->GetSelectionGuid() == originalSelection && bot->GetMoney() == originalMoney;
        } catch (const std::exception& error) { report.put("failure", error.what()); }
        const auto packets = state->petFixtureDamagePackets.load(std::memory_order_acquire);
        const auto damage = state->petFixtureDamage.load(std::memory_order_acquire);
        const auto resourceAfter = pet->GetPower(power), enemyHealthAfter = enemy->GetHealth();
        restore.fn(); restore.fn = [] {};
        const bool restored = !state->authority.Read(binding.first).lease.actor &&
            !state->gameplayFixtureActor.load(std::memory_order_acquire);
        report.put("native_command_submitted", submitted); report.put("native_damage_packets", packets);
        report.put("native_damage", damage); report.put("resource_after", resourceAfter);
        report.put("enemy_health_after", enemyHealthAfter); report.put("owned_travel_preserved", preserved);
        report.put("fixture_state_restored", restored);
        const bool proved = submitted && packets == 1 && damage > 0 && resourceAfter < resourceBefore &&
            enemyHealthBefore > enemyHealthAfter && enemyHealthBefore - enemyHealthAfter == damage;
        report.put("outcome", proved && preserved && restored ? "passed" : "failed");
        if (!proved && !report.get_optional<std::string>("failure"))
            report.put("failure", "native_instant_pet_result_not_proven");
        emit(report); return;
    }
}
