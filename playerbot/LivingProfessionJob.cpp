#include "LivingProfessionJob.h"
#include "LivingGuildDelivery.h"
#include "LivingRecipeLearning.h"
#include "LivingProfessionTools.h"
#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <map>
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
            EconomyProfessionRecipe(actor,capability,recipe) && DecodeProfessionIntent(task.checkpoint.data,job,blocker) &&
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
        const uint32_t missing=need.perAttempt-stock.bag;
        if (stock.paidInTransit>=missing) return reject("profession_paid_material_in_transit");
        // An accepted partial order protects its exact quantity, not the whole
        // reagent requirement. Buy only the still-unpaid remainder; collection
        // of banked or already-delivered stock retains priority above.
        const uint64_t units=(uint64_t(missing-stock.paidInTransit)+bundle-1)/bundle;
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
    static bool DecodeBaseProfessionJob(const std::string& data, ProfessionJob& job, std::string& blocker) {
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
    namespace {
        std::string Json(const Tree& tree) {
            std::ostringstream out;boost::property_tree::write_json(out,tree,false);return out.str();
        }
        uint64_t Revision(const Tree& value) {
            const auto& text=value.data();
            if(!value.empty() || text.empty() || text.size()>20 || text.find_first_not_of("0123456789")!=std::string::npos)
                throw std::invalid_argument("invalid profession preparation revision");
            return std::stoull(text);
        }
        bool ValidWorkflow(const ProfessionWorkflow& flow,std::string& blocker) {
            if(!ValidateProfessionJob(flow.intent,blocker))return false;
            blocker="invalid_profession_tool_preparation";
            if(flow.tools.size()>4)return false;
            uint64_t previous=0;std::set<uint32_t> recipes,outputs;
            for(size_t i=0;i<flow.tools.size();++i) {
                const auto& tool=flow.tools[i];const auto& job=tool.job;std::string why;
                if(!ValidateProfessionJob(job,why) || job.operation!=ProfessionOperation::CreateItem ||
                    job.purpose!=ProfessionPurpose::Intermediate || job.skill!=flow.intent.skill || job.outputQuantity!=1 ||
                    job.attemptLimit>3 || job.recipe==flow.intent.recipe || job.outputEntry==flow.intent.outputEntry ||
                    !recipes.insert(job.recipe).second || !outputs.insert(job.outputEntry).second ||
                    tool.startedRevision<2 || tool.startedRevision<=previous ||
                    (tool.finishedRevision ? tool.finishedRevision<=tool.startedRevision : i+1!=flow.tools.size()))return false;
                previous=tool.finishedRevision;
            }
            blocker.clear();return true;
        }
        bool SameTool(const ProfessionToolPreparation& a,const ProfessionToolPreparation& b) {
            return SameIntent(a.job,b.job) && a.startedRevision==b.startedRevision && a.finishedRevision==b.finishedRevision;
        }
    }
    bool DecodeProfessionWorkflow(const std::string& data,ProfessionWorkflow& flow,std::string& blocker) {
        blocker="invalid_profession_checkpoint";
        if(data.empty() || data.size()>8192)return false;
        try {
            Tree root;std::istringstream input(data);boost::property_tree::read_json(input,root);
            ProfessionWorkflow parsed;
            if(root.count("tool_preparations")) {
                if(root.count("tool_preparations")!=1)return false;
                const auto list=root.get_child("tool_preparations");root.erase("tool_preparations");
                if(!list.data().empty() || list.empty() || list.size()>4)return false;
                for(const auto& row:list) {
                    if(!row.first.empty() || !row.second.data().empty() || row.second.size()!=3 ||
                        row.second.count("job")!=1 || row.second.count("started_revision")!=1 || row.second.count("finished_revision")!=1)return false;
                    ProfessionToolPreparation tool;
                    if(!DecodeBaseProfessionJob(Json(row.second.get_child("job")),tool.job,blocker))return false;
                    tool.startedRevision=Revision(row.second.get_child("started_revision"));
                    tool.finishedRevision=Revision(row.second.get_child("finished_revision"));parsed.tools.push_back(std::move(tool));
                }
            }
            if(!DecodeBaseProfessionJob(Json(root),parsed.intent,blocker) || !ValidWorkflow(parsed,blocker))return false;
            flow=std::move(parsed);blocker.clear();return true;
        } catch(const std::exception&) {return false;}
    }
    std::string EncodeProfessionWorkflow(const ProfessionWorkflow& flow) {
        std::string blocker;if(!ValidWorkflow(flow,blocker))throw std::invalid_argument(blocker);
        if(flow.tools.empty())return EncodeProfessionJob(flow.intent);
        Tree root;std::istringstream input(EncodeProfessionJob(flow.intent));boost::property_tree::read_json(input,root);
        Tree list;
        for(const auto& tool:flow.tools) {
            Tree row,job;std::istringstream encoded(EncodeProfessionJob(tool.job));boost::property_tree::read_json(encoded,job);
            row.add_child("job",job);row.put("started_revision",tool.startedRevision);row.put("finished_revision",tool.finishedRevision);
            list.push_back({"",row});
        }
        root.add_child("tool_preparations",list);const auto encoded=Json(root);
        if(encoded.size()>8192)throw std::invalid_argument("profession_checkpoint_bound");
        return encoded;
    }
    bool DecodeProfessionIntent(const std::string& data,ProfessionJob& job,std::string& blocker) {
        ProfessionWorkflow flow;if(!DecodeProfessionWorkflow(data,flow,blocker))return false;
        job=std::move(flow.intent);return true;
    }
    bool DecodeProfessionJob(const std::string& data,ProfessionJob& job,std::string& blocker) {
        ProfessionWorkflow flow;if(!DecodeProfessionWorkflow(data,flow,blocker))return false;
        job=!flow.tools.empty() && !flow.tools.back().finishedRevision ? std::move(flow.tools.back().job) : std::move(flow.intent);
        return true;
    }
    bool HasActiveProfessionTool(const std::string& data) {
        ProfessionWorkflow flow;std::string blocker;
        return DecodeProfessionWorkflow(data,flow,blocker) && !flow.tools.empty() && !flow.tools.back().finishedRevision;
    }
    uint32_t ProfessionWorkflowAttemptLimit(const std::string& data) {
        ProfessionWorkflow flow;std::string blocker;if(!DecodeProfessionWorkflow(data,flow,blocker))return 0;
        uint32_t limit=flow.intent.attemptLimit;for(const auto& tool:flow.tools)limit+=tool.job.attemptLimit;return limit;
    }
    bool ProfessionJobAtRevision(const Task& task,uint64_t revision,ProfessionJob& job,bool& current,std::string& blocker) {
        ProfessionWorkflow flow;current=false;
        if(!revision || revision>task.revision || !DecodeProfessionWorkflow(task.checkpoint.data,flow,blocker))return false;
        for(size_t i=0;i<flow.tools.size();++i) {
            const auto& tool=flow.tools[i];
            if(tool.startedRevision<=revision && (!tool.finishedRevision || revision<tool.finishedRevision)) {
                job=tool.job;current=i+1==flow.tools.size() && !tool.finishedRevision;return true;
            }
        }
        job=flow.intent;current=flow.tools.empty() || flow.tools.back().finishedRevision;return true;
    }
    bool IsProfessionJob(const Task& task) {
        // Service step names are backward compatible and shared by learning
        // and guild deliveries; a route label never replaces the typed owner.
        // The typed root retains its identity while using the same adapter.
        return !IsRecipeLearningTask(task) && !IsManagedGuildDelivery(task) &&
            (task.source == "profession_job" || task.checkpoint.step.compare(0, 11, "profession_") == 0);
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
        if (!ValidateRecipeLearningTask(task,blocker)) return false;
        if (!IsProfessionJob(task)) { blocker.clear(); return true; }
        ProfessionWorkflow flow;
        if (task.kind != Kind::Profession || !task.parent.empty() || !DecodeProfessionWorkflow(task.checkpoint.data, flow, blocker) ||
            (!flow.tools.empty() && (!task.accepted || task.mode!=Mode::Active)) ||
            std::any_of(flow.tools.begin(),flow.tools.end(),[&](const auto& tool){
                return tool.startedRevision>task.revision || tool.finishedRevision>task.revision;})) {
            if (blocker.empty()) blocker = "invalid_profession_task";
            return false;
        }
        blocker.clear(); return true;
    }
    bool PreserveProfessionIntent(const Task& before, const Task& after, std::string& blocker) {
        if (!PreserveRecipeLearningIntent(before,after,blocker)) return false;
        if (!ValidateProfessionTask(after, blocker)) return false;
        if (!before.accepted || !IsProfessionJob(before)) {
            ProfessionWorkflow initial;
            if(IsProfessionJob(after) && DecodeProfessionWorkflow(after.checkpoint.data,initial,blocker) && !initial.tools.empty()) {
                blocker="profession_preparation_requires_accepted_root";return false;
            }
            return true;
        }
        ProfessionWorkflow oldFlow,nextFlow;
        if (!IsProfessionJob(after) || !DecodeProfessionWorkflow(before.checkpoint.data, oldFlow, blocker) ||
            !DecodeProfessionWorkflow(after.checkpoint.data, nextFlow, blocker) || !SameIntent(oldFlow.intent, nextFlow.intent)) {
            blocker = "accepted_profession_intent_is_immutable"; return false;
        }
        blocker="accepted_profession_preparation_is_immutable";
        if(nextFlow.tools.size()<oldFlow.tools.size() || nextFlow.tools.size()>oldFlow.tools.size()+1)return false;
        for(size_t i=0;i<oldFlow.tools.size();++i) {
            if(SameTool(oldFlow.tools[i],nextFlow.tools[i]))continue;
            const auto& a=oldFlow.tools[i];const auto& b=nextFlow.tools[i];
            if(i+1!=oldFlow.tools.size() || nextFlow.tools.size()!=oldFlow.tools.size() || a.finishedRevision ||
                !SameIntent(a.job,b.job) || a.startedRevision!=b.startedRevision || b.finishedRevision!=after.revision ||
                after.revision!=before.revision+1 || after.checkpoint.step!="profession_tool_ready" ||
                before.phase!=Phase::Verifying || after.phase!=Phase::Verifying)return false;
        }
        if(nextFlow.tools.size()>oldFlow.tools.size()) {
            if((!oldFlow.tools.empty() && !oldFlow.tools.back().finishedRevision) ||
                nextFlow.tools.back().startedRevision!=after.revision || nextFlow.tools.back().finishedRevision ||
                after.revision!=before.revision+1 || after.checkpoint.step!="profession_tool_prepare" ||
                after.phase!=Phase::Preparing || (before.phase!=Phase::Preparing && before.phase!=Phase::Verifying))return false;
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
        const bool protectedMaterials=snapshot.readinessBlocker=="profession_material_has_legacy_commitment";
        if ((!snapshot.readinessBlocker.empty() && !protectedMaterials) || snapshot.attempts.size() > ProfessionWorkflowAttemptLimit(task.checkpoint.data) ||
            !ValidProfessionTools(job,snapshot.requiredTools) || (!snapshot.toolBlocker.empty() && !IsToken(snapshot.toolBlocker)) ||
            (!protectedMaterials && (snapshot.stock.size()!=job.reagents.size() || snapshot.toolStock.size()!=snapshot.requiredTools.size())))
            return stop(ProfessionStep::Reconcile, "profession_snapshot_inconsistent");
        for (size_t i = 0; !protectedMaterials && i < job.reagents.size(); ++i)
            if (snapshot.stock[i].entry != job.reagents[i].entry)
                return stop(ProfessionStep::Reconcile, "profession_stock_identity_mismatch");
        for (size_t i=0;!protectedMaterials && i<snapshot.requiredTools.size();++i)
            if (snapshot.toolStock[i].entry!=snapshot.requiredTools[i].entry)
                return stop(ProfessionStep::Reconcile,"profession_tool_stock_identity_mismatch");

        // Only exact, saved native receipts count. Inventory totals, a changed
        // skill flag, elapsed time and a planner's replaced goal are not proof.
        std::set<std::string> operations;
        bool earnedSkillTarget = false;
        uint32_t currentAttempts=0;
        std::map<uint32_t,uint32_t> attemptsByRecipe;
        uint64_t previousRevision = 0;
        for (const auto& attempt : snapshot.attempts) {
            const auto& receipt = attempt.receipt;
            ProfessionJob proofJob;bool currentAttempt=false;
            if(!ProfessionJobAtRevision(task,receipt.taskRevision,proofJob,currentAttempt,blocker) ||
                ++attemptsByRecipe[proofJob.recipe]>proofJob.attemptLimit)
                return stop(ProfessionStep::Reconcile,"profession_attempt_step_mismatch");
            if(currentAttempt)++currentAttempts;
            if (!attempt.committed || !IsUuid(receipt.id) || !operations.insert(receipt.id).second ||
                receipt.task != task.id || receipt.taskRevision <= previousRevision || receipt.taskRevision > task.revision ||
                receipt.kind != "profession_craft" || attempt.recipe != proofJob.recipe ||
                attempt.subjectItem != proofJob.subjectItem || !IsToken(receipt.evidence) ||
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
                !attempt.nativeEffectVerified || attempt.consumed != proofJob.reagents ||
                attempt.skillAfter < attempt.skillBefore || attempt.produced.size() > 16)
                return stop(ProfessionStep::Reconcile, "profession_native_proof_mismatch");
            uint32_t previous = 0;
            bool expectedOutput = false;
            for (const auto& output : attempt.produced) {
                if (!output.entry || output.entry <= previous || !output.perAttempt)
                    return stop(ProfessionStep::Reconcile, "profession_output_proof_invalid");
                previous = output.entry;
                if (output.entry == proofJob.outputEntry) {
                    if(currentAttempt)decision.verifiedOutput += output.perAttempt;
                    expectedOutput = true;
                }
            }
            if ((proofJob.operation == ProfessionOperation::CreateItem || proofJob.operation == ProfessionOperation::TransformMaterial) &&
                !expectedOutput) return stop(ProfessionStep::Reconcile, "profession_expected_output_missing");
            if (proofJob.operation == ProfessionOperation::EnchantItem && !attempt.produced.empty())
                return stop(ProfessionStep::Reconcile, "enchant_produced_unexpected_items");
            if (proofJob.operation == ProfessionOperation::DisenchantItem && attempt.produced.empty())
                return stop(ProfessionStep::Reconcile, "disenchant_output_proof_missing");
            if(currentAttempt)++decision.verifiedAttempts;
            // Making a required tool can itself earn the requested skill gain.
            // Credit its actual native recipe, never pretend the main enchant
            // was cast. Tool handoff must finish before the root can settle.
            earnedSkillTarget |= proofJob.skill==job.skill && attempt.skillAfter > attempt.skillBefore &&
                attempt.skillAfter >= job.targetSkill;
        }
        // Validate every saved attempt first. A protected material can delay
        // work but must never hide an uncertain native effect or authorize a buy.
        if(protectedMaterials)return stop(ProfessionStep::Defer,"profession_material_has_legacy_commitment");
        const bool fixedOutput = job.operation == ProfessionOperation::CreateItem || job.operation == ProfessionOperation::TransformMaterial;
        if (job.purpose == ProfessionPurpose::SkillGain ? earnedSkillTarget :
            (fixedOutput ? decision.verifiedOutput >= job.outputQuantity : decision.verifiedAttempts > 0))
            // Finalize is NOT completed: the executor must settle claims, surplus
            // orders and remaining possessions through their native receipts.
            return stop(ProfessionStep::Finalize, "verified_profession_goal_requires_settlement");
        if (!snapshot.safe) return stop(ProfessionStep::Pause, "profession_safety_pause");
        if (!snapshot.retryReady) return stop(ProfessionStep::Defer, "profession_retry_not_due");
        if (!snapshot.knownRecipe) return stop(ProfessionStep::Defer, "profession_recipe_not_known");
        if (currentAttempts >= job.attemptLimit)
            return stop(ProfessionStep::Defer, "profession_attempt_limit");
        if (job.purpose == ProfessionPurpose::SkillGain && snapshot.skill >= job.targetSkill)
            return stop(ProfessionStep::Defer, "profession_target_met_without_job_proof");
        if (!snapshot.blocker.empty())
            return stop(ProfessionStep::Defer, IsToken(snapshot.blocker) ? snapshot.blocker.c_str() : "profession_native_inspection_blocked");
        if (!snapshot.useful) return stop(ProfessionStep::Defer, "profession_recipe_no_longer_useful");

        bool withdraw = false, collect = false, incoming = false, unavailable = false;
        std::string sourceBlocker=IsToken(snapshot.toolBlocker)?snapshot.toolBlocker:"";
        if (!snapshot.toolBlocker.empty()) unavailable=true;
        std::vector<ProfessionReagent> bank, mail, buy;
        for (size_t i = 0; i < job.reagents.size()+snapshot.requiredTools.size(); ++i) {
            const bool tool=i>=job.reagents.size();
            const auto index=tool?i-job.reagents.size():i;
            const auto& need=tool?snapshot.requiredTools[index]:job.reagents[index];
            const auto& stock=tool?snapshot.toolStock[index]:snapshot.stock[index];
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
