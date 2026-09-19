#pragma once
#include "LivingActivity.h"
#include "LivingTrainingLesson.h"
#include "LivingServiceTravel.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <algorithm>
#include <map>
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
    return ValidTrainingLesson(q) && !q.cast && !q.cost && !q.skill.id && q.skill==TrainingSkillTransition{} && q.teachingSpell==q.lesson &&
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
// Shape only. Native admission additionally validates every teaching effect,
// its target and resources. Skill tiers and pet lessons need different proofs.
inline bool FreePlayerTrainingCastQuote(const TrainingLessonQuote& q) {
    const auto& s=q.skill;
    if(s.id) {
        if(!s.after.step || s.after.step>6 || s.before.step>s.after.step || !s.after.value ||
            s.before.value>s.before.maximum || s.after.value>s.after.maximum ||
            s.after.maximum<s.before.maximum || s.after.value<s.before.value)return false;
    } else if(!(s==TrainingSkillTransition{}))return false;
    return ValidTrainingLesson(q) && q.cast && !q.cost && q.teachingSpell==q.lesson &&
        !q.playerSpells.empty() && q.petSpells.empty() &&
        std::find(q.playerSpells.begin(),q.playerSpells.end(),q.lesson)==q.playerSpells.end();
}
inline bool ManagedTrainingQuote(const TrainingLessonQuote& q) {
    return DirectFreeTrainingQuote(q) || FreePlayerTrainingCastQuote(q);
}
// Native _SaveSpells omits dependent abilities. Each omitted target must be
// reachable through native non-auto learning edges from a saved quoted target.
// No arbitrary known spell, unrooted cycle or missing row counts as proof.
inline bool TrainingPersistenceTargets(const std::set<uint32_t>& present,const std::set<uint32_t>& dependent,
    const std::vector<std::pair<uint32_t,uint32_t>>& edges,std::set<uint32_t>& saved) {
    saved.clear();if(present.size()>3 || edges.size()>9)return false;
    for(auto id:dependent)if(!present.count(id))return false;
    for(auto id:present) {if(!id)return false;if(!dependent.count(id))saved.insert(id);}
    auto reached=saved;
    for(const auto& edge:edges)if(!present.count(edge.first) || !present.count(edge.second))return false;
    for(size_t n=0;n<present.size();++n)
        for(const auto& edge:edges)if(reached.count(edge.first))reached.insert(edge.second);
    return reached==present;
}
inline std::string EncodePartyTrainingQuote(const TrainingLessonQuote& q) {
    if(!q.cast)return EncodeDirectTrainingQuote(q); // Historical journal fingerprint.
    std::string out="{\"workflow\":\""+std::string(q.skill.id?"training_cast_v2":"training_cast_v1")+"\",\"actor\":"+std::to_string(q.actor)+
        ",\"trainer\":"+std::to_string(q.trainer)+",\"lesson\":"+std::to_string(q.lesson)+
        ",\"money\":"+std::to_string(q.money)+",\"pet\":"+std::to_string(q.pet)+",\"targets\":[";
    for(size_t i=0;i<q.playerSpells.size();++i){if(i)out+=',';out+=std::to_string(q.playerSpells[i]);}
    out+=']';
    if(q.skill.id) {
        const auto& s=q.skill;
        out+=",\"skill\":{\"id\":"+std::to_string(s.id)+",\"before_value\":"+std::to_string(s.before.value)+
            ",\"before_maximum\":"+std::to_string(s.before.maximum)+",\"before_step\":"+std::to_string(s.before.step)+
            ",\"after_value\":"+std::to_string(s.after.value)+",\"after_maximum\":"+std::to_string(s.after.maximum)+
            ",\"after_step\":"+std::to_string(s.after.step)+'}';
    }
    return out+'}';
}
inline bool DecodePartyTrainingQuote(const std::string& value,TrainingLessonQuote& out) {
    if(DecodeDirectTrainingQuote(value,out))return true;
    out={};if(value.size()>512)return false;
    try {
        boost::property_tree::ptree p;std::istringstream in(value);boost::property_tree::read_json(in,p);
        const auto workflow=p.get<std::string>("workflow");
        if(workflow!="training_cast_v1" && workflow!="training_cast_v2")return false;
        TrainingLessonQuote q;q.cast=true;q.actor=p.get<uint32_t>("actor");q.trainer=p.get<uint64_t>("trainer");
        q.lesson=q.teachingSpell=p.get<uint32_t>("lesson");q.money=p.get<uint32_t>("money");q.pet=p.get<uint64_t>("pet");
        for(const auto& row:p.get_child("targets")) {
            if(!row.first.empty() || q.playerSpells.size()>=3)return false;
            q.playerSpells.push_back(row.second.get_value<uint32_t>());
        }
        if(workflow=="training_cast_v2") {
            const auto& s=p.get_child("skill");q.skill.id=s.get<uint16_t>("id");
            q.skill.before={s.get<uint16_t>("before_value"),s.get<uint16_t>("before_maximum"),s.get<uint16_t>("before_step")};
            q.skill.after={s.get<uint16_t>("after_value"),s.get<uint16_t>("after_maximum"),s.get<uint16_t>("after_step")};
        }
        if(!FreePlayerTrainingCastQuote(q) || EncodePartyTrainingQuote(q)!=value)return false;
        out=std::move(q);return true;
    }catch(...){return false;}
}
inline const char* PartyTrainingEvidence(const TrainingLessonQuote& q) {
    return q.skill.id?"native_training_exact_cast_skill_and_spellbook":
        q.cast?"native_training_exact_cast_and_spellbook":"native_training_exact_spellbook_and_unchanged_money";
}
inline bool VerifiedPartyTrainingEvidence(const std::string& evidence) {
    return evidence=="native_training_exact_cast_skill_and_spellbook" || evidence=="native_training_exact_cast_and_spellbook" ||
        evidence=="native_training_exact_spellbook_and_unchanged_money";
}
inline bool SameTrainingState(const TrainingLessonState& a,const TrainingLessonState& b) {
    return a.actor==b.actor && a.money==b.money && a.pet==b.pet &&
        a.playerSpells==b.playerSpells && a.petSpells==b.petSpells;
}
struct TrainingCastResult {
    TrainingLessonState before,after;
    std::map<uint16_t,TrainingSkillState> skillsBefore,skillsAfter;
    bool started=false,effect=false,finished=false,succeeded=false,uncertain=false;
};
inline OperationState VerifyTrainingCast(const TrainingLessonQuote& q,const TrainingCastResult& r,std::string& why) {
    why="native_training_cast_requires_reconciliation";
    if(!FreePlayerTrainingCastQuote(q) || !TrainingLessonReady(q,r.before) || !r.started || !r.finished || r.uncertain)
        return OperationState::Reconciling;
    if(!r.effect && !r.succeeded && SameTrainingState(r.before,r.after) && r.skillsBefore==r.skillsAfter) {
        why="native_training_cast_rejected_without_effect";return OperationState::Rejected;
    }
    auto expected=r.before;expected.playerSpells.insert(q.playerSpells.begin(),q.playerSpells.end());
    auto expectedSkills=r.skillsBefore;
    if(q.skill.id) {
        const auto found=r.skillsBefore.find(q.skill.id);
        const auto before=found==r.skillsBefore.end()?TrainingSkillState{}:found->second;
        if(!(before==q.skill.before))return OperationState::Reconciling;
        expectedSkills[q.skill.id]=q.skill.after;
    }
    if(!r.effect || !r.succeeded || !SameTrainingState(expected,r.after) || expectedSkills!=r.skillsAfter)return OperationState::Reconciling;
    why=PartyTrainingEvidence(q);return OperationState::Verified;
}
inline bool PartyTrainingQuoteMatches(const Task& task,const TrainingLessonQuote& quote) {
    PartyTrainingJob job;std::string why;
    return IsPartyTrainingTask(task) && ValidatePartyTrainingTask(task,why) &&
        DecodePartyTrainingJob(task.checkpoint.data,job) && job.next<job.lessons.size() &&
        quote.actor==task.actor && ManagedTrainingQuote(quote) && job.lessons[job.next]==quote.lesson;
}
inline bool AcknowledgePartyTraining(Task& after,const TrainingLessonQuote& quote,const OperationResult& proof) {
    if(after.phase!=Phase::Verifying || !PartyTrainingQuoteMatches(after,quote) || !IsUuid(proof.id) ||
        proof.task!=after.id || !after.revision || proof.taskRevision!=after.revision-1 ||
        proof.kind!="party_training_learn" || proof.state!=OperationState::Verified ||
        proof.nativeReference!="trainer_lesson:"+std::to_string(quote.lesson) ||
        proof.evidence!=PartyTrainingEvidence(quote))return false;
    PartyTrainingJob job;if(!DecodePartyTrainingJob(after.checkpoint.data,job))return false;
    ++job.next;after.checkpoint.data=EncodePartyTrainingJob(job);
    if(job.next==job.lessons.size())after.phase=Phase::Completed;
    return true;
}
}
