#ifndef LIVING_PROFESSION_NATIVE_H
#define LIVING_PROFESSION_NATIVE_H
#include "LivingProfessionJob.h"
class Player;
namespace LivingActivity {
    // World-thread transition/admission inspection only, not a per-tick scan.
    // No native mutation, database query, material attribution or cast occurs.
    NativeProfessionRecipe InspectNativeProfessionRecipe(Player& actor, const ProfessionJob& job);
    bool ValidateNativeProfessionTask(Player& actor, const Task& task, std::string& blocker);
}
#endif
