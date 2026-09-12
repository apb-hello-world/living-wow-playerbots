#pragma once
#include "LivingActivity.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>

namespace LivingActivity {
    // Finite, immutable learning intent in the existing task checkpoint. Book
    // GUID/location belongs to its native resource claim, not a virtual item.
    struct RecipeLearningJob { uint32_t book=0,recipe=0,skill=0,teacher=0; };
    inline bool ValidRecipeLearningJob(const RecipeLearningJob& job) {
        return job.book && job.recipe && job.teacher && job.recipe!=job.teacher &&
            std::set<uint32_t>{129,164,165,171,182,185,186,197,202,333,356,393,755}.count(job.skill);
    }
    inline std::string EncodeRecipeLearningJob(const RecipeLearningJob& job) {
        if (!ValidRecipeLearningJob(job)) throw std::invalid_argument("invalid_recipe_learning_job");
        return "{\"workflow\":\"recipe_learning_v1\",\"book\":"+std::to_string(job.book)+
            ",\"recipe\":"+std::to_string(job.recipe)+",\"skill\":"+std::to_string(job.skill)+
            ",\"teacher\":"+std::to_string(job.teacher)+'}';
    }
    inline bool DecodeRecipeLearningJob(const std::string& data,RecipeLearningJob& job,std::string& blocker) {
        job={};blocker="invalid_recipe_learning_checkpoint";
        if (data.empty() || data.size()>1024) return false;
        try {
            boost::property_tree::ptree p;std::istringstream input(data);boost::property_tree::read_json(input,p);
            const std::set<std::string> fields{"workflow","book","recipe","skill","teacher"};std::set<std::string> seen;
            for (const auto& field:p) if (!fields.count(field.first) || !field.second.empty() || !seen.insert(field.first).second) return false;
            if (seen!=fields || p.get<std::string>("workflow")!="recipe_learning_v1") return false;
            auto number=[&](const char* key) {
                const auto text=p.get<std::string>(key);
                if (text.empty() || text.size()>10 || text[0]=='0' || text.find_first_not_of("0123456789")!=std::string::npos)
                    throw std::invalid_argument("invalid_recipe_number");
                const auto value=std::stoull(text);
                if (value>std::numeric_limits<uint32_t>::max()) throw std::invalid_argument("invalid_recipe_number");
                return uint32_t(value);
            };
            const RecipeLearningJob parsed{number("book"),number("recipe"),number("skill"),number("teacher")};
            if (!ValidRecipeLearningJob(parsed)) return false;
            job=parsed;blocker.clear();return true;
        } catch (const std::exception&) {return false;}
    }
    inline bool IsRecipeLearningTask(const Task& task) {
        return task.source=="recipe_learning" || task.checkpoint.step.compare(0,7,"recipe_")==0;
    }
    inline bool ValidateRecipeLearningTask(const Task& task,std::string& blocker) {
        if (!IsRecipeLearningTask(task)) {blocker.clear();return true;}
        RecipeLearningJob job;
        if (task.source!="recipe_learning" || task.kind!=Kind::Profession || task.root!=task.id ||
            !task.parent.empty() || !DecodeRecipeLearningJob(task.checkpoint.data,job,blocker)) {
            blocker="invalid_recipe_learning_task";return false;
        }
        blocker.clear();return true;
    }
    inline bool PreserveRecipeLearningIntent(const Task& before,const Task& after,std::string& blocker) {
        if (!ValidateRecipeLearningTask(after,blocker)) return false;
        if (before.accepted && IsRecipeLearningTask(before) &&
            (!IsRecipeLearningTask(after) || before.checkpoint.data!=after.checkpoint.data)) {
            blocker="accepted_recipe_learning_intent_is_immutable";return false;
        }
        blocker.clear();return true;
    }
    struct RecipeLearningFrame {
        uint32_t actor=0,guid=0,entry=0,count=0,bag=0,slot=0,money=0,skill=0,maximum=0;
        bool known=false;
        bool operator==(const RecipeLearningFrame& b) const {
            return actor==b.actor && guid==b.guid && entry==b.entry && count==b.count && bag==b.bag && slot==b.slot &&
                money==b.money && skill==b.skill && maximum==b.maximum && known==b.known;
        }
    };
    struct RecipeLearningResult {
        RecipeLearningFrame before,after;
        bool started=false,effect=false,finished=false,succeeded=false,uncertain=false;
    };
    inline OperationState VerifyRecipeLearning(const RecipeLearningJob& job,const RecipeLearningResult& r,std::string& blocker) {
        blocker="recipe_learning_native_outcome_uncertain";
        if (!ValidRecipeLearningJob(job) || !r.started || !r.finished || r.uncertain || !r.before.actor ||
            !r.before.guid || r.before.entry!=job.book || !r.before.count || r.before.known ||
            r.after.actor!=r.before.actor || r.after.guid!=r.before.guid || r.after.entry!=job.book)
            return OperationState::Reconciling;
        if (!r.effect && !r.succeeded && r.before==r.after) {
            blocker="native_recipe_learning_rejected_without_effect";return OperationState::Rejected;
        }
        // Tier-book advancement has a different skill-cap proof; never pretend
        // ordinary recipe learning proves that unsupported transition.
        if (!r.effect || !r.succeeded || !r.after.known || r.after.count!=r.before.count-1 ||
            r.after.money!=r.before.money || r.after.skill!=r.before.skill || r.after.maximum!=r.before.maximum ||
            (r.after.count && (r.after.bag!=r.before.bag || r.after.slot!=r.before.slot))) return OperationState::Reconciling;
        blocker="native_recipe_book_consumed_and_spell_learned";return OperationState::Verified;
    }
}
