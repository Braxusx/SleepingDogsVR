#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstddef>
#include <cstdint>

namespace spatch::vehicle_beam_light {

// Round78: the headlight light is a REAL engine light component, not a record
// injected into the deferred light tables.
//
// Why the table injection was abandoned (Documentation/re-scan-results/
// round78/notes.md): the pass that renders the per-light shadow maps
// (FUN_140035370, cbShadowTransform / DropShadows) never reads the light
// tables or the group counters. Its input is the DynamicLightComponent list.
// FUN_14004e230 builds the deferred record table by walking that list, so a
// component is the only thing that can own a shadow map. A packed entry
// injected past the component layer can be lit and can never be shadowed.
//
// The component is 0xF0 bytes (Render::DynamicLightComponent) and its light
// record is `component + 0x50`, which is why the record field offsets below
// are unchanged from round76/77 - they are simply applied to a component's
// own memory instead of a static clone.

// Per-frame record table builder. Called synchronously by the orchestrator
// (FUN_140059750) BEFORE the collection job is queued, so a component updated
// at its entry is picked up by the walk in the same frame.
inline constexpr std::uintptr_t kRecordTableBuilderRva = 0x0004E230;

// First 12 bytes of FUN_14004e230: mov rax,rsp / push rbp,rsi,rdi,r12,r13,r14.
inline constexpr std::array<std::uint8_t, 12> kRecordTableBuilderSignature = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};

// Component geometry (FUN_140073080 creator + FUN_140072ec0 constructor).
inline constexpr std::size_t kComponentSize = 0xF0;

// We allocate MORE than the component needs and zero it. The stock object is
// 0xF0 bytes, but the renderer-side light path reads fields past that on
// objects it finds in the same list (+0x1BA class lane, +0x268 linked source).
// Reading those off the end of a 0xF0 block is an unmapped-page risk; reading
// zeros is a guarded "skip". The surplus is never written and never freed.
inline constexpr std::size_t kComponentAllocSize = 0x400;
inline constexpr std::size_t kComponentVftableOffset = 0x00;
inline constexpr std::size_t kComponentSelfListOffset = 0x30;  // SimComponent qNode
inline constexpr std::size_t kComponentNodeOffset = 0x40;      // light-list qNode
inline constexpr std::size_t kComponentPositionOffset = 0x80;
inline constexpr std::size_t kComponentColorOffset = 0x90;
inline constexpr std::size_t kComponentRadiusOffset = 0x9C;
inline constexpr std::size_t kComponentFovInnerOffset = 0xA0;
inline constexpr std::size_t kComponentFovOuterOffset = 0xA4;
inline constexpr std::size_t kComponentShadowNearOffset = 0xA8;
inline constexpr std::size_t kComponentTextureUidOffset = 0xAC;
inline constexpr std::size_t kComponentTypeOffset = 0xB8;
inline constexpr std::size_t kComponentShadowFlagsOffset = 0xB9;
inline constexpr std::size_t kComponentSpecularOffset = 0xBA;

// The light record the renderer reads is the component's field block.
inline constexpr std::size_t kRecordOffsetInComponent = 0x50;

// Render::DynamicLightComponent::vftable and the circular list sentinel the
// constructor splices into ({next at +0, prev at +8}).
//
// RVA math, done explicitly because this class of error has bitten this
// project twice: the values below are image-base relative, i.e. VA minus
// 0x140000000. VA 0x14167AEE8 -> 0x167AEE8; VA 0x142024678 -> 0x2024678.
// A wrong RVA does not fault loudly - the SEH guards turn it into "no
// template" and the feature silently does nothing.
inline constexpr std::uintptr_t kImageBase = 0x140000000;
inline constexpr std::uintptr_t kComponentVftableRva = 0x0167AEE8;
inline constexpr std::uintptr_t kLightListSentinelRva = 0x002024678;
static_assert(kComponentVftableRva == 0x14167AEE8ull - kImageBase,
              "vftable RVA must equal VA - image base");
static_assert(kLightListSentinelRva == 0x142024678ull - kImageBase,
              "sentinel RVA must equal VA - image base");

