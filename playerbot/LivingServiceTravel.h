#ifndef LIVING_SERVICE_TRAVEL_H
#define LIVING_SERVICE_TRAVEL_H
#include "LivingActivity.h"
#include <tuple>

namespace LivingActivity {
    inline bool LegacyProfessionInFlight(bool casting,bool service,bool savedService,bool paidWindow) {
        return casting || (service && !savedService) || paidWindow;
    }
    enum class ServiceDestination { Mailbox, PersonalBank, CraftingStation };
    inline const char* ServiceStep(ServiceDestination service) {
        switch (service) {
        case ServiceDestination::Mailbox: return "profession_service_mail";
        case ServiceDestination::PersonalBank: return "profession_service_bank";
        case ServiceDestination::CraftingStation: return "profession_service_station";
        }
        return "";
    }
    inline bool ParseServiceStep(const std::string& step,ServiceDestination& service) {
        for (const auto candidate : {ServiceDestination::Mailbox,ServiceDestination::PersonalBank,ServiceDestination::CraftingStation})
            if (step==ServiceStep(candidate)) {service=candidate;return true;}
        return false;
    }
    struct ServiceTravelResult {
        bool arrived=false;
        std::string blocker;
        uint64_t activeElapsedMs=0,retryAtMs=0;
    };
    // Admission ordering only, not another scheduler or execution authority.
    // Both legacy and saved trips occupy the existing economy service queue.
    struct ServiceQueueEntry {
        uint32_t actor=0;
        Priority priority=Priority::Progression;
        uint64_t ticket=0;
        bool ready=false,running=false;
    };
    inline bool ServiceSlotAvailable(const ServiceQueueEntry& candidate,const std::vector<ServiceQueueEntry>& queue) {
        if (!candidate.actor || !candidate.ticket || !candidate.ready) return false;
        size_t running=0,ahead=0,matches=0;
        const auto rank=[](const ServiceQueueEntry& e){return std::make_tuple(e.priority,e.ticket,e.actor);};
        for (const auto& e : queue) {
            if (e.actor==candidate.actor) {
                if(e.ticket!=candidate.ticket || e.priority!=candidate.priority || e.ready!=candidate.ready || e.running!=candidate.running)
                    return false;
                ++matches;
            }
            if (e.running) ++running;
            else if (e.ready && e.actor!=candidate.actor && rank(e)<rank(candidate)) ++ahead;
        }
        return matches==1 && (candidate.running ? running<=4 : running<4 && ahead<4-running);
    }
    inline bool SameServiceSearch(const Task& saved,const ActionContext& action,const ActivityLease& route,uint64_t revision) {
        return IsUuid(saved.id) && route.generation && saved.mode==Mode::Active && saved.accepted && saved.phase==Phase::Traveling &&
            saved.actor==route.actor && saved.id==route.rootTask && saved.root==saved.id &&
            saved.revision==revision && saved.context==route.context &&
            action.task==saved.id && action.rootTask==saved.id && action.revision==revision &&
            action.ownerGeneration==route.generation && action.world==route.context;
    }
}
#endif
