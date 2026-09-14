#include "LivingActivityOperations.h"
#include "LivingActivityTransfer.h"
#include "LivingGuildMailHandoff.h"
#include "LivingCommissionMail.h"
#include "LivingLootQuote.h"
#include "LivingGatherQuote.h"
#include "LivingRepairQuote.h"
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
            if(request.kind=="commission_mail_send") {
                CommissionMailQuote quote;
                if(!DecodeCommissionMailQuote(request.beforeState,quote) ||
                    !ExactCommissionMailConsumption(request.transition.task,quote,request.consumption) ||
                    request.effects!=(Mask(Effect::Inventory)|Mask(Effect::Money)) ||
                    request.persistence!=NativePersistence::Inventory || !request.mailGain.Empty() ||
                    !request.itemGain.Empty() || !request.itemTransfer.id.empty())
                    throw std::invalid_argument("Exact commission parcel and postage contract required");
            }
            if(request.kind=="critical_equipment_repair") {
                NativeRepairQuote quote;
                if(!DecodeNativeRepairQuote(request.beforeState,quote) || quote.actor!=request.transition.task.actor ||
                    request.transition.task.checkpoint.step!="maintenance_repair" ||
                    request.effects!=(Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Equipment)) ||
                    request.persistence!=NativePersistence::Inventory || !request.itemGain.Empty() ||
                    !request.mailGain.Empty() || !request.itemTransfer.id.empty() || request.consumption.size()!=1 ||
                    request.consumption.front().before.location!="money" ||
                    request.consumption.front().before.copper!=quote.copper || request.consumption.front().used!=quote.copper)
                    throw std::invalid_argument("Exact paid critical repair contract required");
            }
            if(request.kind=="gather_open") {
                NativeGatherQuote quote;
                if(!DecodeNativeGatherQuote(request.beforeState,quote) || quote.actor!=request.transition.task.actor ||
                    request.effects!=(Mask(Effect::Inventory)|Mask(Effect::Spell)|Mask(Effect::Movement)) ||
                    request.persistence!=NativePersistence::Profession || !request.itemGain.Empty() ||
                    !request.mailGain.Empty() || !request.itemTransfer.id.empty() || !request.consumption.empty())
                    throw std::invalid_argument("Exact native gathering cast contract required");
            }
            if(request.kind=="loot_collect") {
                NativeLootQuote quote;
                if(!DecodeNativeLootQuote(request.beforeState,quote) || quote.actor!=request.transition.task.actor ||
                    request.itemGain.entry!=quote.entry || request.itemGain.quantity!=quote.quantity ||
                    request.effects!=Mask(Effect::Inventory) || request.persistence!=NativePersistence::Inventory ||
                    !request.consumption.empty() || !request.mailGain.Empty() || !request.itemTransfer.id.empty())
                    throw std::invalid_argument("Exact native loot acquisition contract required");
            }
            if(request.kind=="guild_mail_send") {
                GuildMailQuote quote;
                if(!DecodeGuildMailQuote(request.beforeState,quote) ||
                    !ExactGuildMailConsumption(request.transition.task,quote,request.consumption) ||
                    request.effects!=(Mask(Effect::Inventory)|Mask(Effect::Money)|Mask(Effect::Guild)) ||
                    request.persistence!=NativePersistence::Inventory || !request.mailGain.Empty() ||
                    !request.itemGain.Empty() || !request.itemTransfer.id.empty())
                    throw std::invalid_argument("Exact guild parcel and postage contract required");
            }
            if(!request.mailGain.Empty() && (!ValidMailGainSpec(request.mailGain) ||
                request.kind!="auction_purchase" || !request.itemGain.Empty() || !request.itemTransfer.id.empty() ||
                request.effects!=(Mask(Effect::Inventory)|Mask(Effect::Money)) ||
                request.persistence!=NativePersistence::Inventory || request.consumption.size()!=1 ||
                request.consumption.front().before.location!="money"))
                throw std::invalid_argument("Exact auction mail purchase contract required");
            if (!request.itemTransfer.id.empty()) {
                const auto& c=request.itemTransfer;
                if (!request.consumption.empty() || !request.itemGain.Empty() || !ValidItemTransfer(c) || request.kind!=ItemTransferKind(c) ||
                    request.effects!=Mask(Effect::Inventory) || request.persistence!=NativePersistence::Inventory ||
                    c.actor!=request.transition.task.actor || c.task!=request.transition.task.root)
                    throw std::invalid_argument("Bank transfer contract mismatch");
                return "{\"native\":"+request.beforeState+",\"transfer\":"+ItemTransferIdentity(c)+'}';
            }
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
    bool ValidateOperationAdapter(const OperationRequest& request, const NativeOperationAdapter& adapter,
        std::string& blocker) {
        auto reject=[&](const char* code){blocker=code;return false;};
        if(request.kind!=adapter.OperationKind() || request.effects!=adapter.OperationEffects() ||
            request.persistence!=adapter.PersistencePolicy())return reject("native_adapter_mismatch");
        const bool transfer=adapter.SupportsItemTransfer() && ValidItemTransfer(request.itemTransfer) &&
            request.kind==ItemTransferKind(request.itemTransfer) && request.effects==Mask(Effect::Inventory) &&
            request.persistence==NativePersistence::Inventory && request.consumption.empty() && request.itemGain.Empty();
        // These finite native contracts already validate exact effects, no
        // consumption/transfer, spell or loot identity, and the declared gain.
        // A generic item-gain capability is not permission to spend unclaimed
        // money or run an arbitrary resource-mutating spell.
        const bool loot=request.kind=="loot_collect" && adapter.SupportsItemGain() && !adapter.DeferredNativeCast();
        const bool gather=request.kind=="gather_open" && adapter.DeferredNativeCast();
        if(loot || gather) {
            try {NativeBefore(request);}
            catch(const std::exception&) {return reject("invalid_native_acquisition_contract");}
        }
        if(!transfer && !loot && !gather && (request.effects&(Mask(Effect::Money)|Mask(Effect::Inventory))) &&
            (!adapter.SupportsClaimedConsumption() || request.consumption.empty() || request.persistence==NativePersistence::JournalOnly))
            return reject("resource_effect_adapter_not_supported");
        if(!request.consumption.empty() && !adapter.SupportsClaimedConsumption())return reject("native_adapter_mismatch");
        if(!request.itemTransfer.id.empty() && !transfer)return reject("native_adapter_mismatch");
        if(!request.itemGain.Empty() && (!adapter.SupportsItemGain() || !ValidItemGainSpec(request.itemGain)))
            return reject("native_item_gain_adapter_not_supported");
        if(!request.mailGain.Empty() && (!adapter.SupportsMailGain() || !ValidMailGainSpec(request.mailGain)))
            return reject("native_mail_gain_adapter_not_supported");
        if(request.kind=="guild_mail_send" && !adapter.SupportsGuildMailHandoff())return reject("native_guild_mail_adapter_required");
        if(request.kind=="commission_mail_send" && !adapter.SupportsCommissionMailSend())return reject("native_commission_mail_adapter_required");
        blocker.clear();return true;
    }
    bool ValidateOperationRequest(const OperationRequest& request, const Task& saved,
        const WorldContext& current, const Task* root, uint64_t wallNow, std::string& blocker) {
        const auto& next = request.transition.task;
        if (!request.effects || (request.effects & ~AllEffects) ||
            unsigned(request.persistence) > unsigned(NativePersistence::Profession) || !IsToken(request.kind, 48) ||
            !JsonObject(request.beforeState, 4096) ||
            (!request.itemGain.Empty() && (!ValidItemGainSpec(request.itemGain) ||
                !(request.effects & Mask(Effect::Inventory)) || request.persistence == NativePersistence::JournalOnly)) ||
            next.phase != Phase::Executing ||
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
        const std::string gain = !request.mailGain.Empty() ? ",\"mail_gain\":"+MailGainSpecJson(request.mailGain) :
            request.itemGain.Empty() ? "" : ",\"item_gain\":"+ItemGainSpecJson(request.itemGain);
        if (!gain.empty() && (!(request.effects & Mask(Effect::Inventory)) || request.persistence == NativePersistence::JournalOnly))
            throw std::invalid_argument("Item gains require native inventory persistence");
        const std::string state = "{\"effects\":" + std::to_string(request.effects) +
            ",\"persistence\":" + std::to_string(unsigned(request.persistence)) + ",\"native\":" + NativeBefore(request) + '}';
        // Preserve the exact historical intent encoding for operations without
        // output claims. An old receipt does not silently change fingerprint.
        const auto intent=gain.empty() ? state : state.substr(0,state.size()-1)+gain+'}';
        return OperationIntentWrite(request.transition.task, request.transition.expectedRevision,
            request.transition.receipt, request.kind, intent);
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
        if (request.consumption.empty() && request.itemTransfer.id.empty()) { blocker.clear(); return true; }
        if (!claims.Protection().ready) return reject("resource_protection_unavailable");
        try { NativeBefore(request); }
        catch (const std::exception&) { return reject("invalid_claimed_consumption"); }
        if (!request.itemTransfer.id.empty()) {
            const auto& before=request.itemTransfer;const auto* saved=claims.Inspect(before.id);
            if (!saved || !SameResourceClaim(*saved,before)) return reject("acknowledged_transfer_claim_changed");
            if (balances.size()!=1 || balances[0].actor!=before.actor || balances[0].itemGuid!=before.itemGuid ||
                balances[0].itemEntry!=before.itemEntry || balances[0].quantity!=before.quantity ||
                balances[0].location!=before.location || balances[0].nativeReference!=before.nativeReference ||
                balances[0].copper) return reject("whole_native_transfer_stack_required");
            uint32_t available=0;
            if (!claims.AvailableToTask(before.task,balances[0],available) || available!=before.quantity)
                return reject("bank_stack_has_other_commitments");
            blocker.clear();return true;
        }
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
