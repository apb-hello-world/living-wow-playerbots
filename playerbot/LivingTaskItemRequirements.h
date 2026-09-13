#pragma once
#include "LivingProfessionJob.h"
#include "LivingRecipeLearning.h"
#include "LivingGuildDelivery.h"

namespace LivingActivity {
    // A typed projection of an already accepted job, not a synthetic recipe or
    // a new planner. Shared service adapters never invent their own item need.
    inline bool ReadTaskItemRequirements(const Task& task,std::vector<ProfessionReagent>& items,std::string& blocker) {
        items.clear();
        if (IsManagedGuildDelivery(task)) {
            GuildDeliveryJob job;
            if (!ValidateGuildDeliveryTask(task,blocker) || !DecodeGuildDeliveryJob(task.checkpoint.data,job,blocker) || job.money)
                return false;
            items.push_back({job.entry,job.quantity});blocker.clear();return true;
        }
        if (IsRecipeLearningTask(task)) {
            RecipeLearningJob job;
            if (!ValidateRecipeLearningTask(task,blocker) || !DecodeRecipeLearningJob(task.checkpoint.data,job,blocker)) return false;
            items.push_back({job.book,1});blocker.clear();return true;
        }
        ProfessionJob job;
        if (!IsProfessionJob(task) || !ValidateProfessionTask(task,blocker) ||
            !DecodeProfessionJob(task.checkpoint.data,job,blocker)) {
            if (blocker.empty()) blocker="item_service_typed_demand_required";
            return false;
        }
        items=job.reagents;blocker.clear();return true;
    }
    inline bool MatchesRecipeBookPurchase(const Task& task,uint32_t entry,uint32_t quantity,std::string& blocker) {
        std::vector<ProfessionReagent> required;
        if (!IsRecipeLearningTask(task) || !ReadTaskItemRequirements(task,required,blocker)) return false;
        if (required.size()!=1 || required.front().entry!=entry || quantity!=1) {
            blocker="vendor_purchase_not_the_learning_book";return false;
        }
        blocker.clear();return true;
    }
}
