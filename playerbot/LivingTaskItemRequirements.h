#pragma once
#include "LivingProfessionJob.h"
#include "LivingRecipeLearning.h"

namespace LivingActivity {
    // A typed projection of an already accepted job, not a synthetic recipe or
    // a new planner. Shared service adapters never invent their own item need.
    inline bool ReadTaskItemRequirements(const Task& task,std::vector<ProfessionReagent>& items,std::string& blocker) {
        items.clear();
        if (IsRecipeLearningTask(task)) {
            RecipeLearningJob job;
            if (!ValidateRecipeLearningTask(task,blocker) || !DecodeRecipeLearningJob(task.checkpoint.data,job,blocker)) return false;
            items.push_back({job.book,1});blocker.clear();return true;
        }
        ProfessionJob job;
        if (!IsProfessionJob(task) || !ValidateProfessionTask(task,blocker) ||
            !DecodeProfessionJob(task.checkpoint.data,job,blocker)) {
            if (blocker.empty()) blocker="item_service_typed_demand_required";return false;
        }
        items=job.reagents;blocker.clear();return true;
    }
}
