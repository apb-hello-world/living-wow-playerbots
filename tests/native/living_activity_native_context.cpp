#include "LivingActivityNativeContext.h"
#include "LivingActivityEpoch.h"
#include <cassert>
using namespace LivingActivity;
enum MovementFlags { Falling = 0x2000, FallingFar = 0x4000 };
struct MovementInfo {
    uint32_t flags = 0;
    bool HasMovementFlag(MovementFlags value) const { return (flags & value) != 0; }
};
struct NativeGroup {
    NativeEpoch epoch;
    uint32_t GetId() const { return 7; }
    uint64_t GetLivingActivityIdentity() const { return epoch.Actor(); }
    uint64_t GetLivingActivityRevision() const { return epoch.Map(); }
};
struct NativeAI {
    NativeEpoch epoch;
    uint64_t GetActivityActorEpoch() const { return epoch.Actor(); }
    uint64_t GetActivityMapEpoch() const { return epoch.Map(); }
};
struct NativePlayer {
    NativeAI ai;
    NativeGroup* group = nullptr;
    bool inWorld = true, teleporting = false;
    bool alive = true, combat = false, taxi = false;
    MovementInfo m_movementInfo;
    uint32_t GetGUIDLow() const { return 497; }
    uint32_t GetMapId() const { return 1; }
    uint32_t GetInstanceId() const { return 0; }
    NativeAI* GetPlayerbotAI() { return &ai; }
    NativeGroup* GetGroup() { return group; }
    bool IsInWorld() const { return inWorld; }
    bool IsBeingTeleported() const { return teleporting; }
    bool IsAlive() const { return alive; }
    bool IsInCombat() const { return combat; }
    bool IsTaxiFlying() const { return taxi; }
    void* GetTransport() { return nullptr; }
};
int main() {
    const std::string boot = "637bd562-36d2-5b01-bc01-e2d831c49f38";
    NativePlayer player; NativeGroup group;
    auto safety = [&] { return ReadNativeSafety(player, MovementFlags(Falling | FallingFar)); };
    assert(safety() == 0);
    assert(std::string(NativeSafetyReason(0)).empty());
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Death)|uint32_t(Safety::Combat)))=="native_death_recovery");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Combat)))=="native_combat");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Transfer)))=="native_map_transfer");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Taxi)))=="native_taxi_travel");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Transport)))=="native_transport_travel");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::Falling)))=="native_falling");
    assert(std::string(NativeSafetyReason(uint32_t(Safety::UnsafeOperation)))=="native_operation_safety");
    player.m_movementInfo.flags = FallingFar;
    assert(safety() == uint32_t(Safety::Falling));
    player.combat = player.taxi = true;
    assert(safety() == (uint32_t(Safety::Falling) | uint32_t(Safety::Combat) | uint32_t(Safety::Taxi)));
    player.combat = player.taxi = false; player.m_movementInfo.flags = 0;
    auto solo = ReadNativeContext(player, 1, boot);
    assert(solo.actor == 497 && solo.session.empty() && !solo.sessionRevision && solo.mapGeneration);
    player.group = &group; player.ai.epoch.Invalidate();
    const auto joined = ReadNativeContext(player, 1, boot);
    assert(!(joined == solo) && joined.sessionRevision && !joined.session.empty());
    group.epoch.Invalidate(); // Leader/member/permission-sensitive native roster update.
    assert(!(ReadNativeContext(player, 1, boot) == joined));
    const auto prior = ReadNativeContext(player, 1, boot);
    player.group = nullptr; player.ai.epoch.Invalidate();
    player.group = &group; player.ai.epoch.Invalidate();
    assert(!(ReadNativeContext(player, 1, boot) == prior)); // Unobserved leave/rejoin ABA.
    NativeGroup replacement; player.group = &replacement;
    assert(ReadNativeContext(player, 1, boot).session != prior.session); // Reused native numeric group ID.
    player.teleporting = true; assert(!ReadNativeContext(player, 1, boot).mapGeneration);
    player.teleporting = false; player.inWorld = false; assert(!ReadNativeContext(player, 1, boot).mapGeneration);
    player.inWorld = true;
    assert(!(ReadNativeContext(player, 2, boot) == ReadNativeContext(player, 1, boot)));
}
