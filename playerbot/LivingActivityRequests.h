#ifndef LIVING_ACTIVITY_REQUESTS_H
#define LIVING_ACTIVITY_REQUESTS_H
#include "LivingActivity.h"

namespace LivingActivity {
    // Internal domain-to-world contract, never a gateway/native-operation RPC.
    // A successful submission means persistence is pending, not permission to
    // execute. The exact acknowledged cache revision is checked again at grant.
    struct TaskRequest {
        Task task;
        uint64_t expectedRevision = 0;
        uint64_t rootRevision = 0; // Required only for a finite direct child.
        std::string receipt;
    };
    enum class AdmissionCode {
        Pending, Saved, Disabled, NotReady, InvalidRequest, StaleRevision,
        StaleContext, ConflictingWrite, Backpressure, ReconciliationRequired
    };
    const char* Name(AdmissionCode code);
    struct AdmissionResult {
        AdmissionCode code = AdmissionCode::InvalidRequest;
        std::string task, blocker;
        uint64_t revision = 0;
    };
    // Pure validation used by the real coordinator and native tests. Native
    // capability/permission checks still belong to the producing domain.
    AdmissionCode ValidateTaskRequest(const TaskRequest& request, const Task* saved,
        const WorldContext& current, std::string& reason, const Task* root = nullptr);
    bool SameRequest(const WritePlan& left, const WritePlan& right);
    // Reserved field in the version-1 child checkpoint; included in the SQL
    // fingerprint and persisted with its domain payload, not an ephemeral hint.
    uint64_t ParentRevision(const Task& child);
    // This excludes queued, paused and uncertain work even if a caller has an
    // older executable copy. Receipts do not survive as execution authority.
    bool SavedTaskExecutable(const Task& saved, uint64_t revision,
        const WorldContext& current, uint64_t wallNow, std::string& reason);
    bool SavedStepExecutable(const Task& step, const Task& root, uint64_t revision,
        const WorldContext& current, uint64_t wallNow, std::string& reason);
}
#endif
