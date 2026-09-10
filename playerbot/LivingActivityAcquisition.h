#ifndef LIVING_ACTIVITY_ACQUISITION_H
#define LIVING_ACTIVITY_ACQUISITION_H
#include "LivingActivityAuthority.h"
#include "LivingActivityRequests.h"

namespace LivingActivity {
    // Admission/lease waiting is NOT cancellation or a failed native attempt.
    // Deliberately no implicit bool conversion: domain callers must decide how
    // to preserve their accepted work on each outcome.
    enum class AcquisitionState { Granted, Waiting, Invalidated, LegacyAllowed };
    struct Acquisition {
        AcquisitionState state = AcquisitionState::Invalidated;
        std::string blocker = "invalid_activity_request";
        bool Permitted() const {
            return state == AcquisitionState::Granted || state == AcquisitionState::LegacyAllowed;
        }
        bool Waiting() const { return state == AcquisitionState::Waiting; }
    };
    inline Acquisition AcquisitionFrom(AdmissionCode code) {
        switch (code) {
        case AdmissionCode::Pending: case AdmissionCode::NotReady:
        case AdmissionCode::ConflictingWrite: case AdmissionCode::Backpressure:
        case AdmissionCode::ReconciliationRequired:
            return {AcquisitionState::Waiting, Name(code)};
        // Saved means only the write was acknowledged. A separate lease check
        // must still succeed; it is never permission to begin native work.
        case AdmissionCode::Saved: return {AcquisitionState::Waiting, "lease_required"};
        default: return {AcquisitionState::Invalidated, Name(code)};
        }
    }
    inline Acquisition AcquisitionFrom(AuthorityCode code) {
        switch (code) {
        case AuthorityCode::Granted: case AuthorityCode::Renewed: case AuthorityCode::Preempted:
            return {AcquisitionState::Granted, ""};
        case AuthorityCode::SafetyPaused: case AuthorityCode::AtomicPending:
        case AuthorityCode::PriorityDenied: case AuthorityCode::ReconciliationRequired:
        case AuthorityCode::Capacity:
            return {AcquisitionState::Waiting, Name(code)};
        default: return {AcquisitionState::Invalidated, Name(code)};
        }
    }
}
#endif
