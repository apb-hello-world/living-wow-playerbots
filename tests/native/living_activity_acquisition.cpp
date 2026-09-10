#include "LivingActivityAcquisition.h"
#include "LivingActivityWorkClock.h"
#include <cassert>
#include <limits>
#include <type_traits>
using namespace LivingActivity;
int main() {
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
