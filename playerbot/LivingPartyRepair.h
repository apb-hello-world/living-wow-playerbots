#pragma once
#include "LivingActivity.h"
namespace LivingActivity {
// Finite maintenance obligation. Native session consent is never persisted here.
inline bool IsPartyRepairTask(const Task& task) {
    return task.kind==Kind::PartyErrand && task.source=="party_repair" &&
        task.checkpoint.data=="{\"workflow\":\"party_repair_v1\"}" &&
        task.root==task.id && task.parent.empty();
}
inline bool ValidatePartyRepairTask(const Task& task,std::string& why) {
    if(task.source!="party_repair")return true;
    if(!IsPartyRepairTask(task)) {why="invalid_party_repair_task";return false;}
    return true;
}
}
