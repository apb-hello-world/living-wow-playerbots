#ifndef LIVING_ACTIVITY_NATIVE_CONTEXT_H
#define LIVING_ACTIVITY_NATIVE_CONTEXT_H
#include "LivingActivityEffects.h"

namespace LivingActivity {
    inline const char* NativeSafetyReason(uint32_t safety) {
        // Diagnostics only: never clear or reinterpret the native safety mask.
        if (safety & uint32_t(Safety::Death)) return "native_death_recovery";
        if (safety & uint32_t(Safety::Combat)) return "native_combat";
        if (safety & uint32_t(Safety::Transfer)) return "native_map_transfer";
        if (safety & uint32_t(Safety::Taxi)) return "native_taxi_travel";
        if (safety & uint32_t(Safety::Transport)) return "native_transport_travel";
        if (safety & uint32_t(Safety::Falling)) return "native_falling";
        return safety ? "native_operation_safety" : "";
    }
    template<class NativePlayer, class MovementFlag>
    uint32_t ReadNativeSafety(NativePlayer& player, MovementFlag falling) {
        uint32_t safety = 0;
        if (!player.IsAlive()) safety |= uint32_t(Safety::Death);
        if (player.IsInCombat()) safety |= uint32_t(Safety::Combat);
        if (!player.IsInWorld() || player.IsBeingTeleported()) safety |= uint32_t(Safety::Transfer);
        if (player.IsTaxiFlying()) safety |= uint32_t(Safety::Taxi);
        if (player.GetTransport()) safety |= uint32_t(Safety::Transport);
        if (player.m_movementInfo.HasMovementFlag(falling)) safety |= uint32_t(Safety::Falling);
        return safety;
    }
    // Reads the actor's native lifecycle and the group's atomic stamp only.
    // Never enumerates a roster or calls a manager's mutating 'const' session
    // getter on a map worker. Core patch 016 supplies the native group stamp.
    template<class NativePlayer>
    WorldContext ReadNativeContext(NativePlayer& player, uint64_t policy, const std::string& boot) {
        WorldContext value; value.actor = player.GetGUIDLow(); value.policyRevision = policy; value.boot = boot;
        const auto* ai = player.GetPlayerbotAI();
        if (!ai || !player.IsInWorld() || player.IsBeingTeleported()) return value; // Invalid epoch, fail closed.
        value.map = player.GetMapId(); value.instance = player.GetInstanceId();
        value.actorGeneration = ai->GetActivityActorEpoch(); value.mapGeneration = ai->GetActivityMapEpoch();
        if (const auto* group = player.GetGroup()) {
            if (!group->GetLivingActivityIdentity() || !group->GetLivingActivityRevision()) {
                value.mapGeneration = 0; return value; // Native counter exhaustion never revives authority.
            }
            value.session = "group:" + std::to_string(group->GetId()) + ':' +
                std::to_string(group->GetLivingActivityIdentity());
            value.sessionRevision = group->GetLivingActivityRevision();
        }
        return value;
    }
}
#endif
