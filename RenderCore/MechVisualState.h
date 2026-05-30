// RenderCore/MechVisualState.h
//
// GAMEADAPTERS-VISUAL-STATE-BRIDGE-1 (Slice 1).
//
// Renderer-facing, sanitized snapshot of per-mech gameplay VISUAL facts.
// This is a pure-POD type in RenderCore: NO GL, NO game headers. It exists
// so game code (code/, mclib/) can hand the renderer a small by-value copy
// of visual state WITHOUT the renderer ever chasing a game pointer.
//
// Producer:  BattleMech::render()  (code/mech.cpp) builds one of these from
//            already-available gameplay state and pushes it onto the actor's
//            Mech3DAppearance via setVisualState(). No GL, no raw renderer
//            access -- mirrors the existing setObjStatus()/setObjectParameters()
//            feed pattern.
// Carrier:   Mech3DAppearance::m_visualState mirrors the last pushed value.
// Consumer:  mclib/mech3d.cpp submit site copies damage01 + flags into the
//            GpuMechSubmitDesc -> GpuMechInstance SSBO record (engine side).
//
// IMPORTANT -- heat is COMPILED OUT in this engine:
//   The MechWarrior heat simulation lives behind #ifdef USEHEAT, which is
//   never defined. There is no live runtime heat to read. heat01 is therefore
//   ALWAYS 0.0 today; the field is kept for forward-compatibility (and so a
//   future USEHEAT-revival arc has a landing slot) but no GPU instance slot is
//   spent on it. Do NOT relabel a luminance/damage proxy as "heat".
//
// All fields default to SAFE NEUTRAL values so a mech that never gets a push
// (spawn frame, missing data) reads as undamaged / unflagged.
#pragma once

#include <cstdint>

namespace RenderCore {

// Bit positions for MechVisualState::flags. Only player-visible, non-secret
// facts may be packed here (see docs/sensor-contact-presentation-recon.md for
// the hidden-information boundary). Team RELATION is already shown to the
// player via the existing health-bar / highlight color, so it is non-secret.
enum MechVisualFlagBits : uint32_t {
    kMechVisualFlag_Selected   = 1u << 0,  // unit is currently player-selected
    kMechVisualFlag_Shutdown   = 1u << 1,  // OBJECT_STATUS_SHUTDOWN
    kMechVisualFlag_Disabled   = 1u << 2,  // OBJECT_STATUS_DISABLED
    kMechVisualFlag_Destroyed  = 1u << 3,  // OBJECT_STATUS_DESTROYED
    // Relation to the local player, packed in bits [4:5]:
    //   0 = own (same team & commander), 1 = ally (same team), 2 = enemy.
    // Matches the BattleMech::render() homeRelations value.
    kMechVisualFlag_RelationShift = 4u,
    kMechVisualFlag_RelationMask  = 0x3u << 4,
};

// 12-byte POD. Trivially copyable; passed by value across the game/renderer
// boundary. No constructors beyond default member initializers so it stays an
// aggregate.
struct MechVisualState {
    float    heat01   = 0.0f;  // [0,1] mech heat. Always 0 today (USEHEAT off).
    float    damage01 = 0.0f;  // [0,1] composite damage; 0 = pristine, 1 = wrecked.
    uint32_t flags    = 0u;    // MechVisualFlagBits.
};

// Clamp a raw float into the safe [0,1] visual range, mapping any non-finite
// input (NaN/Inf -- e.g. getStatusRating() divides by maxArmor with no zero
// guard) to 0.0. Header-only so both the game-side producer and any test can
// use the identical sanitizer.
inline float sanitizeMechVisual01(float v) {
    // NaN fails every comparison, so the (v == v) self-check rejects NaN;
    // the explicit bounds reject +/-Inf and out-of-range values.
    if (!(v == v)) return 0.0f;          // NaN
    if (v <= 0.0f) return 0.0f;
    if (v >= 1.0f) return 1.0f;
    return v;
}

inline uint32_t packMechRelation(uint32_t flags, uint32_t relation /*0..2*/) {
    flags &= ~kMechVisualFlag_RelationMask;
    flags |= (relation << kMechVisualFlag_RelationShift) & kMechVisualFlag_RelationMask;
    return flags;
}

}  // namespace RenderCore
