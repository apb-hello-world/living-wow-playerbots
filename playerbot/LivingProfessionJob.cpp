#include "LivingProfessionJob.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace LivingActivity {
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
}