// Component allocation goes through the engine pool, exactly as the stock
// creator does: pool = FUN_140190c90(), block = FUN_140166b60(pool, 0xF0,
// "DynamicLightComponent", 0, 1). Using the engine allocator keeps the block
// valid for any later pool bookkeeping; we never free it ourselves.
inline constexpr std::uintptr_t kComponentPoolGetterRva = 0x00190C90;
inline constexpr std::uintptr_t kComponentAllocRva = 0x00166B60;

// Light record field offsets (round75/76-verified, re-confirmed against the
// component map in round78; record == component + 0x50).
inline constexpr std::size_t kRecordBasisOffset = 0x00;        // 3x4 basis, 12 floats
inline constexpr std::size_t kRecordPositionOffset = 0x30;     // xyz + pad
inline constexpr std::size_t kRecordColorOffset = 0x40;        // rgb floats
inline constexpr std::size_t kRecordRadiusOffset = 0x4C;
inline constexpr std::size_t kRecordFovInnerOffset = 0x50;
inline constexpr std::size_t kRecordFovOuterOffset = 0x54;
inline constexpr std::size_t kRecordShadowNearOffset = 0x58;
inline constexpr std::size_t kRecordTextureUidOffset = 0x5C;
inline constexpr std::size_t kRecordOnTimeOffset = 0x60;
inline constexpr std::size_t kRecordOffTimeOffset = 0x64;
inline constexpr std::size_t kRecordTypeOffset = 0x68;
inline constexpr std::size_t kRecordShadowFlagsOffset = 0x69;
inline constexpr std::size_t kRecordSpecularOffset = 0x6A;

// Byte count of this light's own shadow-clip planes. The collector's type-1
// path (FUN_14007dd40) runs, for every such light:
//
//     if (record[0x6B] != 0)
//         for (i = 0; i < record[0x6B]; ++i)
//             if (dot(cam_pos, plane[record[0x74 + i]]) < 0) -> REJECT the light
//
// The indices at 0x74 select planes out of a per-frame table, so the test is
// "is the camera inside this light's shadow frustum". A clone inherits the
// TEMPLATE lamp's indices, which describe a frustum around the lamp - so our
// light is admitted only while the camera happens to sit inside that
// stranger's shadow volume, and silently disappears as soon as the player
// drives away from it. That is a per-light rejection that has nothing to do
// with where the light itself is placed, which is why it looked like a
// placement bug: the session that shipped the 14 m offset submitted 118
// frames and then nothing at all, while the type-1 lane emptied at the same
// moment.
//
// Our light owns no clip planes, so the count must be zero: the test is a
// pure rejection, and skipping it can only make the light more likely to be
// admitted, never less.
inline constexpr std::size_t kRecordClipPlaneCountOffset = 0x6B;
inline constexpr std::uint8_t kLightTypeSpot = 1;
inline constexpr std::uint8_t kLightTypeHero = 3;  // the player-attached per-frame light
inline constexpr std::uint32_t kNoTextureUid = 0xFFFFFFFFu;

// The selector computes SIL = (record[0x6A] != 0) && (DAT_142136135 == 0) and
// then does `test sil,sil / cmovnz r8d,eax`: with the shadow-eligible bit set
// and the global shadow query answering non-zero, EAX holds 0xB3DD445F (the
// specular spot variant) and R8D holds 0x70960186 (DRSPOTLIGHTSHADOWS). A
// non-zero specular byte therefore OVERRIDES the shadow technique.
inline constexpr std::uint8_t kBeamSpecular = 0;
inline constexpr std::uint8_t kBeamShadowFlags = 1;

// Tuning surface (config v71). Radius in metres; color as linear 0..1.
inline constexpr float kBeamRadiusDefault = 14.0f;
inline constexpr float kBeamRadiusMin = 4.0f;
inline constexpr float kBeamRadiusMax = 40.0f;
// Headlights are white, not warm. The previous 1.00/0.93/0.78 was a warm
// interior-lamp tint and read as yellow on the road.
inline constexpr float kBeamColorDefaultR = 0.95f;
inline constexpr float kBeamColorDefaultG = 0.97f;
inline constexpr float kBeamColorDefaultB = 1.00f;
inline constexpr float kBeamForwardOffsetMeters = 2.5f;
inline constexpr float kBeamDropMeters = 0.8f;

