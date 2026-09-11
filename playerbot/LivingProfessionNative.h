#ifndef LIVING_PROFESSION_NATIVE_H
#define LIVING_PROFESSION_NATIVE_H
#include "LivingProfessionJob.h"
#include "LivingProfessionEvidence.h"
class Player;
namespace LivingActivity {
    // World-thread transition/admission inspection only, not a per-tick scan.
    // No native mutation, database query, material attribution or cast occurs.
    NativeProfessionRecipe InspectNativeProfessionRecipe(Player& actor, const ProfessionJob& job);
    bool ValidateNativeProfessionTask(Player& actor, const Task& task, std::string& blocker);
    // Fresh world-thread composition of acknowledged history and actual native
    // possessions/readiness. Never acquires a lease or changes a task, spell,
    // inventory, group, route or database. Call only for a selected due step.
    bool InspectNativeProfessionSnapshot(Player& actor,const Task& task,const ProfessionHistory& history,
        uint64_t nowMs,ProfessionSnapshot& snapshot,std::string& blocker);
}
#endif
