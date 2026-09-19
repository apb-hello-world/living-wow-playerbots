#pragma once
#include <cstdint>
#include <set>
#include <vector>

namespace LivingActivity {
struct TrainingSkillState {
    uint16_t value=0,maximum=0,step=0;
    bool operator==(const TrainingSkillState& b) const {return value==b.value && maximum==b.maximum && step==b.step;}
};
struct TrainingSkillTransition {
    uint16_t id=0;
    TrainingSkillState before,after;
    bool operator==(const TrainingSkillTransition& b) const {return id==b.id && before==b.before && after==b.after;}
};
// One trainer lesson, not an aggregate spell-count change. Shared by legacy
// dispatch and available to the pending durable service adapter; native code
// supplies all facts. This value contract alone is not a persisted receipt.
struct TrainingLessonQuote {
    uint32_t actor=0, lesson=0, teachingSpell=0, money=0, cost=0;
    uint64_t trainer=0, pet=0;
    bool cast=false;
    std::vector<uint32_t> playerSpells, petSpells;
    TrainingSkillTransition skill;
    // Stable database pet identity and exact native training-point/rank delta.
    // Empty for historical owner lessons; never inferred from a pet GUID alone.
    uint32_t petNumber=0,petEntry=0,petLevel=0,petReplacedSpell=0;
    int32_t petPoints=0,petPointCost=0;
};
struct TrainingLessonState {
    uint32_t actor=0, money=0;
    uint64_t pet=0;
    std::set<uint32_t> playerSpells, petSpells;
};
enum class TrainingLessonOutcome { Rejected, Verified, Uncertain };
inline bool ValidTrainingLesson(const TrainingLessonQuote& q) {
    if(!q.actor || !q.trainer || !q.lesson || !q.teachingSpell || q.cost>q.money ||
        q.playerSpells.size()+q.petSpells.size()==0 ||
        q.playerSpells.size()+q.petSpells.size()>3 || (!q.petSpells.empty() && !q.pet))return false;
    for(const auto* spells:{&q.playerSpells,&q.petSpells}) {
        uint32_t previous=0;
        for(const auto spell:*spells) {if(!spell || spell<=previous)return false;previous=spell;}
    }
    return true;
}
inline bool SameTrainingLessonQuote(const TrainingLessonQuote& a,const TrainingLessonQuote& b) {
    return a.actor==b.actor && a.lesson==b.lesson && a.teachingSpell==b.teachingSpell &&
        a.money==b.money && a.cost==b.cost && a.trainer==b.trainer && a.pet==b.pet &&
        a.cast==b.cast && a.playerSpells==b.playerSpells && a.petSpells==b.petSpells && a.skill==b.skill &&
        a.petNumber==b.petNumber && a.petEntry==b.petEntry && a.petLevel==b.petLevel &&
        a.petReplacedSpell==b.petReplacedSpell && a.petPoints==b.petPoints && a.petPointCost==b.petPointCost;
}
inline bool TrainingLessonSubjectMatches(const TrainingLessonQuote& q,const TrainingLessonState& s) {
    return s.actor==q.actor && (q.petSpells.empty() || s.pet==q.pet);
}
inline bool TrainingLessonHasTargets(const TrainingLessonQuote& q,const TrainingLessonState& s) {
    for(const auto spell:q.playerSpells)if(!s.playerSpells.count(spell))return false;
    for(const auto spell:q.petSpells)if(!s.petSpells.count(spell))return false;
    return true;
}
inline bool TrainingLessonReady(const TrainingLessonQuote& q,const TrainingLessonState& before) {
    return ValidTrainingLesson(q) && TrainingLessonSubjectMatches(q,before) &&
        before.money==q.money && !TrainingLessonHasTargets(q,before);
}
inline TrainingLessonOutcome ObserveTrainingLesson(const TrainingLessonQuote& q,
    const TrainingLessonState& before,const TrainingLessonState& after) {
    if(!TrainingLessonReady(q,before) || !TrainingLessonSubjectMatches(q,after))
        return TrainingLessonOutcome::Uncertain;
    if(after.money==q.money-q.cost && TrainingLessonHasTargets(q,after))
        return TrainingLessonOutcome::Verified;
    if(after.money==before.money && after.playerSpells==before.playerSpells &&
        after.petSpells==before.petSpells)return TrainingLessonOutcome::Rejected;
    // Partial effects, unrelated newly learned spells or a payment without the
    // exact lesson are not permission to repeat the native operation.
    return TrainingLessonOutcome::Uncertain;
}
}