// A car has two headlights, so the feature drives TWO light components placed
// symmetrically about the vehicle's centre line. One central light reads as a
// spotlight bolted to the bonnet, which is not what a headlight looks like.
inline constexpr int kBeamLightCount = 2;
// Half the track width: the lamps sit either side of the centre line.
inline constexpr float kBeamHeadlightLateralMeters = 0.72f;

// Bound on the light-list walks: the list is engine-owned and mutated on
// render threads, so every hop is SEH-guarded and counted.
inline constexpr std::uint32_t kMaxListWalk = 4096;

// Deferred light collection job FUN_14007dd40(a, b, ctx). Runs after the record
// table is built and walks every record, gating it against the camera carried
// in `ctx`. Hooked because `ctx` is the ONLY place the exact camera the gates
// use is available - the view object probed in session 4 (+0x30 = [0.38 0 0])
// is not a world position and must not be used as an anchor.
inline constexpr std::uintptr_t kCollectorRva = 0x0007DD40;

// First 13 bytes: mov [rsp+8],rbx / mov [rsp+10],rdx / push rbp,rsi,rdi.
inline constexpr std::array<std::uint8_t, 13> kCollectorSignature = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x54,
    0x24, 0x10, 0x55, 0x56, 0x57};

// Context layout, derived from the collector decompile (param_3 is a uint*):
//   0x00  record count           0x30  record table pointer
//   0x10  distance params        0x40  6 frustum planes, 4 floats each
//   0xA0  camera position xyz    0xB0  camera forward xyz
inline constexpr std::size_t kCtxCountOffset = 0x00;
inline constexpr std::size_t kCtxRecordTableOffset = 0x30;
inline constexpr std::size_t kCtxPlanesOffset = 0x40;
inline constexpr std::size_t kCtxCameraPosOffset = 0xA0;
inline constexpr std::size_t kCtxCameraForwardOffset = 0xB0;

// Where the beam sits relative to the camera. The record's position is the
// cone's BASE CENTRE, not its emitter: both gates offset it backwards along
// the record normal to find the apex, so the visible cone runs from
// `pos - radius*cos(fov)*normal` to `pos`.
inline constexpr float kBeamAheadMeters = 7.0f;
inline constexpr float kBeamAimDropMeters = 0.6f;

// Vehicle frame (round80). The anchor is the chase camera's desired look
// target - the car while driving - and the heading is `look - eye` flattened
// to the ground. Both are recomputed every frame by the game, so unlike a
// scanned matrix they cannot go stale.
inline constexpr float kAnchorMaxDistance = 20.0f;

// The anchor is the chase camera's look target, which is the car. But the
// game's own argument order for eye/look is not documented anywhere, and
// deriving the heading from `look - eye` produced a beam that only appeared
// when the player pressed the look-back camera - i.e. the direction was
// inverted. Rather than guess the argument order, the heading is now taken
// from CAMERA -> ANCHOR, which cannot be inverted: a chase camera always looks
// at the car it follows, so that vector points the way the car is facing. The
// anchor distance stops being a direction and becomes a scalar lead.
inline constexpr float kAnchorMinDirectionDistance = 1.0f;

// Floor on that lead. If the anchor turns out to be the camera itself (i.e.
// the game really did hand us eye/look the other way round), the distance is
// zero and there is no direction to take; fall back to the camera's own
// forward and this lead, which is a typical chase-camera standoff.
inline constexpr float kAnchorMinLeadMeters = 5.0f;

// Chooses the beam's heading and the lead out from the camera.
//
// The heading is ALWAYS the collector camera's forward, flattened to the ground
// plane. It is the only unambiguous "forward" in reach: a chase camera looks
// the way the car drives, and - unlike anything derived from the anchor - it
// cannot be inverted by the eye/look argument order.
//
// The anchor contributes a DISTANCE and nothing else. Three sessions were spent
// on the anchor's direction: first `look - eye` (sign depends on an undocumented
// argument order), then `anchor - camera` (inverts the moment the anchor is
// behind the camera). Both produced a beam that sat behind the car and, worse,
// one that moved FURTHER behind as the offset grew - which is the signature of
// an inverted direction. A scalar cannot be inverted.
struct BeamHeading {
    float dir_x;
    float dir_y;
    float lead;
    bool from_anchor;
};

