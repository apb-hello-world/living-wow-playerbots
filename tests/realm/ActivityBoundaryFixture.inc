// Included only by the explicitly isolated native build, after coordinator
// State is defined. These are real installed native values, not fake classes.
// It runs synchronously on the world thread BEFORE map updates. No character
// items, gold, talents, quests or persistent task outcomes are manufactured.
#include "playerbot/strategy/values/TargetValue.h"
#include "playerbot/strategy/values/RpgValues.h"
#include "playerbot/strategy/actions/LootRollAction.h"
#include "playerbot/LivingActivityGameplay.h"
#include <cstdlib>
#include <functional>
#include <stdexcept>

namespace {
    class TargetMutatingEligibility final : public ai::Action {
    public:
        TargetMutatingEligibility(PlayerbotAI* ai,ai::TravelTarget* target,uint32_t replacement)
            : Action(ai,"isolated_eligibility_probe"),target(target),replacement(replacement) {}
        bool isUseful() override { target->SetRelevance(replacement); return true; }
        bool isPossible() override { target->SetRelevance(replacement); return true; }
    private:
        ai::TravelTarget* target;
        uint32_t replacement;
    };
    void RequireIsolatedNativeFixtureEnvironment() {
        const char* environment = std::getenv("LIVING_WOW_TEST_ENVIRONMENT");
        std::ifstream marker("/isolated/ENVIRONMENT");
        std::string value; std::getline(marker, value);
        if (!environment || std::string(environment) != "isolated-migration" || value != "isolated-migration-v1")
            throw std::runtime_error("Isolated native fixture binary refused an unmarked environment");
    }
}

#include "ActivityProfessionFixture.inc"

