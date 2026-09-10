// Included only by the explicitly isolated native build, after coordinator
// State is defined. These are real installed native values, not fake classes.
// It runs synchronously on the world thread BEFORE map updates. No character
// items, gold, talents, quests or persistent task outcomes are manufactured.
#include "playerbot/strategy/values/TargetValue.h"
#include "playerbot/strategy/values/RpgValues.h"
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace {
    void RequireIsolatedNativeFixtureEnvironment() {
        const char* environment = std::getenv("LIVING_WOW_TEST_ENVIRONMENT");
        std::ifstream marker("/isolated/ENVIRONMENT");
        std::string value; std::getline(marker, value);
        if (!environment || std::string(environment) != "isolated-migration" || value != "isolated-migration-v1")
            throw std::runtime_error("Isolated native fixture binary refused an unmarked environment");
    }
}

void LivingActivityCoordinator::RunIsolatedBoundaryFixture() {
    MANGOS_ASSERT(OnWorldThread());
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    const char* suite = std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    if (!suite || std::string(suite) != "activity-boundaries-v1") return;
    RequireIsolatedNativeFixtureEnvironment();
    if (!state->loaded || state->ioPending || !state->pending.empty() || !state->incoming.empty()) return;
    // Fixtures never run in the presence of an authenticated native client.
    // Random bots use separate holder sessions, not World client sessions.
    if (sWorld.GetActiveSessionCount()) return;

    for (auto& binding : state->bindings) {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        if (!ai || ai->GetActivityActorEpoch() != binding.second.actorEpoch ||
            !bot->IsInWorld() || bot->GetGroup() || NativeSafety(bot)) continue;
        const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
        if (!current.actorGeneration || !current.mapGeneration ||
            state->authority.Read(binding.first).lease.actor) continue;
        state->fixtureFinished = true; // One bounded attempt per boot, never a retry loop.
        auto& publisher = binding.second.publisher;
        auto* target = ai->GetAiObjectContext()->GetValue<ai::TravelTarget*>("travel target")->Get();
        auto* rpg = ai->GetAiObjectContext()->GetValue<ai::GuidPosition>("rpg target");
        auto* next = ai->GetAiObjectContext()->GetValue<std::string>("next rpg action");
        if (!target || !rpg || !next) {
            sLog.outError("Living isolated fixture: suite=activity-boundaries-v1 outcome=failed reason=native_values_missing");
            return;
        }
        const bool originalForced = target->IsForced();
        const uint32_t originalRelevance = target->GetRelevance();
        const auto originalRpg = rpg->Get(); const auto originalNext = next->Get();
        const auto originalPolicy = state->publishedPolicyRevision.load(std::memory_order_acquire);
        const bool originalEnforcement = state->enforceEffects.load(std::memory_order_acquire);
        struct Restore { std::function<void()> fn; ~Restore() { fn(); } } restore{[&] {
            // Restore exact transient values before any map worker can execute.
            state->enforceEffects.store(false, std::memory_order_release);
            state->publishedPolicyRevision.store(originalPolicy, std::memory_order_release);
            target->SetForced(originalForced); target->SetRelevance(originalRelevance);
            rpg->Set(originalRpg); next->Set(originalNext);
            state->authority.Forget(binding.first);
            state->authority.Observe(current, NativeSafety(bot));
            publisher.Publish(state->authority.Read(binding.first));
            state->enforceEffects.store(originalEnforcement, std::memory_order_release);
        }};
        boost::property_tree::ptree report, checks;
        report.put("suite", "activity-boundaries-v1"); report.put("boot", state->boot);
        report.put("actor", binding.first); report.put("test_only", true);
        report.put("native_inventory_operations", 0); report.put("persistent_task_completions", 0);
        bool passed = true;
        auto require = [&](bool result, const char* name) {
            boost::property_tree::ptree check; check.put("case", name); check.put("passed", result);
            checks.push_back({"", check}); passed = passed && result;
            if (!result) throw std::runtime_error(name);
        };
        try {
            const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            Task task; task.id = task.root = NewId(); task.source = "isolated_fixture";
            task.sourceKey = state->boot; task.actor = binding.first; task.context = current;
            task.createdAtMs = task.updatedAtMs = NowMs(); task.mode = Mode::Active;
            task.phase = Phase::Executing; task.kind = Kind::Maintenance; task.priority = Priority::Optional;
            task.checkpoint.step = "native_boundary_fixture";
            state->authority.Observe(current, 0);
            const auto first = state->authority.Acquire(task, Mask(Effect::TravelTarget), monotonic, 60000);
            require(first.Granted(), "world_grant"); task.ownerGeneration = first.lease.generation;
            publisher.Publish(state->authority.Read(binding.first));
            ActionContext action; action.task = action.rootTask = task.id; action.world = current;
            action.revision = task.revision; action.ownerGeneration = first.lease.generation;
            action.origin = "isolated_boundary_fixture"; action.permittedEffects = Mask(Effect::TravelTarget);
            state->enforceEffects.store(true, std::memory_order_release);
            target->SetForced(!originalForced); target->SetRelevance(originalRelevance ^ 0x100);
            require(target->IsForced() == originalForced && target->GetRelevance() == originalRelevance,
                "unscoped_installed_route_rejected");
            ai::TravelTarget candidate(ai); candidate.SetRelevance(873);
            require(candidate.GetRelevance() == 873, "uninstalled_planning_value_allowed");
            const ai::GuidPosition otherRpg = originalRpg.GetRawValue() == bot->GetObjectGuid().GetRawValue() ?
                ai::GuidPosition() : ai::GuidPosition(bot);
            rpg->Set(otherRpg); next->Set("isolated_pending_action");
            require(rpg->Get().GetRawValue() == originalRpg.GetRawValue() && next->Get() == originalNext,
                "unscoped_rpg_values_rejected");
            {
                ExecutionScope scope(task, action);
                target->SetForced(!originalForced); target->SetRelevance(originalRelevance ^ 0x100);
                rpg->Set(otherRpg); next->Set("isolated_pending_action");
                require(target->IsForced() != originalForced && target->GetRelevance() == (originalRelevance ^ 0x100) &&
                    rpg->Get().GetRawValue() == otherRpg.GetRawValue() && next->Get() == "isolated_pending_action",
                    "exact_owner_native_values_allowed");
            }
            Task human = task; human.id = human.root = NewId(); human.priority = Priority::Human;
            const auto second = state->authority.Acquire(human, Mask(Effect::TravelTarget), monotonic, 60000);
            require(second.Granted() && SameLease(second.displaced, first.lease), "preemption_returns_exact_displaced_owner");
            publisher.Publish(state->authority.Read(binding.first));
            {
                ExecutionScope stale(task, action); target->SetForced(originalForced);
                require(target->IsForced() != originalForced, "preempted_scope_cannot_mutate_installed_route");
            }
            require(state->authority.Release(first.lease).code == AuthorityCode::StaleLease &&
                SameLease(state->authority.Read(binding.first).lease, second.lease), "stale_release_preserves_new_owner");
            human.ownerGeneration = second.lease.generation;
            action.task = action.rootTask = human.id; action.ownerGeneration = second.lease.generation;
            {
                ExecutionScope scope(human, action); target->SetForced(originalForced);
                require(target->IsForced() == originalForced, "replacement_owner_can_mutate");
                state->publishedPolicyRevision.store(originalPolicy + 1, std::memory_order_release);
                target->SetForced(!originalForced);
                require(target->IsForced() == originalForced, "fresh_policy_revocation_rejects_cached_grant");
                state->publishedPolicyRevision.store(originalPolicy, std::memory_order_release);
            }
            ++action.world.mapGeneration;
            {
                ExecutionScope stale(human, action); target->SetForced(!originalForced);
                require(target->IsForced() == originalForced, "stale_map_scope_rejected");
            }
            require(PermitEffects(*ai, {0, Lane::Inspection, true}, "isolated read only"), "inspection_remains_allowed");
            require(!PermitEffects(*ai, {0, Lane::Managed, false}, "isolated unknown effect"), "unclassified_mutation_rejected");
        } catch (const std::exception& error) { passed = false; report.put("failure", error.what()); }
        restore.fn(); restore.fn = [] {};
        const bool restored = target->IsForced() == originalForced && target->GetRelevance() == originalRelevance &&
            rpg->Get().GetRawValue() == originalRpg.GetRawValue() && next->Get() == originalNext &&
            !state->authority.Read(binding.first).lease.actor &&
            state->enforceEffects.load(std::memory_order_acquire) == originalEnforcement;
        passed = passed && restored; report.put("transient_state_restored", restored);
        report.put("outcome", passed ? "passed" : "failed"); report.add_child("checks", checks);
        // Unique boot file; separate from native task outcomes and live receipts.
        std::ofstream evidence("/isolated/evidence/activity-boundaries-" + state->boot + ".json");
        evidence << Json(report); evidence.close();
        if (!evidence) sLog.outError("Living isolated fixture evidence write failed");
        sLog.outString("Living isolated fixture: %s", Json(report).c_str());
        return;
    }
}
