#include "LivingActivityOperations.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
namespace LivingActivity {
    namespace {
        bool JsonObject(const std::string& json, size_t limit) {
            if (json.empty() || json.size() > limit) return false;
            const auto first = json.find_first_not_of(" \t\r\n");
            if (first == std::string::npos || json[first] != '{') return false;
            try {
                boost::property_tree::ptree parsed; std::istringstream in(json);
                boost::property_tree::read_json(in, parsed); return true;
            } catch (const std::exception&) { return false; }
        }
    }
    bool ValidateOperationRequest(const OperationRequest& request, const Task& saved,
        const WorldContext& current, const Task* root, uint64_t wallNow, std::string& blocker) {
        const auto& next = request.transition.task;
        if (!request.effects || (request.effects & ~AllEffects) || !IsToken(request.kind, 48) ||
            !JsonObject(request.beforeState, 4096) || next.phase != Phase::Executing ||
            (saved.phase != Phase::Preparing && saved.phase != Phase::Traveling)) {
            blocker = "invalid_native_operation_intent"; return false;
        }
        if (ValidateTaskRequest(request.transition, &saved, current, blocker, root) != AdmissionCode::Pending)
            return false;
        if (!SavedTaskExecutable(saved, request.transition.expectedRevision, current, wallNow, blocker)) return false;
        auto scoped = saved; scoped.ownerGeneration = request.authorization.ownerGeneration;
        if (!scoped.ownerGeneration || !Fresh(scoped, request.authorization, current) ||
            !IsToken(request.authorization.origin) || !request.authorization.operation.empty() ||
            (request.effects & ~request.authorization.permittedEffects)) {
            blocker = "predecessor_authority_required"; return false;
        }
        blocker.clear(); return true;
    }
    WritePlan OperationRequestWrite(const OperationRequest& request) {
        if (!JsonObject(request.beforeState, 4096) || !request.effects || (request.effects & ~AllEffects))
            throw std::invalid_argument("Invalid native operation state/effects");
        const std::string state = "{\"effects\":" + std::to_string(request.effects) + ",\"native\":" + request.beforeState + '}';
        return OperationIntentWrite(request.transition.task, request.transition.expectedRevision,
            request.transition.receipt, request.kind, state);
    }
    bool ValidateNativeObservation(const NativeObservation& result) {
        if (!IsToken(result.evidence) || !JsonObject(result.afterState, 8192) || result.nativeReference.size() > 160)
            return false;
        return (result.state == OperationState::Verified && !result.nativeReference.empty()) ||
            result.state == OperationState::Rejected || result.state == OperationState::Reconciling;
    }
}
