#include "pet_clancer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"

#include "custom_actors.h"
#include "far_coop.h"
#include "player2.h"

namespace mm::pet_clancer {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;

gpr addr(uint32_t vaddr) { return static_cast<gpr>(static_cast<int32_t>(vaddr)); }
gpr slot_addr(int slot) { return addr(kActors + static_cast<uint32_t>(slot) * kStride); }

// His states (func_801B1BA4, the decompilation's names for the match):
constexpr uint16_t kIdleEnter = 0x20, kIdle = 0x21, kTurnEnter = 0x22, kTurn = 0x23;
constexpr uint16_t kWalkEnter = 0x24, kWalk = 0x25;
constexpr uint16_t kRunEnter = 0x26, kRun = 0x27;
constexpr uint16_t kFallEnter = 0x28, kFall = 0x29;
constexpr uint16_t kThrownEnter = 0x30, kThrown = 0x31;
constexpr uint16_t kLand = 0x38, kHurtLand = 0x40;
constexpr uint16_t kLostEnter = 0x48, kLostWait = 0x49, kLostEnd = 0x4A;
constexpr uint16_t kJumpEnter = 0x50, kJumpUp = 0x51, kJumpDown = 0x52;
constexpr uint16_t kBallThrowEnter = 0x58, kBallThrow = 0x59, kBallThrown = 0x5A;
constexpr uint16_t kLookEnter = 0x80, kLook = 0x81, kLook2 = 0x82, kLook3 = 0x83;
constexpr uint16_t kCatchEnter = 0x100, kCatch = 0x101, kGemEnter = 0x110, kGem = 0x111;
constexpr uint16_t kHeld = 0x1000, kHitEnter = 0x2000, kHit = 0x2001;

// +0x80 flag bits.
constexpr uint32_t kFacesLeft = 0x20;
constexpr uint32_t kGrabbable = 0x10000;   // Marina may pick him up
constexpr uint32_t kSolidBox = 0x40000;    // another actor's solid box

constexpr int32_t kRunSpeed = 0x25800;     // 2.34 px a frame (his run is 1.5625; half as fast again)
constexpr int32_t kJumpSpeed = 0x1E000;    // across, while he jumps (his own is 0.625)
constexpr int kStopNear = 40;              // px from the player he stops at
constexpr int kJumpUpNear = 56;            // px: a player this close and above him is worth a jump
constexpr int kJumpUpHeight = 20;          // px above his feet
constexpr int kWallAhead = 12;             // px ahead of his middle probed for a wall
constexpr int kBodyTop = 14;               // px above his feet the body reaches (box -4..8)
constexpr int kStepUp = 6;                 // px: this much he steps up without a jump
constexpr int kJumpMargin = 6;             // px over the obstacle's top the jump aims for
constexpr int kJumpMax = 56;               // px: the highest he jumps (his own 4.5 px/frame rises 46)
// His gravity (func_801B0D80: -0.21875 px a frame each frame, 16.16).
constexpr int32_t kGravity = 0x3800;
constexpr int kJumpWait = 45;              // frames between jumps
constexpr int16_t kHealth = 200;           // kept: below 5 his match ends (stage state!)

// Jump timer: his +0xD8 word, read by his code only before he is built.
constexpr uint32_t kTimer = 0xD8;

// What his match code wants and this stage has no use for.
bool match_only(uint16_t state) {
    switch (state) {
        case kWalkEnter: case kWalk:
        case kLostEnter: case kLostWait: case kLostEnd:
        case kBallThrowEnter: case kBallThrow: case kBallThrown:
        case kCatchEnter: case kCatch: case kGemEnter: case kGem:
            return true;
        default:
            return false;
    }
}

bool on_feet(uint16_t state) {
    switch (state) {
        case kIdleEnter: case kIdle: case kTurnEnter: case kTurn: case kRunEnter: case kRun:
        case kLookEnter: case kLook: case kLook2: case kLook3:
            return true;
        default:
            return false;
    }
}

// Solid at a camera-relative point: terrain, or another actor's solid box.
bool solid_at(uint8_t* rdram, int self, int x, int y) {
    if (mm::custom_actors::terrain_solid(rdram, x, y)) return true;
    for (int s = 1; s < 0x90; ++s) {
        if (s == self) continue;
        const gpr o = slot_addr(s);
        if ((static_cast<uint32_t>(MEM_W(0x80, o)) & kSolidBox) == 0) continue;
        const int ox = MEM_H(0x88, o), oy = MEM_H(0x8C, o);
        if (x >= ox + MEM_H(0xAA, o) && x <= ox + MEM_H(0xAC, o) && y >= oy + MEM_H(0xB0, o) && y <= oy + MEM_H(0xAE, o)) return true;
    }
    return false;
}

// Ground within 2 px under his feet (his box, three columns), terrain or
// a solid or stand-on actor.
bool ground_under(uint8_t* rdram, int self, int x, int feet) {
    for (int px : {x - 6, x, x + 6}) {
        for (int py = feet - 1; py >= feet - 3; --py) {
            if (mm::custom_actors::terrain_solid(rdram, px, py)) return true;
        }
    }
    return mm::custom_actors::floor_between(rdram, self, x, feet, feet - 3) != mm::custom_actors::kNoFloor;
}

bool wall_ahead(uint8_t* rdram, int self, int x, int feet, int dir) {
    const int px = x + dir * kWallAhead;
    for (int dy = 3; dy <= kBodyTop; dy += 4) {
        if (solid_at(rdram, self, px, feet + dy)) return true;
    }
    return false;
}

// How high the thing in his way stands over his feet: the column ahead is
// followed up through what is solid to the first gap his body fits in
// (kBodyTop + 2). Only what bars him counts, not anything further up.
// kTooHigh when there is no such gap within his reach.
constexpr int kTooHigh = 0x7FFF;
int obstacle_height(uint8_t* rdram, int self, int x, int feet, int dir) {
    const int px = x + dir * kWallAhead;
    int top = -1, clear = 0;
    for (int py = feet + 1; py <= feet + kJumpMax + kBodyTop + 2; py += 2) {
        if (solid_at(rdram, self, px, py)) {
            top = py + 1;
            clear = 0;
        } else {
            clear += 2;
            if (top >= 0 && clear > kBodyTop) return top - feet;
        }
    }
    return kTooHigh;
}

// The rise (16.16 px a frame) that peaks height px up under his gravity.
int32_t rise_for(int height) {
    const double h = static_cast<double>(height) * 65536.0, g = static_cast<double>(kGravity);
    return static_cast<int32_t>(std::sqrt(2.0 * g * h));
}

// The nearest player's record (slot 0, or player 2's).
gpr target(uint8_t* rdram, int x) {
    gpr best = slot_addr(0);
    const int p2 = mm::player2::slot(rdram);
    if (p2 > 0) {
        const gpr b = slot_addr(p2);
        if (MEM_W(0x80, b) != 0 && std::abs(MEM_H(0x88, b) - x) < std::abs(MEM_H(0x88, best) - x)) best = b;
    }
    return best;
}

enum Want { kHisOwn, kWantIdle, kWantRun, kWantJump, kWantFall };
struct Frame {
    int slot = -1;
    uint16_t before = 0;    // his state going into his update
    Want want = kHisOwn;    // what before() asked of him this frame
    int dir = 0;            // the way he should move (-1 / 0 / 1)
    bool near = false;      // within kStopNear of the player
    int32_t rise = 0;       // the jump asked for, 16.16 px a frame (0: his own)
};
Frame g_frame;

void face(uint8_t* rdram, gpr a, int dir) {
    if (dir == 0) return;
    const uint32_t flags = static_cast<uint32_t>(MEM_W(0x80, a));
    MEM_W(0x80, a) = static_cast<int32_t>(dir < 0 ? flags | kFacesLeft : flags & ~kFacesLeft);
}

void set_state(uint8_t* rdram, gpr a, uint16_t state) {
    MEM_H(0xD0, a) = static_cast<int16_t>(state);
}

void say(const char* what) {
    if (!mm::faraway::in_shadow()) std::fprintf(stderr, "[pet] Cat Clancer %s\n", what);
}

// Idle, as his match code may vary it: a turn to face Marina, a look around.
bool idling(uint16_t state) {
    switch (state) {
        case kIdleEnter: case kIdle: case kTurnEnter: case kTurn:
        case kLookEnter: case kLook: case kLook2: case kLook3:
            return true;
        default:
            return false;
    }
}

} // namespace

