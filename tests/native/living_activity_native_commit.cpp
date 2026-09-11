#include "LivingActivityNativeCommit.h"
#include <cassert>
#include <string>
#include <vector>
using namespace LivingActivity;
struct NativeField {
    std::string value;
    const char* GetString() const { return value.c_str(); } // Actual native API, not std::string.
    std::string GetCppString() const { return value; }
    uint64_t GetUInt64() const { return std::stoull(value); }
};
struct NativeRows {
    std::vector<std::vector<NativeField>> values;
    size_t row = 0;
    unsigned fields = 2;
    unsigned GetFieldCount() const { return fields; }
    NativeField* Fetch() { return values.empty() ? nullptr : values[row].data(); }
    bool NextRow() { return ++row < values.size(); }
};
int main() {
    for (unsigned value=0;value<=static_cast<unsigned>(NativeSaveStatus::CommitUncertain);++value)
        assert(std::string(Name(static_cast<NativeSaveStatus>(value))) != "native_save_invalid_status");
    assert(std::string(Name(NativeSaveStatus::NativeProofRejected)) != Name(NativeSaveStatus::JournalProofRejected));
    const std::string task="5da54d36-1e92-5829-ac33-b37f802dcfc3";
    auto probe=[&](std::vector<std::vector<NativeField>> values) {
        NativeRows rows{std::move(values)}; return ProbeNativeReceipt(&rows,task,7);
    };
    assert(ProbeNativeReceipt<NativeRows>(nullptr,task,7)==NativeReceipt::Unavailable);
    assert(probe({{{""},{"0"}}})==NativeReceipt::Absent);
    assert(probe({{{task},{"7"}},{{""},{"0"}}})==NativeReceipt::Present);
    assert(probe({{{""},{"0"}},{{task},{"7"}}})==NativeReceipt::Present);
    assert(probe({{{task},{"7"}}})==NativeReceipt::Unavailable); // No healthy sentinel.
    assert(probe({{{task},{"6"}},{{""},{"0"}}})==NativeReceipt::Conflict);
    assert(probe({{{"other-task"},{"7"}},{{""},{"0"}}})==NativeReceipt::Conflict);
    assert(probe({{{task},{"7"}},{{task},{"7"}}})==NativeReceipt::Conflict);
    assert(probe({{{""},{"0"}},{{""},{"0"}}})==NativeReceipt::Conflict);
    assert(probe({{{task},{"7"}},{{""},{"0"}},{{""},{"0"}}})==NativeReceipt::Conflict);
    assert(probe({})==NativeReceipt::Conflict);
    NativeRows wrongShape{{{{task},{"7"}}}}; wrongShape.fields=1;
    assert(ProbeNativeReceipt(&wrongShape,task,7)==NativeReceipt::Conflict);

    unsigned applies=0;
    auto apply=[&] { ++applies; return true; };
    for (auto status : {NativeReceipt::Unavailable,NativeReceipt::Conflict})
        assert(!ExecuteRetainedNativeSave([&] { return status; },apply));
    assert(!applies);
    assert(ExecuteRetainedNativeSave([] { return NativeReceipt::Present; },apply));
    assert(!applies); // Already committed but caller lost its acknowledgement.
    assert(ExecuteRetainedNativeSave([] { return NativeReceipt::Absent; },apply));
    assert(applies==1);
    assert(!ExecuteRetainedNativeSave([] { return NativeReceipt::Absent; },[] { return false; }));

    // The real native transaction's final assertion rolls the whole body back
    // on missing journal proof. This decision test does not manufacture native
    // inventory proof or exercise SqlTransaction itself; the realm fixture must.
    bool receipt=false, forceRollback=true;
    unsigned nativeGameplayCalls=1, savedBatches=0;
    auto read=[&] { return receipt ? NativeReceipt::Present : NativeReceipt::Absent; };
    auto save=[&] { ++savedBatches; if (forceRollback) return false; receipt=true; return true; };
    assert(!ExecuteRetainedNativeSave(read,save));
    forceRollback=false;
    assert(ExecuteRetainedNativeSave(read,save));
    for (unsigned duplicate=0;duplicate<100;++duplicate) assert(ExecuteRetainedNativeSave(read,save));
    assert(savedBatches==2 && nativeGameplayCalls==1);
}