[[nodiscard]] inline BeamHeading ResolveBeamHeading(
    float cam_fwd_x,
    float cam_fwd_y,
    float anchor_distance,
    bool anchored) noexcept {
    BeamHeading result{};
    const float length =
        std::sqrt(cam_fwd_x * cam_fwd_x + cam_fwd_y * cam_fwd_y);
    if (length > 0.1f) {
        result.dir_x = cam_fwd_x / length;
        result.dir_y = cam_fwd_y / length;
    } else {
        // Camera pointing straight down: no horizontal heading to take. Any
        // fixed axis is as good as another and the lead clamp keeps it sane.
        result.dir_x = 1.0f;
        result.dir_y = 0.0f;
    }
    result.lead = kAnchorMinLeadMeters;
    result.from_anchor = false;
    if (anchored && anchor_distance > kAnchorMinDirectionDistance &&
        anchor_distance < kAnchorMaxDistance) {
        result.lead = anchor_distance;
        result.from_anchor = true;
    }
    return result;
}

// A coordinate this large is not a world position; it is an uninitialised
// field or a unit vector. Rejecting it keeps a bad anchor from parking the
// light outside the level.
[[nodiscard]] inline bool IsPlausibleWorldPosition(float x,
                                                   float y,
                                                   float z) noexcept {
    const auto finite_in_range = [](float v) {
        return v == v && v > -100000.0f && v < 100000.0f;
    };
    return finite_in_range(x) && finite_in_range(y) && finite_in_range(z) &&
           !(x == 0.0f && y == 0.0f && z == 0.0f);
}

// A live type-1 light is a usable clone template.
[[nodiscard]] inline bool IsSpotTemplate(std::uint8_t type) noexcept {
    return type == kLightTypeSpot;
}

// True when the record pointer the selector was handed is OUR component's
// record block. Used to force the shadow technique past the global query
// without touching any other light.
[[nodiscard]] inline bool IsOwnRecord(const void* record,
                                      const std::uint8_t* component) noexcept {
    return component != nullptr && record != nullptr &&
           record == component + kRecordOffsetInComponent;
}

// Gate for the per-frame placement: a beam needs the feature on, a template to
// clone and a hero light to anchor against.
[[nodiscard]] inline bool ShouldSpawnBeam(bool config_enabled,
                                          bool in_frame_with_lamp,
                                          bool in_frame_with_hero) noexcept {
    return config_enabled && in_frame_with_lamp && in_frame_with_hero;
}

// Right vector on the ground plane for a horizontal heading: cross(heading,
// world +Z). Used to place the two headlights either side of the centre line.
// For a heading of +Y (north) this yields +X (east), so slot 0 lands on the
// left of the car and slot 1 on the right.
inline void BeamRightVector(float dir_x, float dir_y,
                            float out_right[2]) noexcept {
    out_right[0] = dir_y;
    out_right[1] = -dir_x;
}

// Tuning surface for one beam.
struct BeamTuning {
    float radius;
    float color_r;
    float color_g;
    float color_b;
};

// Tracks which way the hero light is travelling, so the beam can be pushed
// ahead of the player. Only a movement consistent with the stored heading
// updates it: reversing or standing still must not flip the beam round.
struct BeamTracker {
    bool have_last_position = false;
    float last_hero_x = 0.0f;
    float last_hero_y = 0.0f;
    float last_hero_z = 0.0f;
    bool have_forward = false;
    float forward_x = 0.0f;
    float forward_y = 0.0f;
    float forward_z = 1.0f;

