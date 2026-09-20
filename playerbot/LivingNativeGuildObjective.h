#pragma once
#include "LivingActivity.h"
#include <string>
class Player;
namespace ai {class TravelTarget;}
namespace LivingActivity {
std::string AdvanceNativeGuildQuestObjective(Player&,const Task&,const ActionContext&,ai::TravelTarget&);
}