void LivingActivityCoordinator::RunIsolatedBoundaryFixture() {
    MANGOS_ASSERT(OnWorldThread());
    const char* requested = std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    if (requested && std::string(requested)=="activity-off-ownership-v1") {
        RequireIsolatedNativeFixtureEnvironment();
        if (state->fixtureFinished || NowMs()<state->fixtureNext) return;
        state->fixtureNext=NowMs()+1000;
        // Read-only restart proof: do not toggle enforcement, submit work,
        // manufacture a claim, or write a task just to exercise Off mode.
        if (sWorld.GetActiveAndQueuedSessionCount() || state->ioPending ||
            !state->loaded || !state->incoming.empty() || !state->claimsEnumerated ||
            !state->incomingClaims.empty()) return;
        state->fixtureFinished=true;
        boost::property_tree::ptree report,accepted;
        report.put("suite","activity-off-ownership-v1");report.put("boot",state->boot);
        report.put("test_only",true);report.put("native_humans",0);
        for (const auto& row : state->cache) {
            const auto& task=row.second;
            if (task.mode!=Mode::Active || !task.accepted || Terminal(task.phase)) continue;
            boost::property_tree::ptree entry;entry.put("task",task.id);entry.put("revision",task.revision);
            entry.put("actor",task.actor);entry.put("phase",Name(task.phase));accepted.push_back({"",entry});
        }
        // Earlier isolated fault injection deliberately retained this unsupported
        // checkpoint. It must stay quarantined, not be deleted to pass a restart.
        const bool knownQuarantine=state->quarantined.empty() ||
            (state->quarantined.size()==1 &&
             state->quarantined.begin()->first=="99f5da45-114a-581c-8d27-52016d6d8e37" &&
             state->quarantined.begin()->second=="unsupported_checkpoint");
        report.put("only_known_metadata_fault_quarantined",knownQuarantine);
        const bool passed=state->desired=="off" && state->effective==Mode::Off &&
            !state->enforceEffects.load(std::memory_order_acquire) &&
            ProfessionOwnershipProjection()==EconomyOwnershipProjection::Managed && ProfessionStoreReady() &&
            state->resources.Protection().ready && state->resources.Size() && !accepted.empty() &&
            !state->taskAdmissions && !state->nativeDispatches && !state->acknowledged &&
            knownQuarantine && state->invalidRecords==state->quarantined.size() &&
            !state->invalidClaims && state->pending.empty();
        report.put("outcome",passed ? "passed" : "failed");report.add_child("accepted",accepted);
        boost::property_tree::ptree status;std::istringstream input(StatusJson());
        boost::property_tree::read_json(input,status);report.add_child("status",status);
        std::ofstream evidence("/isolated/evidence/activity-off-ownership-"+state->boot+".json");
        evidence<<Json(report);evidence.close();
        if (!evidence) sLog.outError("Living isolated Off ownership evidence write failed");
        sLog.outString("Living isolated fixture: %s",Json(report).c_str());return;
    }
    if (requested && std::string(requested)=="activity-profession-recover-v1") {
        RunIsolatedProfessionRecoveryFixture();return;
    }
    if (requested && std::string(requested)=="activity-profession-paid-v1") {
        if (state->craftFixture.request.task.actor) RunIsolatedCraftFixture();
        else RunIsolatedVendorFixture();
        return;
    }
    if (requested && (std::string(requested)=="activity-craft-v1" || std::string(requested)=="activity-craft-supplied-v1" ||
        std::string(requested)=="activity-craft-interrupted-v1" || std::string(requested)=="activity-profession-settlement-v1" ||
        std::string(requested)=="activity-profession-bank-v1" || std::string(requested)=="activity-profession-mail-v1" ||
        std::string(requested)=="activity-profession-mail-checkpoint-v1" || std::string(requested)=="activity-profession-mail-resume-v1" ||
        std::string(requested)=="activity-profession-mail-travel-v1" || std::string(requested)=="activity-profession-mail-travel-resume-v1")) {
        RunIsolatedCraftFixture();return;
    }
    if (requested && (std::string(requested) == "activity-vendor-v1" || std::string(requested) == "activity-vendor-failure-v1")) {
        RunIsolatedVendorFixture(); return;
    }
    if (requested && std::string(requested) == "activity-profession-v1") {
        RequireIsolatedNativeFixtureEnvironment();
        RunIsolatedProfessionFixture(*state); return;
    }
    if (requested && (std::string(requested) == "activity-gameplay-v1" || std::string(requested) == "activity-support-v1")) {
        RunIsolatedGameplayFixture(); return;
    }
    if (requested && std::string(requested) == "activity-combat-v1") {
        RunIsolatedCombatFixture(); return;
    }
    if (requested && std::string(requested) == "activity-pet-v1") {
        RunIsolatedPetFixture(); return;
    }
    if (requested && (std::string(requested) == "activity-admission-v1" || std::string(requested) == "activity-operations-v1" ||
        std::string(requested) == "activity-resources-v1" || std::string(requested) == "activity-resources-hold-v1" ||
        std::string(requested) == "activity-resources-recover-v1" || std::string(requested) == "activity-consumption-v1")) {
        RunIsolatedAdmissionFixture(); return;
    }
    if (state->fixtureFinished || NowMs() < state->fixtureNext) return;
    state->fixtureNext = NowMs() + 1000;
    const char* suite = std::getenv("LIVING_WOW_NATIVE_FIXTURE");
    if (!suite || std::string(suite) != "activity-boundaries-v1") return;
    RequireIsolatedNativeFixtureEnvironment();
    if (!state->loaded || state->ioPending || !state->pending.empty() || !state->incoming.empty()) return;
    // Fixtures never run in the presence of an authenticated native client.
    // Random bots use separate holder sessions, not World client sessions.
    if (sWorld.GetActiveAndQueuedSessionCount()) return;

    for (auto& binding : state->bindings) {
        Player* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
        auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
        if (!ai || ai->GetActivityActorEpoch() != binding.second.actorEpoch ||
            !bot->IsInWorld() || bot->GetGroup() || NativeSafety(bot) ||
            sPlayerbotRendezvousManager.GetPartyActivityOwner(binding.first)!=PlayerbotRendezvousManager::PartyActivityOwner::none) continue;
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
            state->compatibilityActors.erase(binding.first);
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
            task.phase = Phase::Traveling; task.kind = Kind::Maintenance; task.priority = Priority::Optional;
            task.checkpoint.step = "native_boundary_fixture";
            state->authority.Observe(current, 0);
            const auto first = state->authority.Acquire(task, Mask(Effect::TravelTarget), monotonic, 60000);
            require(first.Granted(), "world_grant"); task.ownerGeneration = first.lease.generation;
            publisher.Publish(state->authority.Read(binding.first));
            ActionContext action; action.task = action.rootTask = task.id; action.world = current;
            action.revision = task.revision; action.ownerGeneration = first.lease.generation;
            action.origin = "isolated_boundary_fixture"; action.permittedEffects = Mask(Effect::TravelTarget);
            state->enforceEffects.store(true, std::memory_order_release);
            const auto unvalidated = NativeActionContext(*ai, Lane::Roll, Mask(Effect::Inventory), uint32_t(Safety::Combat));
            require(unvalidated.world.actor == binding.first && !unvalidated.validated,
                "native_context_alone_is_not_permission");
            ai::AutoLootRollAction roll(ai); ai::Event rollEvent;
            require(roll.GetActivityEffects().lane == Lane::Roll && !roll.GetNativeActivityPermit(rollEvent).validated,
                "ungrouped_native_actor_has_no_roll_exception");
            require(!NativeSpellPermit(*ai, 0, bot).validated,
                "unknown_native_spell_has_no_gameplay_exception");
            require(!NativeCombatMovementPermit(*ai, nullptr).validated,
                "missing_native_focus_has_no_combat_movement_exception");
            require(!NativeCombatMovementPermit(*ai, bot).validated,
                "self_focus_does_not_establish_native_combat");
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
                TargetMutatingEligibility evaluationAction(ai,target,originalRelevance ^ 0x100);
                require(!evaluationAction.EvaluateUsefulness() && target->GetRelevance() == originalRelevance,
                    "native_action_usefulness_rejects_route_side_effect");
                require(!evaluationAction.EvaluatePossibility() && target->GetRelevance() == originalRelevance,
                    "native_action_possibility_rejects_route_side_effect");
                {
                    EvaluationScope evaluation(true);
                    ExecutionScope nestedNativeScope(task, action);
                    target->SetForced(!originalForced); target->SetRelevance(originalRelevance ^ 0x100);
                    rpg->Set(otherRpg); next->Set("isolated_pending_action");
                    require(evaluation.Rejected() && target->IsForced() == originalForced &&
                        target->GetRelevance() == originalRelevance && rpg->Get().GetRawValue() == originalRpg.GetRawValue() &&
                        next->Get() == originalNext, "eligibility_cannot_mutate_owned_native_targets");
                }
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
            }
            Task child = human; child.id = NewId(); child.root = child.parent = human.id;
            child.checkpoint.step = "fixture_dependent_service";
            ActionContext childAction = action; childAction.task = child.id;
            require(state->authority.SelectStep(second.lease, &child) == AuthorityCode::Allowed,
                "finite_child_uses_parent_lease");
            publisher.Publish(state->authority.Read(binding.first));
            {
                ExecutionScope parentScope(human, action); target->SetForced(!originalForced);
                require(target->IsForced() == originalForced, "parent_cannot_overwrite_current_child_route");
            }
            {
                ExecutionScope childScope(child, childAction); target->SetForced(!originalForced);
                require(target->IsForced() != originalForced, "current_child_native_mutation_allowed");
                target->SetForced(originalForced);
            }
            require(state->authority.SelectStep(second.lease, nullptr) == AuthorityCode::Allowed,
                "child_handoff_preserves_parent_lease");
            publisher.Publish(state->authority.Read(binding.first));
            {
                ExecutionScope oldChild(child, childAction); target->SetForced(!originalForced);
                require(target->IsForced() == originalForced, "finished_child_cannot_mutate_parent_route");
            }
            {
                ExecutionScope scope(human, action);
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
            require(state->authority.Release(second.lease).code==AuthorityCode::Released,"fixture_owner_released_before_legacy_replay");
            publisher.Publish(state->authority.Read(binding.first));
            state->enforceEffects.store(false,std::memory_order_release);
            ActivityLease legacy;
            const auto legacyResult=sPlayerbotRendezvousManager.AcquirePartyActivityLease(binding.first,
                PlayerbotRendezvousManager::PartyActivityOwner::economy_service,
                PlayerbotRendezvousManager::PartyActivityPhase::traveling,45,"isolated_legacy_replay",state->boot,legacy);
            require(legacyResult.Permitted() && state->authority.Read(binding.first).compatibility &&
                SameLease(state->authority.Read(binding.first).lease,legacy),"native_legacy_owner_uses_coordinator_book");
            require(state->authority.BeginAtomic(legacy,NewId(),monotonic).code==AuthorityCode::ReconciliationRequired,
                "legacy_handle_cannot_start_verified_operation");
            human.ownerGeneration=0;
            const auto replacement=state->authority.Acquire(human,Mask(Effect::TravelTarget),monotonic,60000);
            require(replacement.Granted() && SameLease(replacement.displaced,legacy),"new_task_preempts_legacy_exactly_once");
            publisher.Publish(state->authority.Read(binding.first));
            require(!sPlayerbotRendezvousManager.HasPartyActivityLease(legacy),"legacy_projection_observes_managed_preemption");
            sPlayerbotRendezvousManager.ReleasePartyActivityLease(legacy,
                PlayerbotRendezvousManager::PartyActivityPhase::deferred,"isolated_stale_release");
            require(SameLease(state->authority.Read(binding.first).lease,replacement.lease),"stale_legacy_release_preserves_managed_owner");
            ActivityLease competitor;
            const auto waiting=sPlayerbotRendezvousManager.AcquirePartyActivityLease(binding.first,
                PlayerbotRendezvousManager::PartyActivityOwner::guild_supply,
                PlayerbotRendezvousManager::PartyActivityPhase::traveling,45,"isolated_competitor",state->boot,competitor);
            require(waiting.Waiting() && !competitor.actor && SameLease(state->authority.Read(binding.first).lease,replacement.lease),
                "legacy_competitor_waits_behind_managed_owner");
            state->enforceEffects.store(true,std::memory_order_release);
            require(!NativeEngagedAttackPermit(*ai,nullptr).validated,"missing_native_attack_target_not_authorized");
            require(!NativeEngagedAttackPermit(*ai,bot).validated,"self_attack_has_no_native_exception");
            require(!NativePetSpellPermit(*ai,0,nullptr).validated,"unknown_pet_spell_has_no_native_exception");
            state->enforceEffects.store(false,std::memory_order_release);
            // Native slot layout, deliberately NOT a live membership mutation
            // or proof of a complete human-party workflow (stage 6).
            Group::MemberSlotList offlineRoster;
            Group::MemberSlot offline; offline.guid=bot->GetObjectGuid(); offline.livingActivityAccountId=1;
            offlineRoster.push_back(offline);
            const auto offlineHuman=ReadPartyProtection(offlineRoster,[](uint32_t){return false;},[](ObjectGuid){return false;});
            require(offlineHuman==PartyProtection::Human &&
                *PartyAdmissionBlocker(offlineHuman,PartyAdmission::ServiceCompatibility,false),
                "native_member_slot_offline_human_protected");
            require(*PartyAdmissionBlocker(offlineHuman,PartyAdmission::SavedExecutor,true),
                "unmigrated_saved_party_executor_cannot_claim_human_priority");
            offlineRoster.front().livingActivityAccountId=0;
            require(ReadPartyProtection(offlineRoster,[](uint32_t){return true;},[](ObjectGuid){return false;})==PartyProtection::Unresolved,
                "native_member_slot_unknown_account_fails_closed");
            require(ReleaseTaskLease(replacement.lease).code==AuthorityCode::Released,
                "managed_owner_released_before_cross_domain_replay");
            ActivityLease oldService, explicitWork;
            const auto service=sPlayerbotRendezvousManager.AcquirePartyActivityLease(binding.first,
                PlayerbotRendezvousManager::PartyActivityOwner::economy_service,
                PlayerbotRendezvousManager::PartyActivityPhase::traveling,45,"isolated_priority_replay",state->boot,oldService);
            require(service.Permitted(),"legacy_service_priority_replay_acquired");
            const auto explicitGrant=sPlayerbotRendezvousManager.AcquirePartyActivityLease(binding.first,
                PlayerbotRendezvousManager::PartyActivityOwner::player_command,
                PlayerbotRendezvousManager::PartyActivityPhase::traveling,45,"isolated_priority_replay",state->boot,explicitWork);
            require(explicitGrant.Permitted() && explicitWork.generation>oldService.generation &&
                !sPlayerbotRendezvousManager.HasPartyActivityLease(oldService),"legacy_human_priority_preempts_legacy_service");
            sPlayerbotRendezvousManager.ReleasePartyActivityLease(oldService,
                PlayerbotRendezvousManager::PartyActivityPhase::deferred,"isolated_stale_release");
            require(sPlayerbotRendezvousManager.HasPartyActivityLease(explicitWork),"preempted_service_cannot_release_explicit_work");
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
