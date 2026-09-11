#include "LivingActivityOperations.h"
#include <boost/property_tree/json_parser.hpp>
#include <sstream>
#include <tuple>
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
        std::string NativeBefore(const OperationRequest& request) {
            if (request.consumption.empty()) return request.beforeState;
            for (const auto& use : request.consumption) {
                const auto& c=use.before; const auto& task=request.transition.task;
                const auto required=c.copper ? Mask(Effect::Money) : Mask(Effect::Inventory);
                if (c.actor != task.actor || (c.task != task.id && c.task != task.root) ||
                    (required & ~request.effects)) throw std::invalid_argument("Claim owner/effect mismatch");
            }
            return ClaimedNativeState(request.beforeState,request.consumption);
        }
    }
    bool ValidateOperationRequest(const OperationRequest& request, const Task& saved,
        const WorldContext& current, const Task* root, uint64_t wallNow, std::string& blocker) {
        const auto& next = request.transition.task;
        if (!request.effects || (request.effects & ~AllEffects) ||
            unsigned(request.persistence) > unsigned(NativePersistence::Profession) || !IsToken(request.kind, 48) ||
            !JsonObject(request.beforeState, 4096) || next.phase != Phase::Executing ||
            (saved.phase != Phase::Preparing && saved.phase != Phase::Traveling)) {
            blocker = "invalid_native_operation_intent"; return false;
        }
        try { NativeBefore(request); }
        catch (const std::exception&) { blocker="invalid_claimed_consumption"; return false; }
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
        if (!JsonObject(request.beforeState, 4096) || !request.effects || (request.effects & ~AllEffects) ||
            unsigned(request.persistence) > unsigned(NativePersistence::Profession))
            throw std::invalid_argument("Invalid native operation state/effects");
        const std::string state = "{\"effects\":" + std::to_string(request.effects) +
            ",\"persistence\":" + std::to_string(unsigned(request.persistence)) + ",\"native\":" + NativeBefore(request) + '}';
        return OperationIntentWrite(request.transition.task, request.transition.expectedRevision,
            request.transition.receipt, request.kind, state);
    }
    bool ValidateNativeObservation(const NativeObservation& result) {
        if (!IsToken(result.evidence) || !JsonObject(result.afterState, 8192) || result.nativeReference.size() > 160)
            return false;
        return (result.state == OperationState::Verified && !result.nativeReference.empty()) ||
            result.state == OperationState::Rejected || result.state == OperationState::Reconciling;
    }
    bool ValidateOperationResources(const OperationRequest& request, const ResourceClaimBook& claims,
        const std::vector<NativeResourceBalance>& balances, std::string& blocker) {
        auto reject=[&](const char* code) { blocker=code; return false; };
        if (request.consumption.empty()) { blocker.clear(); return true; }
        if (!claims.Protection().ready) return reject("resource_protection_unavailable");
        try { NativeBefore(request); }
        catch (const std::exception&) { return reject("invalid_claimed_consumption"); }
        for (const auto& use : request.consumption) {
            const auto& before=use.before;
            const auto* saved=claims.Inspect(before.id);
            if (!saved || !SameResourceClaim(*saved,before)) return reject("acknowledged_claim_changed");
            const NativeResourceBalance* native=nullptr;
            for (const auto& row : balances) if (row.actor == before.actor && row.itemGuid == before.itemGuid &&
                row.itemEntry == before.itemEntry && row.location == before.location) {
                if (native) return reject("ambiguous_native_resource_balance");
                native=&row;
            }
            if (!native) return reject("claimed_native_resource_unavailable");
            // All other obligations remain protected, including pending holds.
            // The declared consumption is already contained in this exact held
            // claim; checking total backing prevents borrowing another job's stock.
            if (before.copper ? claims.Protection().ProtectedMoney(before.actor) > native->copper :
                claims.Protection().ProtectedItem(before.actor,before.itemGuid,before.itemEntry) > native->quantity)
                return reject("claimed_native_resource_shortfall");
        }
        blocker.clear(); return true;
    }
    bool VerifyConsumedNativeResources(const OperationRequest& request,
        const std::vector<NativeResourceBalance>& before, const std::vector<NativeResourceBalance>& after,
        std::string& blocker) {
        auto reject=[&](const char* code){blocker=code;return false;};
        if (request.consumption.empty()) {blocker.clear();return true;}
        try {NativeBefore(request);}
        catch (const std::exception&) {return reject("invalid_claimed_consumption");}
        using Key=std::tuple<uint32_t,uint32_t,uint32_t,std::string>;
        std::map<Key,uint64_t> used,initial,final;
        for (const auto& use : request.consumption) {
            const auto& c=use.before;
            used[{c.actor,c.itemGuid,c.itemEntry,c.location}]+=use.used;
        }
        for (const auto& row : before)
            if (!initial.emplace(Key{row.actor,row.itemGuid,row.itemEntry,row.location},uint64_t(row.quantity)+row.copper).second)
                return reject("ambiguous_native_consumption_proof");
        for (const auto& row : after)
            if (!final.emplace(Key{row.actor,row.itemGuid,row.itemEntry,row.location},uint64_t(row.quantity)+row.copper).second)
                return reject("ambiguous_native_consumption_proof");
        for (const auto& resource : used) {
            const auto start=initial.find(resource.first),end=final.find(resource.first);
            // A consumed stack can disappear. A missing wallet observation is
            // unknown, never evidence that all its money was spent.
            if (start == initial.end() || (end == final.end() && std::get<3>(resource.first) == "money"))
                return reject("native_consumption_proof_missing");
            const auto remaining=end == final.end() ? 0 : end->second;
            if (start->second < resource.second || remaining != start->second-resource.second)
                return reject("native_consumption_delta_mismatch");
        }
        blocker.clear(); return true;
    }
}
