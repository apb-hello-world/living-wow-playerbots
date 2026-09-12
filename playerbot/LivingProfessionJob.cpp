#include "LivingProfessionJob.h"
#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace LivingActivity {
    std::string EconomyProfessionSourceKey(uint64_t goalRow) {
        return goalRow ? "economy_goal:"+std::to_string(goalRow) : "";
    }
    bool EconomyProfessionRecipe(uint32_t actor,const std::string& capability,uint32_t& recipe) {
        recipe=0;
        const auto prefix="profession:"+std::to_string(actor)+":";
        if (!actor || capability.compare(0,prefix.size(),prefix)) return false;
        const auto value=capability.substr(prefix.size());
        if (value.empty() || value.size()>10 || value[0]=='0' || value.find_first_not_of("0123456789")!=std::string::npos)
            return false;
        const auto parsed=std::stoull(value);
        if (parsed>std::numeric_limits<uint32_t>::max()) return false;
        recipe=uint32_t(parsed);return true;
    }
    bool MatchesEconomyProfession(const Task& task,uint32_t actor,uint64_t goalRow,const std::string& capability) {
        ProfessionJob job;std::string blocker;uint32_t recipe=0;
        return goalRow && task.source=="profession_job" && task.sourceKey==EconomyProfessionSourceKey(goalRow) &&
            task.actor==actor && task.id==task.root && task.parent.empty() && task.accepted && task.mode==Mode::Active &&
            EconomyProfessionRecipe(actor,capability,recipe) && DecodeProfessionJob(task.checkpoint.data,job,blocker) &&
            job.recipe==recipe;
    }
    bool RequiredProfessionVendorQuantity(const ProfessionReagent& need,const ProfessionStock& stock,
        uint32_t bundle,uint32_t& quantity,std::string& blocker) {
        quantity=0;
        auto reject=[&](const char* why){blocker=why; return false;};
        if (!need.entry || need.entry!=stock.entry || !need.perAttempt || !bundle)
            return reject("profession_vendor_demand_invalid");
        if (stock.bag>=need.perAttempt) return reject("profession_purchase_material_already_available");
        if (stock.bank) return reject("profession_banked_material_requires_collection");
        if (stock.delivered) return reject("profession_delivered_material_requires_collection");
        if (stock.paidInTransit) return reject("profession_paid_material_in_transit");
        const uint64_t units=(uint64_t(need.perAttempt-stock.bag)+bundle-1)/bundle;
        const uint64_t rounded=units*bundle;
        if (units>255 || rounded>10000) return reject("profession_vendor_quantity_exceeds_native_bound");
        quantity=uint32_t(rounded); blocker.clear(); return true;
    }
    namespace {
        using Tree = boost::property_tree::ptree;
        uint32_t Number(const Tree& node) {
            const auto& raw = node.data();
            if (!node.empty() || raw.empty() || raw.size() > 10 || raw.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("invalid profession number");
            const auto value = std::stoull(raw);
            if (value > std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("profession number overflow");
            return uint32_t(value);
        }
        bool SameIntent(const ProfessionJob& a, const ProfessionJob& b) {
            return std::tie(a.recipe,a.skill,a.subjectItem,a.outputEntry,a.outputQuantity,a.initialSkill,a.targetSkill,
                a.attemptLimit,a.operation,a.purpose,a.reagents) ==
                std::tie(b.recipe,b.skill,b.subjectItem,b.outputEntry,b.outputQuantity,b.initialSkill,b.targetSkill,
                b.attemptLimit,b.operation,b.purpose,b.reagents);
        }
    }
    bool ValidateProfessionJob(const ProfessionJob& job, std::string& blocker) {
        blocker = "invalid_profession_job";
        if (!job.recipe || !job.skill || !job.attemptLimit || job.attemptLimit > 20 || job.reagents.size() > 8 ||
            unsigned(job.operation) > unsigned(ProfessionOperation::DisenchantItem) ||
            unsigned(job.purpose) > unsigned(ProfessionPurpose::Equipment)) return false;
        const bool itemOperation = job.operation == ProfessionOperation::EnchantItem || job.operation == ProfessionOperation::DisenchantItem;
        if (itemOperation != (job.subjectItem != 0)) return false;
        if (job.operation == ProfessionOperation::CreateItem || job.operation == ProfessionOperation::TransformMaterial)
            if (!job.outputEntry || !job.outputQuantity || job.outputQuantity > 10000) return false;
        if (job.operation == ProfessionOperation::EnchantItem && (job.outputEntry || job.outputQuantity)) return false;
        if (job.operation == ProfessionOperation::DisenchantItem && (job.outputEntry || job.outputQuantity)) return false;
        if (job.purpose == ProfessionPurpose::SkillGain) {
            if (!job.targetSkill || job.targetSkill <= job.initialSkill) return false;
        } else if (job.targetSkill) return false;
        uint32_t previous = 0;
        for (const auto& reagent : job.reagents) {
            if (!reagent.entry || reagent.entry <= previous || !reagent.perAttempt || reagent.perAttempt > 10000) return false;
            previous = reagent.entry; // Canonical unique order, no double-counting slots.
        }
        blocker.clear(); return true;
    }
    std::string EncodeProfessionJob(const ProfessionJob& job) {
        std::string blocker;
        if (!ValidateProfessionJob(job, blocker)) throw std::invalid_argument(blocker);
        Tree root;
        root.put("workflow", "profession_job_v1"); root.put("recipe", job.recipe); root.put("skill", job.skill);
        root.put("operation", unsigned(job.operation)); root.put("purpose", unsigned(job.purpose));
        root.put("subject_item", job.subjectItem); root.put("output_entry", job.outputEntry); root.put("output_quantity", job.outputQuantity);
        root.put("initial_skill", job.initialSkill); root.put("target_skill", job.targetSkill); root.put("attempt_limit", job.attemptLimit);
        Tree reagents;
        for (const auto& reagent : job.reagents) {
            Tree row; row.put("entry", reagent.entry); row.put("per_attempt", reagent.perAttempt); reagents.push_back({"", row});
        }
        root.add_child("reagents", reagents);
        std::ostringstream output; boost::property_tree::write_json(output, root, false); return output.str();
    }
    bool DecodeProfessionJob(const std::string& data, ProfessionJob& job, std::string& blocker) {
        blocker = "invalid_profession_checkpoint";
        if (data.empty() || data.size() > 8192) return false;
        try {
            Tree root; std::istringstream input(data); boost::property_tree::read_json(input, root);
            const std::set<std::string> fields = {"workflow","recipe","skill","operation","purpose","subject_item",
                "output_entry","output_quantity","initial_skill","target_skill","attempt_limit","reagents"};
            std::set<std::string> seen;
            for (const auto& field : root) if (!fields.count(field.first) || !seen.insert(field.first).second) return false;
            if (seen != fields || root.get<std::string>("workflow") != "profession_job_v1" || !root.get_child("workflow").empty()) return false;
            ProfessionJob parsed;
            parsed.recipe = Number(root.get_child("recipe")); parsed.skill = Number(root.get_child("skill"));
            parsed.operation = ProfessionOperation(Number(root.get_child("operation")));
            parsed.purpose = ProfessionPurpose(Number(root.get_child("purpose")));
            parsed.subjectItem = Number(root.get_child("subject_item")); parsed.outputEntry = Number(root.get_child("output_entry"));
            parsed.outputQuantity = Number(root.get_child("output_quantity")); parsed.initialSkill = Number(root.get_child("initial_skill"));
            parsed.targetSkill = Number(root.get_child("target_skill")); parsed.attemptLimit = Number(root.get_child("attempt_limit"));
            const auto& list = root.get_child("reagents");
            if (!list.data().empty() || list.size() > 8) return false;
            for (const auto& row : list) {
                if (!row.first.empty() || !row.second.data().empty() || row.second.size() != 2 ||
                    row.second.count("entry") != 1 || row.second.count("per_attempt") != 1) return false;
                parsed.reagents.push_back({Number(row.second.get_child("entry")),Number(row.second.get_child("per_attempt"))});
            }
            if (!ValidateProfessionJob(parsed, blocker)) return false;
            job = std::move(parsed); blocker.clear(); return true;
        } catch (const std::exception&) { return false; }
    }
    bool IsProfessionJob(const Task& task) {
        return task.source == "profession_job" || task.checkpoint.step.compare(0, 11, "profession_") == 0;
    }
    bool MatchNativeProfessionRecipe(const ProfessionJob& job, const NativeProfessionRecipe& native,
        std::string& blocker) {
        auto reject = [&](const char* reason) { blocker = reason; return false; };
        if (!ValidateProfessionJob(job, blocker)) return false;
        if (!native.blocker.empty()) { blocker = native.blocker; return false; }
        if (!native.known || native.recipe != job.recipe) return reject("profession_recipe_not_known");
        if (!native.skillValue || !native.skillMaximum || native.skill != job.skill)
            return reject("profession_recipe_skill_mismatch");
        const bool creates = job.operation == ProfessionOperation::CreateItem || job.operation == ProfessionOperation::TransformMaterial;
        if ((creates && native.operation != ProfessionOperation::CreateItem) ||
            (!creates && native.operation != job.operation)) return reject("profession_native_operation_mismatch");
        if (job.reagents != native.reagents) return reject("profession_native_reagents_mismatch");
        if (creates && (!native.outputEntry || job.outputEntry != native.outputEntry))
            return reject("profession_native_output_mismatch");
        if (creates && native.reagents.empty()) return reject("profession_native_material_input_required");
        if (!creates && !native.subjectOwned) return reject("profession_subject_not_owned");
        if (job.purpose == ProfessionPurpose::SkillGain) {
            if (job.initialSkill != native.skillValue) return reject("profession_initial_skill_changed");
            if (job.targetSkill > native.skillMaximum) return reject("profession_native_skill_cap");
            if (!native.greyAt || native.skillValue >= native.greyAt) return reject("profession_recipe_has_no_skill_gain");
        }
        blocker.clear(); return true;
    }
    bool ValidateProfessionTask(const Task& task, std::string& blocker) {
        if (!IsProfessionJob(task)) { blocker.clear(); return true; }
        ProfessionJob job;
        if (task.kind != Kind::Profession || !task.parent.empty() || !DecodeProfessionJob(task.checkpoint.data, job, blocker)) {
            if (blocker.empty()) blocker = "invalid_profession_task";
            return false;
        }
        blocker.clear(); return true;
    }
    bool PreserveProfessionIntent(const Task& before, const Task& after, std::string& blocker) {
        if (!ValidateProfessionTask(after, blocker)) return false;
        if (!before.accepted || !IsProfessionJob(before)) return true;
        ProfessionJob oldJob, nextJob;
        if (!IsProfessionJob(after) || !DecodeProfessionJob(before.checkpoint.data, oldJob, blocker) ||
            !DecodeProfessionJob(after.checkpoint.data, nextJob, blocker) || !SameIntent(oldJob, nextJob)) {
            blocker = "accepted_profession_intent_is_immutable"; return false;
        }
        blocker.clear(); return true;
    }
    bool MatchesProfessionMaterial(const Task& task, const ProfessionMaterialLink& link) {
        std::string blocker;
        ProfessionJob job;
        if (!task.accepted || !IsUuid(task.id) || !IsProfessionJob(task) || !ValidateProfessionTask(task, blocker) ||
            !DecodeProfessionJob(task.checkpoint.data, job, blocker) || link.task != task.id || link.actor != task.actor ||
            link.recipe != job.recipe || !IsUuid(link.operation) || !link.nativeReference || !link.quantity) return false;
        for (const auto& reagent : job.reagents) if (reagent.entry == link.entry) return true;
        return false;
    }

    ProfessionDecision NextProfessionStep(const Task& task, const ProfessionSnapshot& snapshot) {
        ProfessionDecision decision;
        auto stop = [&](ProfessionStep step, const char* blocker) {
            decision.step = step; decision.blocker = blocker; decision.quantities.clear(); return decision;
        };
        ProfessionJob job;
        std::string blocker;
        if (!task.accepted || !IsUuid(task.id) || !task.actor || !IsProfessionJob(task) ||
            !ValidateProfessionTask(task, blocker) || !DecodeProfessionJob(task.checkpoint.data, job, blocker))
            return stop(ProfessionStep::Reconcile, "invalid_profession_task");
        if (!snapshot.complete || snapshot.task != task.id || snapshot.revision != task.revision ||
            !(snapshot.context == task.context))
            return stop(ProfessionStep::Reconcile, "profession_snapshot_not_current");
        if (Terminal(task.phase)) return stop(ProfessionStep::Defer, "profession_task_terminal");
        if (snapshot.unresolvedOperation) return stop(ProfessionStep::Reconcile, "profession_operation_unresolved");
        if (snapshot.stock.size() != job.reagents.size() || snapshot.attempts.size() > job.attemptLimit)
            return stop(ProfessionStep::Reconcile, "profession_snapshot_inconsistent");
        for (size_t i = 0; i < job.reagents.size(); ++i)
            if (snapshot.stock[i].entry != job.reagents[i].entry)
                return stop(ProfessionStep::Reconcile, "profession_stock_identity_mismatch");

        // Only exact, saved native receipts count. Inventory totals, a changed
        // skill flag, elapsed time and a planner's replaced goal are not proof.
        std::set<std::string> operations;
        bool earnedSkillTarget = false;
        uint64_t previousRevision = 0;
        for (const auto& attempt : snapshot.attempts) {
            const auto& receipt = attempt.receipt;
            if (!attempt.committed || !IsUuid(receipt.id) || !operations.insert(receipt.id).second ||
                receipt.task != task.id || receipt.taskRevision <= previousRevision || receipt.taskRevision > task.revision ||
                receipt.kind != "profession_craft" || attempt.recipe != job.recipe ||
                attempt.subjectItem != job.subjectItem || !IsToken(receipt.evidence) ||
                receipt.state == OperationState::Intent || receipt.state == OperationState::Reconciling)
                return stop(ProfessionStep::Reconcile, "profession_attempt_not_reconciled");
            previousRevision = receipt.taskRevision;
            if (receipt.state == OperationState::Rejected) {
                if (attempt.nativeEffectVerified || !attempt.consumed.empty() || !attempt.produced.empty() ||
                    attempt.skillBefore != attempt.skillAfter)
                    return stop(ProfessionStep::Reconcile, "rejected_profession_effect_observed");
                continue;
            }
            if (receipt.state != OperationState::Verified || receipt.nativeReference.empty() ||
                !attempt.nativeEffectVerified || attempt.consumed != job.reagents ||
                attempt.skillAfter < attempt.skillBefore || attempt.produced.size() > 16)
                return stop(ProfessionStep::Reconcile, "profession_native_proof_mismatch");
            uint32_t previous = 0;
            bool expectedOutput = false;
            for (const auto& output : attempt.produced) {
                if (!output.entry || output.entry <= previous || !output.perAttempt)
                    return stop(ProfessionStep::Reconcile, "profession_output_proof_invalid");
                previous = output.entry;
                if (output.entry == job.outputEntry) {
                    decision.verifiedOutput += output.perAttempt; expectedOutput = true;
                }
            }
            if ((job.operation == ProfessionOperation::CreateItem || job.operation == ProfessionOperation::TransformMaterial) &&
                !expectedOutput) return stop(ProfessionStep::Reconcile, "profession_expected_output_missing");
            if (job.operation == ProfessionOperation::EnchantItem && !attempt.produced.empty())
                return stop(ProfessionStep::Reconcile, "enchant_produced_unexpected_items");
            if (job.operation == ProfessionOperation::DisenchantItem && attempt.produced.empty())
                return stop(ProfessionStep::Reconcile, "disenchant_output_proof_missing");
            ++decision.verifiedAttempts;
            earnedSkillTarget |= attempt.skillAfter > attempt.skillBefore && attempt.skillAfter >= job.targetSkill;
        }
        const bool fixedOutput = job.operation == ProfessionOperation::CreateItem || job.operation == ProfessionOperation::TransformMaterial;
        if (job.purpose == ProfessionPurpose::SkillGain ? earnedSkillTarget :
            (fixedOutput ? decision.verifiedOutput >= job.outputQuantity : decision.verifiedAttempts > 0))
            // Finalize is NOT completed: the executor must settle claims, surplus
            // orders and remaining possessions through their native receipts.
            return stop(ProfessionStep::Finalize, "verified_profession_goal_requires_settlement");
        if (!snapshot.safe) return stop(ProfessionStep::Pause, "profession_safety_pause");
        if (!snapshot.retryReady) return stop(ProfessionStep::Defer, "profession_retry_not_due");
        if (!snapshot.knownRecipe) return stop(ProfessionStep::Defer, "profession_recipe_not_known");
        if (snapshot.attempts.size() >= job.attemptLimit)
            return stop(ProfessionStep::Defer, "profession_attempt_limit");
        if (job.purpose == ProfessionPurpose::SkillGain && snapshot.skill >= job.targetSkill)
            return stop(ProfessionStep::Defer, "profession_target_met_without_job_proof");
        if (!snapshot.blocker.empty())
            return stop(ProfessionStep::Defer, IsToken(snapshot.blocker) ? snapshot.blocker.c_str() : "profession_native_inspection_blocked");
        if (!snapshot.useful) return stop(ProfessionStep::Defer, "profession_recipe_no_longer_useful");

        bool withdraw = false, collect = false, incoming = false, unavailable = false;
        std::string sourceBlocker;
        std::vector<ProfessionReagent> bank, mail, buy;
        for (size_t i = 0; i < job.reagents.size(); ++i) {
            const auto& need = job.reagents[i]; const auto& stock = snapshot.stock[i];
            uint32_t missing = need.perAttempt > stock.bag ? need.perAttempt - stock.bag : 0;
            const auto takeBank = std::min(missing, stock.bank);
            if (takeBank) { bank.push_back({need.entry,takeBank}); withdraw = true; missing -= takeBank; }
            const auto takeMail = std::min(missing, stock.delivered);
            if (takeMail) { mail.push_back({need.entry,takeMail}); collect = true; missing -= takeMail; }
            const auto expected = std::min(missing, stock.paidInTransit);
            if (expected) { incoming = true; missing -= expected; }
            if (missing) {
                if (stock.sourceAvailable) buy.push_back({need.entry,missing});
                else {
                    unavailable = true;
                    if(sourceBlocker.empty() && IsToken(stock.sourceBlocker)) sourceBlocker=stock.sourceBlocker;
                }
            }
        }
        if (!snapshot.capacity) return stop(ProfessionStep::PrepareCapacity, "profession_capacity_required");
        if (withdraw) {
            // Banked native stock exists; absence of a nearby banker is a
            // service prerequisite, not missing or inaccessible inventory.
            if (!snapshot.bankAccess) return stop(ProfessionStep::ReachBank, "profession_banker_travel_required");
            decision.step = ProfessionStep::Withdraw; decision.quantities = std::move(bank); return decision;
        }
        if (collect) { decision.step = ProfessionStep::Collect; decision.quantities = std::move(mail); return decision; }
        // Do not buy a partial speculative kit while a required reagent has no
        // valid source. Already paid/owned resources remain attached to this job.
        if (unavailable) return stop(ProfessionStep::Defer, sourceBlocker.empty()?"profession_material_source_unavailable":sourceBlocker.c_str());
        if (!buy.empty()) { decision.step = ProfessionStep::Purchase; decision.quantities = std::move(buy); return decision; }
        if (incoming) return stop(ProfessionStep::WaitForDelivery, "profession_paid_material_in_transit");
        if (!snapshot.tools) return stop(ProfessionStep::PrepareTools, "profession_tool_required");
        if (!snapshot.atStation) return stop(ProfessionStep::ReachStation, "profession_station_required");
        return stop(ProfessionStep::Execute, "profession_native_attempt_ready");
    }
}