void before(uint8_t* rdram, int slot) {
    const gpr a = slot_addr(slot);
    g_frame = Frame{};
    g_frame.slot = slot;
    const uint16_t state = MEM_HU(0xD0, a);
    g_frame.before = state;
    if (state < kIdleEnter) return;   // not built yet
    MEM_H(0xE0, a) = kHealth;
    int timer = MEM_W(kTimer, a);
    if (timer > 0) MEM_W(kTimer, a) = --timer;

    const int x = MEM_H(0x88, a), y = MEM_H(0x8C, a);
    const int feet = y + MEM_H(0xB0, a);
    const gpr t = target(rdram, x);
    const int dx = MEM_H(0x88, t) - x, dy = (MEM_H(0x8C, t) + MEM_H(0xB0, t)) - feet;
    const int dir = dx < 0 ? -1 : 1;
    g_frame.dir = dir;
    g_frame.near = std::abs(dx) <= kStopNear;

    if (!on_feet(state)) {
        // In the air, held, thrown, landing: his own code, with his jump
        // carried toward the player.
        if (state == kJumpUp || state == kJumpDown) face(rdram, a, dir);
        return;
    }
    if (!ground_under(rdram, slot, x, feet)) {
        g_frame.want = kWantFall;
        set_state(rdram, a, kFallEnter);
        return;
    }
    if (std::abs(dx) > kStopNear) {
        face(rdram, a, dir);
        if (wall_ahead(rdram, slot, x, feet, dir)) {
            const int height = obstacle_height(rdram, slot, x, feet, dir);
            if (height <= kStepUp) {
                // A step: up it, running on.
                MEM_W(0x8C, a) += height << 16;
                g_frame.want = kWantRun;
                if (state != kRun) set_state(rdram, a, kRunEnter);
                return;
            }
            if (timer == 0 && height <= kJumpMax) {
                g_frame.want = kWantJump;
                g_frame.rise = rise_for(height + kJumpMargin);
                set_state(rdram, a, kJumpEnter);
                MEM_W(kTimer, a) = kJumpWait;
                say("jumps at a wall");
            } else {
                // Too high for him, or just jumped: he waits here.
                g_frame.want = kWantIdle;
                if (!idling(state)) set_state(rdram, a, kIdleEnter);
            }
            return;
        }
        g_frame.want = kWantRun;
        if (state != kRun) set_state(rdram, a, kRunEnter);
        return;
    }
    g_frame.dir = 0;
    if (dy > kJumpUpHeight && dy <= kJumpMax && std::abs(dx) < kJumpUpNear && timer == 0) {
        face(rdram, a, dir);
        g_frame.dir = dir;
        g_frame.want = kWantJump;
        g_frame.rise = rise_for(dy + kJumpMargin);
        set_state(rdram, a, kJumpEnter);
        MEM_W(kTimer, a) = kJumpWait;
        say("jumps up to the player");
        return;
    }
    g_frame.want = kWantIdle;
    if (state == kRun || state == kRunEnter) say("caught up");
    if (!idling(state)) set_state(rdram, a, kIdleEnter);
}