    void ObserveHeroPosition(float x, float y, float z) noexcept {
        if (have_last_position) {
            const float dx = x - last_hero_x;
            const float dy = y - last_hero_y;
            const float dz = z - last_hero_z;
            const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (length > 0.25f) {
                const float nx = dx / length;
                const float ny = dy / length;
                const float nz = dz / length;
                // Only accept a movement that agrees with the heading we
                // already have, so walking backwards does not spin the beam.
                const bool consistent =
                    !have_forward ||
                    (nx * forward_x + ny * forward_y + nz * forward_z) > 0.5f;
                if (consistent) {
                    forward_x = nx;
                    forward_y = ny;
                    forward_z = nz;
                    have_forward = true;
                }
            }
        }
        last_hero_x = x;
        last_hero_y = y;
        last_hero_z = z;
        have_last_position = true;
    }
};

// How far the cone's APEX sits ahead of the car's centre, at the nose.
inline constexpr float kBeamApexForwardMeters = 1.5f;
inline constexpr float kBeamVehicleDropMeters = 0.7f;

// Parked lights are dropped far below the level so the collector's distance
// gate rejects them - a safe "off" that needs no special-case in the pipeline.
inline constexpr float kBeamParkDropMeters = 10000.0f;

// The record's position is the cone's BASE CENTRE, and the cone extends
// BACKWARDS from it to the apex. The record must therefore be placed a full
// cone height beyond where the beam should start.
//
// The height of a cone with base radius R and half-angle fov is R / tan(fov),
// NOT R * cos(fov). That distinction cost four sessions: at R = 14 and
// fov = 0.436 the two give 30.0 m and 12.7 m, and with the wrong (shorter)
// value the record was placed 14.2 m ahead of the car while the cone still
// reached 16 m BEHIND it - so the beam looked "behind the car" no matter how
// the offset was tuned, and every attempt to fix it by moving the record
// further forward was chasing the same 17 m error.
[[nodiscard]] inline float BeamConeHeight(float radius, float fov) noexcept {
    // Guard both ends: tan() blows up at 90 degrees and fov near zero would
    // give an absurd height.
    if (!(fov > 0.01f && fov < 1.45f) || !(radius > 0.0f)) {
        return radius > 0.0f ? radius : 0.0f;
    }
    return radius / std::tan(fov);
}

// Where to put the record so the cone STARTS at the car's nose: the height of
// the cone plus the apex offset. Falls back to the apex offset alone if the
// fov is nonsense.
[[nodiscard]] inline float BeamRecordAheadOffset(float radius,
                                                 float fov) noexcept {
    const float height = BeamConeHeight(radius, fov);
    if (!(height > 0.0f && height < 100.0f)) {
        return kBeamApexForwardMeters;
    }
    return height + kBeamApexForwardMeters;
}

// Writes a record that is guaranteed not to be submitted: the position is far
// outside every distance gate, so the collector skips it without touching any
// other field. Used while the player is on foot, where a headlight would
// otherwise light the character.
inline void ParkBeamRecord(std::uint8_t* record,
                           float x,
                           float y,
                           float z) noexcept {
    if (record == nullptr) {
        return;
    }
    std::memcpy(record + kRecordPositionOffset, &x, sizeof(x));
    std::memcpy(record + kRecordPositionOffset + 4, &y, sizeof(y));
    std::memcpy(record + kRecordPositionOffset + 8, &z, sizeof(z));
}

