#ifndef LIVING_SERVICE_TRAVEL_H
#define LIVING_SERVICE_TRAVEL_H
#include "LivingActivity.h"
#include <tuple>
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace LivingActivity {
    inline bool LegacyProfessionInFlight(bool casting,bool service,bool savedService,bool paidWindow) {
        return casting || (service && !savedService) || paidWindow;
    }
    enum class ServiceDestination { Mailbox, PersonalBank, CraftingStation, Vendor, PurchaseVendor, AuctionHouse, GuildBank };
    inline const char* ServiceStep(ServiceDestination service) {
        switch (service) {
        case ServiceDestination::Mailbox: return "profession_service_mail";
        case ServiceDestination::PersonalBank: return "profession_service_bank";
        case ServiceDestination::CraftingStation: return "profession_service_station";
        case ServiceDestination::Vendor: return "profession_service_capacity_vendor";
        case ServiceDestination::PurchaseVendor: return "profession_service_purchase_vendor";
        case ServiceDestination::AuctionHouse: return "profession_service_auction";
        case ServiceDestination::GuildBank: return "guild_service_bank";
        }
        return "";
    }
    inline bool ParseServiceStep(const std::string& step,ServiceDestination& service) {
        for (const auto candidate : {ServiceDestination::Mailbox,ServiceDestination::PersonalBank,ServiceDestination::CraftingStation,ServiceDestination::Vendor,ServiceDestination::PurchaseVendor,ServiceDestination::AuctionHouse,ServiceDestination::GuildBank})
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
    // Progress-only saved revisions retain route identity, not an old grant.
    inline bool SameServiceIntent(const Task& before,const Task& after) {
        ServiceDestination service;
        return IsUuid(before.id) && before.id==after.id && before.root==before.id && after.root==after.id &&
            before.actor==after.actor && before.context==after.context && before.mode==Mode::Active &&
            after.mode==Mode::Active && before.accepted && after.accepted &&
            before.phase==Phase::Traveling && after.phase==Phase::Traveling && before.revision<=after.revision &&
            before.checkpoint.step==after.checkpoint.step && ParseServiceStep(after.checkpoint.step,service) &&
            before.checkpoint.data==after.checkpoint.data;
    }
    struct ServicePathPoint { uint32_t map=0; double x=0,y=0,z=0; };
    // A failed graph connection may still leave a genuine local navmesh path.
    // Retain that prefix, never a fabricated destination or a start-only path.
    // This proves a walking leg, not arrival, reachability of the final service,
    // permission to teleport, or completion of the owning task.
    template<class PointAt>
    bool ReachableServicePrefix(const ServicePathPoint& here,size_t count,PointAt pointAt,double minimumProgress) {
        const auto finite=[](const ServicePathPoint& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
        if(!finite(here) || count<2 || count>4096 || !std::isfinite(minimumProgress) || minimumProgress<=0)return false;
        for(size_t n=0;n<count;++n) {
            const auto p=pointAt(n);
            if(p.map!=here.map || !finite(p))return false;
        }
        const auto first=pointAt(0),last=pointAt(count-1);
        return std::hypot(first.x-here.x,first.y-here.y,first.z-here.z)<=minimumProgress &&
            std::hypot(last.x-here.x,last.y-here.y,last.z-here.z)>minimumProgress;
    }
    // Service routes end at a reachable interaction position, not necessarily
    // the NPC's centre (which can sit above the navmesh). This is geometry only;
    // the caller must prove its current owner and native service access again.
    inline bool ServiceInteractionApproach(const ServicePathPoint& end,const ServicePathPoint& service,double range) {
        if(end.map!=service.map || !std::isfinite(range) || range<=0 ||
            !std::isfinite(end.x) || !std::isfinite(end.y) || !std::isfinite(end.z) ||
            !std::isfinite(service.x) || !std::isfinite(service.y) || !std::isfinite(service.z))return false;
        return std::hypot(end.x-service.x,end.y-service.y,end.z-service.z)<=range;
    }
    // Project onto the native remaining path: a valid road/dock approach can
    // initially lead away from the final goal. No new pathfinding or pointers.
    template<class PointAt>
    std::optional<double> ServicePathRemaining(const ServicePathPoint& here,size_t count,PointAt pointAt) {
        const auto finite=[](const ServicePathPoint& p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);};
        const auto distance=[](const ServicePathPoint& a,const ServicePathPoint& b){
            const double x=a.x-b.x,y=a.y-b.y,z=a.z-b.z;return std::sqrt(x*x+y*y+z*z);};
        if(!finite(here) || !count || count>4096) return std::nullopt;
        double tail=0,best=std::numeric_limits<double>::infinity(),lateral=best;
        const auto observe=[&](double offset,double remaining){
            if(offset<lateral-0.01 || (std::abs(offset-lateral)<=0.01 && remaining>best)) {
                lateral=offset;best=remaining;
            }
        };
        auto next=pointAt(count-1);if(!finite(next))return std::nullopt;
        for(size_t n=count;n>0;--n) {
            const auto p=pointAt(n-1);if(!finite(p))return std::nullopt;
            if(n<count && p.map==next.map) {
                const double length=distance(p,next);
                if(p.map==here.map && length>0) {
                    const double t=std::clamp(((here.x-p.x)*(next.x-p.x)+(here.y-p.y)*(next.y-p.y)+
                        (here.z-p.z)*(next.z-p.z))/(length*length),0.0,1.0);
                    const ServicePathPoint projection{p.map,p.x+t*(next.x-p.x),p.y+t*(next.y-p.y),p.z+t*(next.z-p.z)};
                    observe(distance(here,projection),(1-t)*length+tail);
                }
                tail+=length;
            }
            if(p.map==here.map)observe(distance(here,p),tail);
            next=p;
        }
        return std::isfinite(best)?std::optional<double>(best+lateral):std::nullopt;
    }
    class ServicePathProgress {
    public:
        bool Observe(double remaining) {
            if(!std::isfinite(remaining)||remaining<0)return false;
            if(!std::isfinite(best)){best=remaining;return false;}
            if(remaining+2>=best)return false;
            best=remaining;return true;
        }
    private:
        double best=std::numeric_limits<double>::infinity();
    };
}
#endif
