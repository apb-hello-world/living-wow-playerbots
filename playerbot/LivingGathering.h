#pragma once
#include <cstdint>

namespace LivingActivity {
// Native level/lock/tool/ownership checks still decide whether an individual
// node can be used. This distinguishes two reasons to consider its skill band.
enum class GatheringIntent : uint8_t { SkillGain, RequestedMaterials };
enum class GatheringSkillResult : uint8_t { Eligible, UnknownSkill, InsufficientSkill,
    SkillCapped, NoSkillGain, UnsupportedIntent };

inline GatheringSkillResult CheckGatheringSkill(uint32_t current,uint32_t maximum,uint32_t required,
    bool fishing,GatheringIntent intent) {
    if(intent!=GatheringIntent::SkillGain && intent!=GatheringIntent::RequestedMaterials)
        return GatheringSkillResult::UnsupportedIntent;
    // Requested fishing needs its own cast/loot outcome adapter. Do not silently
    // advertise it merely because the existing skill-up travel can find water.
    if(fishing && intent==GatheringIntent::RequestedMaterials)return GatheringSkillResult::UnsupportedIntent;
    if(!current)return GatheringSkillResult::UnknownSkill;
    if(required>current)return GatheringSkillResult::InsufficientSkill;
    // A real material request remains useful at the profession cap or on a
    // grey node. Skill gain is neither its goal nor its completion predicate.
    if(intent==GatheringIntent::RequestedMaterials)return GatheringSkillResult::Eligible;
    if(maximum<=current)return GatheringSkillResult::SkillCapped;
    if(!fishing && uint64_t(required)+100<current)return GatheringSkillResult::NoSkillGain;
    return GatheringSkillResult::Eligible;
}
}