// Patches the component's record block in place. `record` is
// `component + kRecordOffsetInComponent`. `anchor_*` is the base position,
// `forward_*` a unit direction (may be zero) and `forward_offset` the distance
// to push along it. The basis is left as cloned, so the cone keeps the
// template lamp's orientation - the shape proven to reach the shadow path.
inline void PatchBeamRecordAt(std::uint8_t* record,
                              float anchor_x,
                              float anchor_y,
                              float anchor_z,
                              float forward_x,
                              float forward_y,
                              float forward_z,
                              float forward_offset,
                              float drop,
                              const BeamTuning& tuning) {
    auto write_float = [record](std::size_t offset, float value) {
        std::memcpy(record + offset, &value, sizeof(value));
    };
    write_float(kRecordPositionOffset,
                anchor_x + forward_x * forward_offset);
    write_float(kRecordPositionOffset + 4,
                anchor_y + forward_y * forward_offset);
    write_float(kRecordPositionOffset + 8,
                anchor_z + forward_z * forward_offset - drop);
    // Warm headlight color; the cloned lamp intensity stays untouched.
    write_float(kRecordColorOffset, tuning.color_r);
    write_float(kRecordColorOffset + 4, tuning.color_g);
    write_float(kRecordColorOffset + 8, tuning.color_b);
    write_float(kRecordRadiusOffset, tuning.radius);
    // Always-on schedule; no projector cookie so the cone reads clean.
    // The texture UID is an int field (the selector compares it against -1),
    // so the "none" value must be the raw bit pattern, not a float cast.
    write_float(kRecordOnTimeOffset, 0.0f);
    write_float(kRecordOffTimeOffset, 0.0f);
    {
        const std::uint32_t no_texture = kNoTextureUid;
        std::memcpy(record + kRecordTextureUidOffset, &no_texture,
                    sizeof(no_texture));
    }
    record[kRecordTypeOffset] = kLightTypeSpot;
    record[kRecordShadowFlagsOffset] = kBeamShadowFlags;
    record[kRecordSpecularOffset] = kBeamSpecular;
    // Never inherit the template's shadow-clip planes: those indices describe
    // a frustum around the lamp we were cloned from, and the collector rejects
    // any light whose clip planes exclude the camera. Written every frame
    // because a record created before this fix still carries the template's
    // count.
    record[kRecordClipPlaneCountOffset] = 0;
}

// Hero-anchored variant kept for the movement tracker path (the hero light is
// still what tells us which way the player is going).
inline void PatchBeamRecord(std::uint8_t* record,
                            float hero_x,
                            float hero_y,
                            float hero_z,
                            const BeamTuning& tuning,
                            BeamTracker& tracker) {
    float forward_x = tracker.forward_x;
    float forward_y = tracker.forward_y;
    float forward_z = tracker.forward_z;
    if (!tracker.have_forward) {
        forward_x = 0.0f;
        forward_y = 0.0f;
        forward_z = 1.0f;
    }
    PatchBeamRecordAt(record, hero_x, hero_y, hero_z, forward_x, forward_y,
                      forward_z, kBeamForwardOffsetMeters, kBeamDropMeters,
                      tuning);
}

// Writes an orthonormal basis whose THIRD row points along `forward`.
//
// The third row is the one both collector gates read as the cone axis
// (record + 0x20, +0x24, +0x28): they offset the record position backwards
// along it to locate the cone's apex and its bounding sphere. A clone of a
// street lamp inherits a DOWNWARD axis, which puts that sphere tens of metres
// in the air and gets the light frustum-culled - the exact reason session 3
// admitted the beam for 35 frames and then dropped it.
//
// World up is +Z in this title (positions carry the level height in Z while X
// and Y span thousands of metres).
inline void WriteAimBasis(std::uint8_t* record,
                          float forward_x,
                          float forward_y,
                          float forward_z) noexcept {
    float length = std::sqrt(forward_x * forward_x + forward_y * forward_y +
                             forward_z * forward_z);
    if (!(length > 1e-4f)) {
        return;  // degenerate direction; keep the cloned basis
    }
    const float fx = forward_x / length;
    const float fy = forward_y / length;
    const float fz = forward_z / length;
    // right = normalize(up x forward)
    float rx = -fy;
    float ry = fx;
    float rz = 0.0f;
    const float rlen = std::sqrt(rx * rx + ry * ry);
    if (rlen > 1e-4f) {
        rx /= rlen;
        ry /= rlen;
        rz = 0.0f;
    } else {
        // forward is vertical; pick any horizontal axis
        rx = 1.0f;
        ry = 0.0f;
        rz = 0.0f;
    }
    // up = forward x right
    const float ux = fy * rz - fz * ry;
    const float uy = fz * rx - fx * rz;
    const float uz = fx * ry - fy * rx;
    const float rows[3][3] = {{rx, ry, rz}, {ux, uy, uz}, {fx, fy, fz}};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            const float value = rows[row][column];
            std::memcpy(record + kRecordBasisOffset + row * 0x10 + column * 4,
                        &value, sizeof(value));
        }
        const float pad = 0.0f;
        std::memcpy(record + kRecordBasisOffset + row * 0x10 + 0xC, &pad,
                    sizeof(pad));
    }
}

}  // namespace spatch::vehicle_beam_light
