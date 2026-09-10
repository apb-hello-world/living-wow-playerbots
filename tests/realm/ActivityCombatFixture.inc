// Copied realm only. Re-engage ONE bot's existing native enemy synchronously
// before map updates, while a simulated prior noncombat commitment owns travel.
// No enemy, item, spell, stat, reward or persistent task is manufactured. This
// proves native attack execution, NOT rotations, damage or dungeon completion.
#include "playerbot/strategy/actions/AttackAction.h"
#include "playerbot/LootObjectStack.h"

namespace {
    class NativeAttackReplay final : public ai::AttackAction {
    public:
        explicit NativeAttackReplay(PlayerbotAI* ai) : AttackAction(ai, "isolated_native_attack") {}
        bool Reengage(Unit* target) { return Attack(nullptr, target); }
    };
}

void LivingActivityCoordinator::RunIsolatedCombatFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    RequireIsolatedNativeFixtureEnvironment();
    state->fixtureNext = NowMs() + 1000;
    if (sWorld.GetActiveAndQueuedSessionCount() || !state->loaded || state->ioPending ||
        !state->pending.empty() || !state->incoming.empty()) return;
    if (!state->combatFixtureDeadline) state->combatFixtureDeadline = NowMs() + 90000;
    auto emit = [&](boost::property_tree::ptree& report) {
        report.put("suite", "activity-combat-v1"); report.put("boot", state->boot);
        report.put("test_only", true); report.put("native_inventory_operations", 0);
        report.put("persistent_task_completions", 0); report.put("manufactured_encounter", false);
        report.put("damage_or_rotation_proof", false);
        report.put("general_execution_enabled", state->enforceEffects.load(std::memory_order_acquire));
        std::ofstream evidence("/isolated/evidence/activity-combat-" + state->boot + ".json");
        evidence << Json(report); evidence.close();
        state->fixtureFinished = true;
        if (!evidence) sLog.outError("Living isolated combat evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
    };
    if (NowMs() > state->combatFixtureDeadline) {
        boost::property_tree::ptree report;
        report.put("outcome", "blocked"); report.put("failure", "no_safely_observable_existing_combat");
        emit(report); return;
    }
    for (auto& binding : state->bindings) {
        auto* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        auto* enemy = bot ? bot->GetVictim() : nullptr;
        if (!ai || !enemy || bot->GetPet() || bot->GetGroup() || bot->IsMounted() ||
            ai->GetActivityActorEpoch() != binding.second.actorEpoch ||
            !bot->IsStopped() || !bot->IsInWorld() || !bot->IsAlive() ||
            !ai->IsStateActive(BotState::BOT_STATE_COMBAT) || bot->IsNonMeleeSpellCasted(true) ||
            bot->GetCurrentSpell(CURRENT_MELEE_SPELL) ||
            NativeSafety(bot) != uint32_t(Safety::Combat) || enemy->GetVictim() != bot ||
            !enemy->IsInWorld() || !enemy->IsAlive() ||
            !sServerFacade.IsHostileTo(bot, enemy) || !bot->CanReachWithMeleeAttack(enemy) ||
            ai->HasStrategy("stealthed", BotState::BOT_STATE_COMBAT) ||
            state->authority.Read(binding.first).lease.actor ||
            sPlayerbotRendezvousManager.GetPartyActivityOwner(binding.first) !=
                PlayerbotRendezvousManager::PartyActivityOwner::none) continue;
        auto* context = ai->GetAiObjectContext();
        auto* target = context->GetValue<ai::TravelTarget*>("travel target")->Get();
        auto* loot = context->GetValue<ai::LootObjectStack*>("available loot")->Get();
        if (!target || !loot) continue;
        const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
        const auto oldSelection = bot->GetSelectionGuid();
        const auto oldCurrent = context->GetValue<Unit*>("current target")->Get();
        const auto oldTarget = context->GetValue<Unit*>("old target")->Get();
        const auto oldLoot = *loot;
        const auto oldMainTimer = bot->getAttackTimer(BASE_ATTACK), oldOffTimer = bot->getAttackTimer(OFF_ATTACK);
        const auto oldHealth = bot->GetHealth(), oldEnemyHealth = enemy->GetHealth();
        const auto oldMana = bot->GetPower(POWER_MANA), oldDelay = ai->aiInternalUpdateDelay;
        const auto oldMoney = bot->GetMoney(), oldRelevance = target->GetRelevance();
        const bool oldForced = target->IsForced();
        const float oldOrientation = bot->GetOrientation();
        const bool originalEnforcement = state->enforceEffects.load(std::memory_order_acquire);
        const bool oldMelee = bot->hasUnitState(UNIT_STAT_MELEE_ATTACKING);
        boost::property_tree::ptree report;
        report.put("actor", binding.first); report.put("enemy_guid", enemy->GetObjectGuid().GetRawValue());
        report.put("simulated_prior_noncombat_owner", true);
        struct Restore { std::function<void()> fn; ~Restore() { fn(); } } restore{[&] {
            state->enforceEffects.store(false, std::memory_order_release);
            bot->Attack(enemy, oldMelee);
            bot->SetSelectionGuid(oldSelection); bot->SetOrientation(oldOrientation);
            context->GetValue<Unit*>("current target")->Set(oldCurrent);
            context->GetValue<Unit*>("old target")->Set(oldTarget);
            *loot = oldLoot;
            target->SetRelevance(oldRelevance); target->SetForced(oldForced);
            bot->setAttackTimer(BASE_ATTACK, oldMainTimer); bot->setAttackTimer(OFF_ATTACK, oldOffTimer);
            ai->aiInternalUpdateDelay = oldDelay;
            state->authority.Forget(binding.first); state->authority.Observe(current, NativeSafety(bot));
            binding.second.publisher.Publish(state->authority.Read(binding.first));
            state->enforceEffects.store(originalEnforcement, std::memory_order_release);
        }};
        bool started = false, relations = false, ownedTargetPreserved = false;
        try {
            Task task; task.id = task.root = NewId(); task.actor = binding.first; task.context = current;
            task.source = "isolated_combat_fixture"; task.sourceKey = state->boot;
            task.mode = Mode::Active; task.phase = Phase::Traveling; task.kind = Kind::Maintenance;
            task.priority = Priority::Optional; task.checkpoint.step = "native_attack_with_other_owner";
            task.createdAtMs = task.updatedAtMs = NowMs();
            const auto monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            // Fault setup represents a previously acquired job interrupted by
            // combat. It is NOT permission to admit ordinary work during combat.
            state->authority.Observe(current, 0);
            const auto grant = state->authority.Acquire(task, Mask(Effect::TravelTarget), monotonic, 30000);
            if (!grant.Granted()) throw std::runtime_error("fixture_prior_owner_rejected");
            state->authority.Observe(current, NativeSafety(bot));
            binding.second.publisher.Publish(state->authority.Read(binding.first));
            bot->AttackStop(); // Controlled, synchronous interruption of THIS encounter only.
            state->enforceEffects.store(true, std::memory_order_release);
            NativeAttackReplay replay(ai);
            started = replay.Reengage(enemy);
            relations = bot->GetVictim() == enemy && enemy->getAttackers().count(bot) != 0;
            ownedTargetPreserved = SameLease(state->authority.Read(binding.first).lease, grant.lease) &&
                target->GetRelevance() == oldRelevance && target->IsForced() == oldForced;
        } catch (const std::exception& error) { report.put("failure", error.what()); }
        restore.fn(); restore.fn = [] {};
        const bool restored = bot->GetVictim() == enemy && bot->GetHealth() == oldHealth &&
            enemy->GetHealth() == oldEnemyHealth && bot->GetMoney() == oldMoney && bot->GetPower(POWER_MANA) == oldMana &&
            bot->GetSelectionGuid() == oldSelection && bot->getAttackTimer(BASE_ATTACK) == oldMainTimer &&
            bot->getAttackTimer(OFF_ATTACK) == oldOffTimer && !state->authority.Read(binding.first).lease.actor;
        report.put("native_attack_started", started); report.put("native_attack_relations_verified", relations);
        report.put("owned_travel_target_preserved", ownedTargetPreserved);
        report.put("fixture_state_restored", restored);
        report.put("outcome", started && relations && ownedTargetPreserved && restored ? "passed" : "failed");
        emit(report); return;
    }
}
