#pragma once
#include "LivingActivity.h"
#include "LivingTrainingLesson.h"
#include "LivingServiceTravel.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {
struct PartyTrainingJob {uint32_t trainer=0,next=0;std::vector<uint32_t> lessons;};
inline bool ValidPartyTrainingJob(const PartyTrainingJob& job) {
    if(!job.trainer || job.lessons.empty() || job.lessons.size()>64 || job.next>job.lessons.size())return false;
    uint32_t previous=0;
    for(auto spell:job.lessons){if(!spell || spell<=previous)return false;previous=spell;}
    return true;
}
inline std::string EncodePartyTrainingJob(const PartyTrainingJob& job) {
    std::string out="{\"workflow\":\"party_training_v1\",\"trainer\":"+std::to_string(job.trainer)+
        ",\"next\":"+std::to_string(job.next)+",\"lessons\":[";
    for(size_t i=0;i<job.lessons.size();++i){if(i)out+=',';out+=std::to_string(job.lessons[i]);}
    return out+"]}";
}
inline bool DecodePartyTrainingJob(const std::string& value,PartyTrainingJob& out) {
    out={};if(value.size()>2048)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        if(p.get<std::string>("workflow")!="party_training_v1")return false;
        PartyTrainingJob job;job.trainer=p.get<uint32_t>("trainer");job.next=p.get<uint32_t>("next");
        for(const auto& row:p.get_child("lessons")) {
            if(!row.first.empty() || job.lessons.size()>=64)return false;
            job.lessons.push_back(row.second.get_value<uint32_t>());
        }
        if(!ValidPartyTrainingJob(job) || EncodePartyTrainingJob(job)!=value)return false;
        out=std::move(job);return true;
    }catch(...){return false;}
}
inline bool IsPartyTrainingTask(const Task& task) {
    return task.source=="party_training" && task.kind==Kind::PartyErrand && task.root==task.id && task.parent.empty();
}
inline bool ValidatePartyTrainingTask(const Task& task,std::string& why) {
    if(task.source!="party_training")return true;
    PartyTrainingJob job;
    if(!IsPartyTrainingTask(task) || !DecodePartyTrainingJob(task.checkpoint.data,job) ||
        (task.checkpoint.step!="party_training_prepare" && task.checkpoint.step!="party_training_learn" &&
            task.checkpoint.step!=ServiceStep(ServiceDestination::ClassTrainer)) ||
        (task.phase==Phase::Queued && job.next) || ((task.phase==Phase::Completed)!=(job.next==job.lessons.size()))) {
        why="invalid_party_training_task";return false;
    }
    return true;
}
inline bool PreservePartyTrainingIntent(const Task& before,const Task& after,std::string& why) {
    if(before.source!="party_training")return true;
    if(!IsPartyTrainingTask(after) || before.checkpoint.data!=after.checkpoint.data) {
        why="party_training_intent_requires_native_receipt";return false;
    }
    return true;
}
inline bool DirectFreeTrainingQuote(const TrainingLessonQuote& q) {
    return ValidTrainingLesson(q) && !q.cast && !q.cost && q.teachingSpell==q.lesson &&
        q.playerSpells==std::vector<uint32_t>{q.lesson} && q.petSpells.empty();
}
inline std::string EncodeDirectTrainingQuote(const TrainingLessonQuote& q) {
    return "{\"actor\":"+std::to_string(q.actor)+",\"trainer\":"+std::to_string(q.trainer)+
        ",\"lesson\":"+std::to_string(q.lesson)+",\"money\":"+std::to_string(q.money)+",\"pet\":"+std::to_string(q.pet)+'}';
}
inline bool DecodeDirectTrainingQuote(const std::string& value,TrainingLessonQuote& out) {
    out={};if(value.size()>512)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        TrainingLessonQuote q;q.actor=p.get<uint32_t>("actor");q.trainer=p.get<uint64_t>("trainer");
        q.lesson=q.teachingSpell=p.get<uint32_t>("lesson");q.money=p.get<uint32_t>("money");q.pet=p.get<uint64_t>("pet");
        q.playerSpells={q.lesson};
        if(!DirectFreeTrainingQuote(q) || EncodeDirectTrainingQuote(q)!=value)return false;
        out=std::move(q);return true;
    }catch(...){return false;}
}
inline bool PartyTrainingQuoteMatches(const Task& task,const TrainingLessonQuote& quote) {
    PartyTrainingJob job;std::string why;
    return IsPartyTrainingTask(task) && ValidatePartyTrainingTask(task,why) &&
        DecodePartyTrainingJob(task.checkpoint.data,job) && job.next<job.lessons.size() &&
        quote.actor==task.actor && DirectFreeTrainingQuote(quote) && job.lessons[job.next]==quote.lesson;
}
inline bool AcknowledgePartyTraining(Task& after,const TrainingLessonQuote& quote,const OperationResult& proof) {
    if(after.phase!=Phase::Verifying || !PartyTrainingQuoteMatches(after,quote) || !IsUuid(proof.id) ||
        proof.task!=after.id || !after.revision || proof.taskRevision!=after.revision-1 ||
        proof.kind!="party_training_learn" || proof.state!=OperationState::Verified ||
        proof.nativeReference!="trainer_lesson:"+std::to_string(quote.lesson) ||
        proof.evidence!="native_training_exact_spellbook_and_unchanged_money")return false;
    PartyTrainingJob job;if(!DecodePartyTrainingJob(after.checkpoint.data,job))return false;
    ++job.next;after.checkpoint.data=EncodePartyTrainingJob(job);
    if(job.next==job.lessons.size())after.phase=Phase::Completed;
    return true;
}
}
