// Read-only native recipe inspection in the explicitly marked copied realm.
// The template takes the already existing coordinator state; it is not a new
// timer, worker, task store or executor. No task is submitted or cast performed.
#include "playerbot/LivingProfessionNative.h"
#include "playerbot/LivingProfessionPlan.h"
#include "Spells/SpellMgr.h"
namespace {
    template<class FixtureState>
    void RunIsolatedProfessionFixture(FixtureState& state) {
        if (state.fixtureFinished || NowMs() < state.fixtureNext) return;
        state.fixtureNext = NowMs() + 1000;
        if (sWorld.GetActiveAndQueuedSessionCount() || !state.loaded || state.ioPending ||
            !state.incoming.empty()) return;
        if (!state.professionFixtureDeadline) state.professionFixtureDeadline = NowMs() + 60000;
        auto emit = [&](boost::property_tree::ptree& report) {
            report.put("suite", "activity-profession-v1"); report.put("boot", state.boot);
            report.put("test_only", true); report.put("read_only_native_inspection", true);
            report.put("native_operations", 0); report.put("persistent_tasks_created", 0);
            report.put("paid_workflow_proof", false); report.put("spell_grants", 0);
            report.put("general_execution_enabled", state.enforceEffects.load(std::memory_order_acquire));
            std::ofstream evidence("/isolated/evidence/activity-profession-" + state.boot + ".json");
            evidence << Json(report); evidence.close(); state.fixtureFinished = true;
            if (!evidence) sLog.outError("Living isolated profession inspection evidence write failed");
            sLog.outString("Living isolated fixture: %s", Json(report).c_str());
        };
        if (NowMs() > state.professionFixtureDeadline) {
            boost::property_tree::ptree report;
            report.put("outcome", "blocked"); report.put("failure", "no_known_native_item_recipe_for_inspection");
            emit(report); return;
        }
        unsigned actors = 0;
        for (const auto& binding : state.bindings) {
            if (++actors > 50) break;
            auto* bot = sRandomPlayerbotMgr.GetPlayerBot(binding.first);
            if (!bot || !bot->GetPlayerbotAI() || !bot->IsInWorld() || bot->IsBeingTeleported()) continue;
            unsigned spells = 0;
            for (const auto& known : bot->GetSpellMap()) {
                if (++spells > 256) break;
                if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled) continue;
                const auto* spell = sServerFacade.LookupSpellInfo(known.first);
                if (!spell || spell->Effect[0] != SPELL_EFFECT_CREATE_ITEM || !spell->EffectItemType[0]) continue;
                const auto bounds = sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spell->Id);
                uint32_t skill = 0;
                for (auto it = bounds.first; it != bounds.second; ++it)
                    if (it->second && LivingProfessions::Primary(it->second->skillId) &&
                        bot->GetSkillValuePure(it->second->skillId)) { skill = it->second->skillId; break; }
                if (!skill) continue;
                ProfessionJob job; job.recipe = spell->Id; job.skill = skill;
                job.purpose = ProfessionPurpose::Intermediate; job.outputQuantity = 1;
                const auto native = InspectNativeProfessionRecipe(*bot, job);
                if (!native.blocker.empty() || native.operation != ProfessionOperation::CreateItem || native.reagents.empty()) continue;
                job.outputEntry = native.outputEntry; job.reagents = native.reagents;
                job.initialSkill = native.skillValue;
                Task task; task.id = task.root = NewId(); task.actor = bot->GetGUIDLow();
                task.kind = Kind::Profession; task.source = "profession_job";
                task.checkpoint.step = "profession_prepare"; task.checkpoint.data = EncodeProfessionJob(job);
                const auto money = bot->GetMoney(), count = bot->GetItemCount(job.outputEntry, true);
                boost::property_tree::ptree report, checks; unsigned failed = 0;
                auto check = [&](const char* name, bool passed) { checks.put(name, passed); if (!passed) ++failed; };
                std::string blocker;
                check("exact_native_recipe_accepted", ValidateNativeProfessionTask(*bot, task, blocker));
                auto changed = job; ++changed.reagents.front().perAttempt;
                task.checkpoint.data = EncodeProfessionJob(changed);
                check("wrong_reagent_quantity_rejected", !ValidateNativeProfessionTask(*bot, task, blocker) &&
                    blocker == "profession_native_reagents_mismatch");
                changed = job; ++changed.outputEntry; task.checkpoint.data = EncodeProfessionJob(changed);
                check("wrong_output_rejected", !ValidateNativeProfessionTask(*bot, task, blocker) &&
                    blocker == "profession_native_output_mismatch");
                task.checkpoint.data = EncodeProfessionJob(job); ++task.actor;
                check("different_actor_rejected", !ValidateNativeProfessionTask(*bot, task, blocker) &&
                    blocker == "profession_native_actor_mismatch");
                task.actor = bot->GetGUIDLow();
                check("revalidated_original_accepted", ValidateNativeProfessionTask(*bot, task, blocker));
                check("native_state_unchanged", bot->GetMoney() == money && bot->GetItemCount(job.outputEntry, true) == count &&
                    bot->GetSkillValuePure(job.skill) == native.skillValue && bot->HasSpell(job.recipe));
                report.put("actor", bot->GetGUIDLow()); report.put("recipe", job.recipe);
                report.put("skill", job.skill); report.put("skill_value", native.skillValue);
                report.put("output_entry", job.outputEntry); report.put("reagent_types", job.reagents.size());
                report.put("outcome", failed ? "failed" : "passed"); report.put("failed_checks", failed);
                report.add_child("checks", checks); emit(report); return;
            }
        }
    }
}
