#include "LivingActivityAcquisition.h"
#include "LivingActivityWorkClock.h"
#include "LivingServiceTravel.h"
#include <cassert>
#include <limits>
#include <type_traits>
using namespace LivingActivity;
int main() {
    {
        assert(!LegacyProfessionInFlight(false,true,true,false)); // A saved trip must not deadlock its own runner.
        assert(LegacyProfessionInFlight(false,true,false,false));
        assert(LegacyProfessionInFlight(true,true,true,false));
        assert(LegacyProfessionInFlight(false,true,true,true));
        // The shared four-trip admission queue is FIFO within a priority band,
        // not GUID order. Unsafe/paused waiters do not hold the front forever.
        std::vector<ServiceQueueEntry> queue{
            {900,Priority::Progression,1,true,true},{800,Priority::Progression,2,true,true},
            {700,Priority::Progression,3,true,true},{600,Priority::Progression,4,true,true},
            {500,Priority::Progression,5,true,false},{1,Priority::Progression,6,true,false}};
        assert(ServiceSlotAvailable(queue[0],queue));
        assert(!ServiceSlotAvailable(queue[4],queue) && !ServiceSlotAvailable(queue[5],queue));
        queue.erase(queue.begin());
        assert(ServiceSlotAvailable(queue[3],queue) && !ServiceSlotAvailable(queue[4],queue));
        queue[3].ready=false;
        assert(!ServiceSlotAvailable(queue[3],queue) && ServiceSlotAvailable(queue[4],queue));
        queue[3].ready=true;queue[4].priority=Priority::Delivery;
        assert(ServiceSlotAvailable(queue[4],queue) && !ServiceSlotAvailable(queue[3],queue));
        auto forged=queue[4];forged.running=true;assert(!ServiceSlotAvailable(forged,queue));
        queue.push_back(queue[4]);assert(!ServiceSlotAvailable(queue[4],queue));
        ServiceDestination service;
        assert(ParseServiceStep("profession_service_mail",service) && service==ServiceDestination::Mailbox);
        assert(ParseServiceStep("profession_service_bank",service) && service==ServiceDestination::PersonalBank);
        assert(ParseServiceStep("profession_service_capacity_vendor",service) && service==ServiceDestination::Vendor);
        assert(!ParseServiceStep("profession_completed",service));
        Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49f38";
        task.actor=11;task.revision=5;task.mode=Mode::Active;task.accepted=true;task.phase=Phase::Traveling;
        task.context.actor=11;task.context.mapGeneration=3;
        ActivityLease lease{11,task.id,7,task.context};
        ActionContext action;action.task=action.rootTask=task.id;action.revision=5;action.ownerGeneration=7;action.world=task.context;
        assert(SameServiceSearch(task,action,lease,5));
        assert(!SameServiceSearch(task,action,lease,4));
        auto changed=lease;++changed.generation;assert(!SameServiceSearch(task,action,changed,5));
        changed=lease;++changed.context.mapGeneration;assert(!SameServiceSearch(task,action,changed,5));
        changed=lease;changed.rootTask="637bd562-36d2-5b01-bc01-e2d831c49f39";assert(!SameServiceSearch(task,action,changed,5));
        auto moved=task;++moved.revision;assert(!SameServiceSearch(moved,action,lease,5));
        moved=task;moved.phase=Phase::Paused;assert(!SameServiceSearch(moved,action,lease,5));
        assert(!CanTransition(moved,Phase::Traveling));
        assert(CanTransition(moved,Phase::Reconciling));
        moved.phase=Phase::Reconciling;assert(CanTransition(moved,Phase::Preparing));
        moved.phase=Phase::Preparing;assert(CanTransition(moved,Phase::Traveling));
        task.checkpoint.step="profession_service_bank";task.checkpoint.data="exact_saved_job";
        moved=task;++moved.revision;moved.checkpoint.activeElapsedMs+=30000;
        assert(SameServiceIntent(task,moved));
        assert(!SameServiceSearch(moved,action,lease,task.revision)); // Fresh grant still required.
        auto other=moved;other.checkpoint.step="profession_service_mail";assert(!SameServiceIntent(task,other));
        other=moved;other.checkpoint.data="different_job";assert(!SameServiceIntent(task,other));
        other=moved;other.phase=Phase::Paused;assert(!SameServiceIntent(task,other));
        other=moved;++other.context.mapGeneration;assert(!SameServiceIntent(task,other));
        other=moved;other.accepted=false;assert(!SameServiceIntent(task,other));
        other=moved;other.revision=task.revision-1;assert(!SameServiceIntent(task,other));
        WorkClock resumed(900000);resumed.Observe(1000,true);resumed.Observe(2000,true);
        assert(resumed.ActiveMs()==901000 && resumed.NoProgressMs()==1000);
    }
    {
        const std::vector<ServicePathPoint> road{{1,0,0,0},{1,-100,0,0},{1,-100,100,0},{1,100,100,0},{1,100,0,0}};
        auto remaining=[&](ServicePathPoint here){return ServicePathRemaining(here,road.size(),[&](size_t n){return road[n];});};
        assert(remaining({1,0,0,0})==500);
        assert(remaining({1,-20,0,0})==480); // Further from goal, but forward on road.
        assert(remaining({1,-100,30,0})==370);
        assert(remaining({1,100,10,0})==10);
        assert(!remaining({530,0,0,0}));
        assert(!remaining({1,std::numeric_limits<double>::quiet_NaN(),0,0}));
        assert(!ServicePathRemaining(road[0],4097,[&](size_t n){return road.at(n);}));
        ServicePathProgress progress;
        assert(!progress.Observe(*remaining(road[0]))); // Installing a route is not progress.
        assert(progress.Observe(*remaining({1,-20,0,0})));
        for(unsigned n=0;n<20;++n) { // Circling/repeating the same segment earns no progress.
            assert(!progress.Observe(*remaining(road[0])));
            assert(!progress.Observe(*remaining({1,-20,0,0})));
        }
        assert(progress.Observe(*remaining({1,-100,30,0})));
        assert(!progress.Observe(-1));assert(!progress.Observe(std::numeric_limits<double>::infinity()));
        const std::vector<ServicePathPoint> crossing{{1,0,0,0},{1,10,0,0},{0,0,0,0},{0,20,0,0}};
        const auto after=ServicePathRemaining({0,5,0,0},crossing.size(),[&](size_t n){return crossing[n];});
        assert(after==15); // No invented walking distance between maps.
    }
    static_assert(!std::is_convertible<Acquisition,bool>::value,"Waiting must not collapse into failure");
    for(auto code:{AdmissionCode::Pending,AdmissionCode::NotReady,AdmissionCode::ConflictingWrite,
        AdmissionCode::Backpressure,AdmissionCode::ReconciliationRequired,AdmissionCode::Saved}) {
        const auto result=AcquisitionFrom(code);
        assert(result.Waiting() && !result.Permitted() && !result.blocker.empty());
    }
    for(auto code:{AdmissionCode::Disabled,AdmissionCode::InvalidRequest,AdmissionCode::StaleRevision,
        AdmissionCode::StaleContext}) {
        const auto result=AcquisitionFrom(code);
        assert(!result.Waiting() && !result.Permitted());
    }
    for(auto code:{AuthorityCode::SafetyPaused,AuthorityCode::AtomicPending,AuthorityCode::PriorityDenied,
        AuthorityCode::ReconciliationRequired,AuthorityCode::Capacity}) {
        const auto result=AcquisitionFrom(code);
        assert(result.Waiting() && !result.Permitted() && !result.blocker.empty());
    }
    for(auto code:{AuthorityCode::Granted,AuthorityCode::Renewed,AuthorityCode::Preempted})
        assert(AcquisitionFrom(code).Permitted());
    // Read/check success is not an acquisition receipt either.
    for(auto code:{AuthorityCode::Allowed,AuthorityCode::NoOwner,AuthorityCode::StaleContext,
        AuthorityCode::StaleLease,AuthorityCode::StaleRevision,AuthorityCode::EffectsDenied})
        assert(!AcquisitionFrom(code).Permitted());
    WorkClock work;
    work.Observe(1000,false);work.Observe(3601000,false);
    assert(work.ActiveMs()==0 && work.NoProgressMs()==0);
    work.Observe(3601000,true);work.Observe(3611000,true);
    assert(work.ActiveMs()==10000 && work.NoProgressMs()==10000);
    work.Progress();
    work.Observe(3621000,true);
    assert(work.ActiveMs()==20000 && work.NoProgressMs()==10000);
    work.Observe(3631000,false); // A newly observed interruption accrues nothing.
    work.Observe(9999999,false);
    assert(!work.Running() && work.ActiveMs()==20000 && work.NoProgressMs()==10000);
    work.Observe(10000000,true);work.Observe(10001000,true);
    assert(work.ActiveMs()==21000 && work.NoProgressMs()==11000);
    work.Observe(10,true); // Defensive monotonic-clock regression is not wraparound.
    assert(work.ActiveMs()==21000);
    work.Observe(std::numeric_limits<uint64_t>::max(),true);
    assert(work.ActiveMs()==std::numeric_limits<uint64_t>::max());
    work.Progress();assert(work.NoProgressMs()==0);
}