void after(uint8_t* rdram, int slot) {
    if (g_frame.slot != slot) return;
    const gpr a = slot_addr(slot);
    const uint16_t state = MEM_HU(0xD0, a);
    if (state < kIdleEnter) return;
    // His match decisions undone (they only set the state word; the state's
    // own code would run next frame): a turn at an arena mark while he
    // runs, a run, jump or catch for a "ball" while he stands.
    switch (g_frame.want) {
        case kWantRun:
            if (state != kRun) MEM_H(0xD0, a) = static_cast<int16_t>(kRun);
            break;
        case kWantIdle:
            if (!idling(state)) MEM_H(0xD0, a) = static_cast<int16_t>(kIdle);
            break;
        default:
            if (match_only(state)) MEM_H(0xD0, a) = static_cast<int16_t>(kIdleEnter);
            break;
    }
    const uint16_t now = MEM_HU(0xD0, a);
    if (now != g_frame.before) {
        if (now == kLand || (now == kIdleEnter && g_frame.before == kJumpDown)) say("lands");
        if (now == kIdleEnter && !on_feet(g_frame.before) && g_frame.near) say("caught up");
        else if (now == kHurtLand) say("lands after a throw");
        else if (now == kHeld) say("is picked up");
        else if (now == kThrownEnter) say("is thrown");
    }
    if (now == kRun) MEM_W(0xEC, a) = g_frame.dir * kRunSpeed;
    if (now == kJumpUp && g_frame.want == kWantJump && g_frame.rise != 0) MEM_W(0xF0, a) = g_frame.rise;   // (his own is 4.5 px a frame)
    if ((now == kJumpUp || now == kJumpDown) && g_frame.dir != 0) MEM_W(0xEC, a) = g_frame.dir * kJumpSpeed;
    // Standing or running he may be picked up (his match sets this only
    // where the match allows a catch).
    if (on_feet(now)) MEM_W(0x80, a) = static_cast<int32_t>(static_cast<uint32_t>(MEM_W(0x80, a)) | kGrabbable);
}

} // namespace mm::pet_clancer
