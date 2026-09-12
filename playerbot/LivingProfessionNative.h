#ifndef LIVING_PROFESSION_NATIVE_H
#define LIVING_PROFESSION_NATIVE_H
#include "LivingProfessionJob.h"
#include "LivingProfessionEvidence.h"
class Player;
namespace LivingActivity {
    // World-thread transition/admission inspection only, not a per-tick scan.
    // No native mutation, database query, material attribution or cast occurs.
    NativeProfessionRecipe InspectNativeProfessionRecipe(Player& actor, const ProfessionJob& job);
    bool BuildNativeSkillGainJob(Player& actor,uint32_t recipe,ProfessionJob& job,std::string& blocker);
    bool ValidateNativeProfessionTask(Player& actor, const Task& task, std::string& blocker);
    // Once per native catalog load. Runtime work inspects this compact index,
    // never scans all item templates for each bot or tick.
    void BuildNativeProfessionToolCatalog();
    bool ReadNativeProfessionTools(Player& actor,const Task& task,
        std::vector<ProfessionReagent>& tools,std::string& unavailable,std::string& blocker);
    bool ReadNativeTaskItemRequirements(Player& actor,const Task& task,
        std::vector<ProfessionReagent>& items,std::string& blocker,std::string* unavailable=nullptr);
    // Fresh world-thread composition of acknowledged history and actual native
    // possessions/readiness. Never acquires a lease or changes a task, spell,
    // inventory, group, route or database. Call only for a selected due step.
    bool InspectNativeProfessionSnapshot(Player& actor,const Task& task,const ProfessionHistory& history,
        uint64_t nowMs,ProfessionSnapshot& snapshot,std::string& blocker);
}
#endif
