#include "LivingCraftCapture.h"
#include "LivingActivityAuthority.h"
#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace LivingActivity {
    bool ValidCraftIdentity(const CraftIdentity& identity) {
        return IsUuid(identity.task) && IsUuid(identity.operation) && identity.revision &&
            identity.ownerGeneration && ExecutionAuthority::ContextValid(identity.world) && identity.recipe && identity.skill;
    }
    namespace {
        using StackValue=std::tuple<uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint8_t>;
        std::vector<StackValue> Values(const CraftFrame& frame) {
            std::vector<StackValue> values;
            for (const auto& stack : frame.stacks)
                values.emplace_back(stack.actor,stack.guid,stack.entry,stack.count,stack.bagGuid,stack.slot);
            std::sort(values.begin(),values.end());return values;
        }
    }
    bool SameCraftFrame(const CraftFrame& a,const CraftFrame& b) {
        return a.actor==b.actor && a.skill==b.skill && a.money==b.money && Values(a)==Values(b);
    }
    bool SameCraftIdentity(const CraftIdentity& a,const CraftIdentity& b) {
        return a.task==b.task && a.operation==b.operation && a.revision==b.revision &&
            a.ownerGeneration==b.ownerGeneration && a.world==b.world && a.recipe==b.recipe && a.skill==b.skill;
    }
    bool ValidCraftFrame(const CraftFrame& frame) {
        if (!frame.actor || !frame.skill || frame.stacks.size()>256) return false;
        std::set<uint32_t> guids;
        std::set<std::pair<uint32_t,uint8_t>> locations;
        for (const auto& stack : frame.stacks)
            if (stack.actor!=frame.actor || !stack.guid || !stack.entry || !stack.count ||
                !guids.insert(stack.guid).second || !locations.emplace(stack.bagGuid,stack.slot).second) return false;
        return true;
    }
    CraftCapture::CraftCapture(CraftIdentity identity) {
        if (!ValidCraftIdentity(identity)) throw std::invalid_argument("invalid_native_craft_identity");
        result.identity=std::move(identity);
        result.blocker.reserve(128); // All callback reason strings fit before any native effect.
    }
    bool CraftCapture::Start(const CraftIdentity& identity,CraftFrame before) {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.phase!=CraftCapturePhase::Reserved || !SameCraftIdentity(identity,result.identity) ||
            !ValidCraftFrame(before) || before.actor!=identity.world.actor) return false;
        result.before=std::move(before);result.phase=CraftCapturePhase::Casting;return true;
    }
    bool CraftCapture::EnterEffect(const CraftIdentity& identity,const CraftFrame& current) {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.phase!=CraftCapturePhase::Casting || !SameCraftIdentity(identity,result.identity)) return false;
        if (!ValidCraftFrame(current) || !SameCraftFrame(result.before,current)) {
            result.blocker="native_craft_inputs_changed_before_effect";return false;
        }
        result.effectEntered=true;result.phase=CraftCapturePhase::Applying;return true;
    }
    bool CraftCapture::Created(const CraftIdentity& identity,uint32_t entry,uint32_t quantity) {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.phase!=CraftCapturePhase::Applying || !SameCraftIdentity(identity,result.identity)) return false;
        // Multiple native creation calls are retained as an explicit unsupported
        // outcome; a second callback cannot silently overwrite the first result.
        if (result.createdCalls==std::numeric_limits<uint32_t>::max()) {
            result.blocker="native_craft_creation_callback_overflow";return false;
        }
        ++result.createdCalls;
        if (!entry || !quantity || quantity>10000 || (result.createdEntry && result.createdEntry!=entry) ||
            uint64_t(result.createdQuantity)+quantity>10000) {
            result.blocker="native_craft_creation_outside_contract";return false;
        }
        result.createdEntry=entry;result.createdQuantity+=quantity;return true;
    }
    bool CraftCapture::Finish(const CraftIdentity& identity,bool succeeded,CraftFrame after) {
        std::lock_guard<std::mutex> lock(mutex);
        if ((result.phase!=CraftCapturePhase::Casting && result.phase!=CraftCapturePhase::Applying) ||
            !SameCraftIdentity(identity,result.identity)) return false;
        if (!ValidCraftFrame(after) || after.actor!=identity.world.actor) {
            result.blocker="native_craft_after_state_unavailable";
            result.phase=CraftCapturePhase::Abandoned;return false;
        }
        result.after=std::move(after);result.nativeFinished=true;
        result.nativeSucceeded=succeeded;result.phase=CraftCapturePhase::Finished;return true;
    }
    void CraftCapture::Abandon() {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.phase==CraftCapturePhase::Finished || result.phase==CraftCapturePhase::Abandoned) return;
        result.phase=CraftCapturePhase::Abandoned;
        if (result.blocker.empty()) result.blocker="native_craft_completion_callback_missing";
    }
    std::optional<CraftCaptureResult> CraftCapture::ReadFinished() const {
        std::lock_guard<std::mutex> lock(mutex);
        if (result.phase!=CraftCapturePhase::Finished && result.phase!=CraftCapturePhase::Abandoned) return std::nullopt;
        return result; // Not consumed by inspection. Retain until native save acknowledgement.
    }
    CraftCapturePhase CraftCapture::Phase() const {
        std::lock_guard<std::mutex> lock(mutex);return result.phase;
    }
    CraftVerification VerifyCraftCapture(const CraftIdentity& expected,const ProfessionJob& job,
        const CraftCaptureResult& observed,const ItemGainSpec& output) {
        CraftVerification result;
        auto reject=[&](const std::string& blocker) {result.blocker=blocker;return result;};
        std::string blocker;
        if (!ValidCraftIdentity(expected) || !SameCraftIdentity(expected,observed.identity) ||
            !ValidateProfessionJob(job,blocker) || job.recipe!=expected.recipe || job.skill!=expected.skill)
            return reject("native_craft_identity_mismatch");
        if ((job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial) ||
            !ValidItemGainSpec(output) || output.entry!=job.outputEntry || job.subjectItem)
            return reject("native_craft_output_contract_unsupported");
        if (!observed.blocker.empty()) return reject(observed.blocker);
        if (observed.phase!=CraftCapturePhase::Finished || !observed.nativeFinished ||
            !ValidCraftFrame(observed.before) || !ValidCraftFrame(observed.after) ||
            observed.before.actor!=expected.world.actor || observed.after.actor!=expected.world.actor)
            return reject("native_craft_completion_proof_missing");
        if (!observed.nativeSucceeded) {
            if (!observed.effectEntered && !observed.createdCalls && SameCraftFrame(observed.before,observed.after)) {
                result.result=CraftEvidence::RejectedWithoutEffect;result.blocker="native_cast_cancelled_without_effect";return result;
            }
            return reject("native_craft_cancelled_after_possible_effect");
        }
        if (!observed.effectEntered || observed.createdCalls!=1 || observed.createdEntry!=output.entry ||
            observed.createdQuantity!=output.quantity) return reject("native_craft_creation_receipt_mismatch");
        result=VerifyCraftResources(expected.world.actor,job,observed.before,observed.after,output);
        if (result.result==CraftEvidence::Verified) result.attempt.nativeEffectVerified=true;
        return result;
    }
    CraftVerification VerifyCraftResources(uint32_t actor,const ProfessionJob& job,
        const CraftFrame& beforeFrame,const CraftFrame& afterFrame,const ItemGainSpec& output) {
        CraftVerification result;std::string blocker;
        auto reject=[&](const std::string& code) {result.blocker=code;return result;};
        if (!ValidateProfessionJob(job,blocker) || !ValidItemGainSpec(output) ||
            output.entry!=job.outputEntry || job.subjectItem ||
            (job.operation!=ProfessionOperation::CreateItem && job.operation!=ProfessionOperation::TransformMaterial) ||
            !ValidCraftFrame(beforeFrame) || !ValidCraftFrame(afterFrame) ||
            beforeFrame.actor!=actor || afterFrame.actor!=actor)
            return reject("native_craft_resource_frame_invalid");
        if (beforeFrame.money!=afterFrame.money || afterFrame.skill<beforeFrame.skill)
            return reject("native_craft_wallet_or_skill_changed_unexpectedly");
        std::set<uint32_t> inputGuids;
        CraftFrame inputsBefore=beforeFrame,inputsAfter=afterFrame;
        inputsBefore.stacks.clear();inputsAfter.stacks.clear();
        std::vector<NativeItemStack> outputsBefore,outputsAfter;
        if (job.reagents.empty() || std::any_of(job.reagents.begin(),job.reagents.end(),
            [&](const auto& need){return need.entry==output.entry;}))
            return reject("native_craft_input_output_overlap_unsupported");
        for (const auto& stack : beforeFrame.stacks) {
            if (stack.entry==output.entry) {outputsBefore.push_back(stack);continue;}
            inputGuids.insert(stack.guid);inputsBefore.stacks.push_back(stack);
        }
        for (const auto& stack : afterFrame.stacks) {
            if (stack.entry==output.entry) {
                if (inputGuids.count(stack.guid)) return reject("native_craft_input_identity_reused_as_output");
                outputsAfter.push_back(stack);continue;
            }
            inputsAfter.stacks.push_back(stack);
        }
        if (!VerifyCraftReagents(actor,job.reagents,inputsBefore,inputsAfter,blocker)) return reject(blocker);
        if (!VerifyNativeItemGain(actor,output,outputsBefore,outputsAfter,result.gains,blocker))
            return reject(blocker);
        result.result=CraftEvidence::Verified;result.blocker.clear();
        result.attempt.recipe=job.recipe;result.attempt.consumed=job.reagents;
        result.attempt.produced={{output.entry,output.quantity}};
        result.attempt.skillBefore=beforeFrame.skill;result.attempt.skillAfter=afterFrame.skill;
        return result;
    }
    bool VerifyCraftReagents(uint32_t actor,const std::vector<ProfessionReagent>& required,
        const CraftFrame& beforeFrame,const CraftFrame& afterFrame,std::string& blocker) {
        auto reject=[&](const char* code){blocker=code;return false;};
        if (!ValidCraftFrame(beforeFrame) || !ValidCraftFrame(afterFrame) ||
            beforeFrame.actor!=actor || afterFrame.actor!=actor || required.empty() || required.size()>8)
            return reject("native_craft_resource_frame_invalid");
        if (beforeFrame.money!=afterFrame.money || afterFrame.skill<beforeFrame.skill)
            return reject("native_craft_wallet_or_skill_changed_unexpectedly");
        std::map<uint32_t,uint64_t> before,after,needed;
        std::map<uint32_t,NativeItemStack> originals;
        for (const auto& need:required)
            if (!need.entry || !need.perAttempt || need.perAttempt>10000 || !needed.emplace(need.entry,need.perAttempt).second)
                return reject("native_craft_reagent_contract_invalid");
        for (const auto& stack:beforeFrame.stacks) {
            if (!needed.count(stack.entry)) return reject("native_craft_unexpected_input_entry");
            before[stack.entry]+=stack.count;originals.emplace(stack.guid,stack);
        }
        for (const auto& stack:afterFrame.stacks) {
            const auto original=originals.find(stack.guid);
            if (!needed.count(stack.entry) || original==originals.end() || original->second.entry!=stack.entry ||
                original->second.count<stack.count || original->second.bagGuid!=stack.bagGuid || original->second.slot!=stack.slot)
                return reject("native_craft_input_identity_changed");
            after[stack.entry]+=stack.count;
        }
        for (const auto& need:needed)
            if (before[need.first]<need.second || before[need.first]-need.second!=after[need.first])
                return reject("native_craft_consumption_mismatch");
        blocker.clear();return true;
    }
}
