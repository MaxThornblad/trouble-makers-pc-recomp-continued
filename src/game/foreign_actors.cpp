#include "foreign_actors.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"

extern "C" void func_80016E70(uint8_t* rdram, recomp_context* ctx);   // banked actor update, $a0 = slot
extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);

namespace mm::foreign_actors {
namespace {

constexpr uint32_t kActors = 0x800EF510u;
constexpr uint32_t kStride = 0x198u;
constexpr uint32_t kStage = 0x800D28E4u;      // current stage id
constexpr uint32_t kScene = 0x800BE5D0u;

// A stage package: its dispatch table and its code+data as the game DMAs
// them (seen with the PI trace on warps to the stages), the end of the
// memory its code uses (its bss: the highest address it refers to), and
// where its live copy is kept. The ROM offsets are the game's own (US 1.1).
struct Package {
    const char* name;
    uint8_t bank;
    uint32_t table_ram, table_rom, table_size;
    uint32_t code_ram, code_rom, code_size;
    uint32_t end_ram;
    uint32_t store;
    int32_t scroll;   // its stage's scroll (16.16 px a frame) its objects' speeds include
};
constexpr Package kPackages[] = {
    // 3-12 CERBERUS: the cat Marina rides (0x0F01), Lunar's missiles (0x0F09), ...
    {"3-12", 0x0F, 0x80192000u, 0x0068A090u, 0x70u, 0x80192100u, 0x0068A100u, 0x15FC0u, 0x801B0200u, 0x807A0000u, 10 << 16},
    // 3-11 Cat-astrophe: Cat Clancer (0x2801) and its dodgeball helpers.
    {"3-11", 0x28, 0x801B0800u, 0x007BC840u, 0x10u, 0x801B0900u, 0x007BC850u, 0x4260u, 0x801B4C00u, 0x807C0000u, 0},
};
constexpr int kPackageCount = sizeof kPackages / sizeof kPackages[0];
// Per package: the stage and scene (+1) its copy was made for, 0 = none yet.
constexpr uint32_t kMadeFor = 0x807E0000u;

constexpr uint16_t kCat = 0x0F01;
constexpr uint16_t kLunarMissile = 0x0F09;
constexpr uint16_t kCatClancer = 0x2801;
constexpr uint16_t kMissileRidden = 4;
constexpr int kGoneX = 640;   // px from the screen's middle
constexpr uint16_t kMissileImage = 0x025C, kMissileDraw = 0x0101;

int g_inside = 0;   // nesting of swapped-in updates
const struct Package* g_swapped = nullptr;   // the package swapped in now

gpr addr(uint32_t vaddr) { return static_cast<gpr>(static_cast<int32_t>(vaddr)); }
uint8_t* host(uint8_t* rdram, uint32_t vaddr) { return rdram + (vaddr - 0x80000000u); }

const Package* package_of(uint16_t type) {
    const uint8_t bank = static_cast<uint8_t>(type >> 8);
    for (const Package& p : kPackages) {
        if (p.bank == bank) return &p;
    }
    return nullptr;
}

bool rom_ready(const Package& p) {
    const auto rom = recomp::get_rom();
    return rom.size() >= p.code_rom + p.code_size && rom.size() >= p.table_rom + p.table_size;
}

// The package's own table is in place: its stage loaded it.
bool is_loaded(uint8_t* rdram, const Package& p) {
    if (!rom_ready(p)) return false;
    const auto rom = recomp::get_rom();
    for (uint32_t i = 0; i < p.table_size; ++i) {
        if (MEM_BU(i, addr(p.table_ram)) != rom[p.table_rom + i]) return false;
    }
    for (uint32_t i = 0; i < 0x40; ++i) {
        if (MEM_BU(i, addr(p.code_ram)) != rom[p.code_rom + i]) return false;
    }
    return true;
}

// A fresh copy for this stage: as its stage's load leaves it (table and
// code+data from the ROM), its bss zeroed (its objects set up their own).
void make_copy(uint8_t* rdram, const Package& p, int index) {
    const auto rom = recomp::get_rom();
    const uint32_t size = p.end_ram - p.table_ram;
    for (uint32_t off = 0; off < size; off += 4) MEM_W(off, addr(p.store)) = 0;
    for (uint32_t i = 0; i < p.table_size; ++i) MEM_B(p.table_ram - p.table_ram + i, addr(p.store)) = static_cast<int8_t>(rom[p.table_rom + i]);
    for (uint32_t i = 0; i < p.code_size; ++i) MEM_B(p.code_ram - p.table_ram + i, addr(p.store)) = static_cast<int8_t>(rom[p.code_rom + i]);
    const uint32_t made_for = ((static_cast<uint32_t>(MEM_HU(0, addr(kStage))) << 16) | MEM_HU(0, addr(kScene))) + 1u;
    MEM_W(4 * index, addr(kMadeFor)) = static_cast<int32_t>(made_for);
    std::fprintf(stderr, "[foreign] %s's package made for this stage\n", p.name);
}

void ensure_copy(uint8_t* rdram, const Package& p) {
    const int index = static_cast<int>(&p - kPackages);
    const uint32_t made_for = ((static_cast<uint32_t>(MEM_HU(0, addr(kStage))) << 16) | MEM_HU(0, addr(kScene))) + 1u;
    if (static_cast<uint32_t>(MEM_W(4 * index, addr(kMadeFor))) != made_for) make_copy(rdram, p, index);
}

// The package in its place for a while (RAII).
class Swap {
public:
    Swap(uint8_t* rdram, const Package& p) : rdram_(rdram), p_(p), size_(p.end_ram - p.table_ram) {
        ++g_inside;
        prev_ = g_swapped;
        g_swapped = &p;
        stage_.assign(host(rdram, p.table_ram), host(rdram, p.table_ram) + size_);
        sections_ = recomp::overlays::get_loaded_sections();
        std::memcpy(host(rdram, p.table_ram), host(rdram, p.store), size_);
        load_overlays(p.code_rom, static_cast<int32_t>(p.code_ram), p.code_size);
    }
    ~Swap() {
        std::memcpy(host(rdram_, p_.store), host(rdram_, p_.table_ram), size_);
        std::memcpy(host(rdram_, p_.table_ram), stage_.data(), size_);
        recomp::overlays::restore_loaded_sections(sections_);
        g_swapped = prev_;
        --g_inside;
    }
    Swap(const Swap&) = delete;
    Swap& operator=(const Swap&) = delete;

private:
    uint8_t* rdram_;
    const Package& p_;
    uint32_t size_;
    const Package* prev_ = nullptr;
    std::vector<uint8_t> stage_;
    std::vector<recomp::overlays::LoadedSectionRecord> sections_;
};

} // namespace

// Cat Clancer (func_801B1BA4) away from 3-11 is a cat, not a boss. His
// states: 0 builds him, then 1..0x11 are his stage's introduction (crowd
// chatter 0x12D, a camera zoom, six talk messages, each waited on through
// the message queue), 0x1E the fight's start (crowd cheer 0x12E, a fanfare,
// the stage's music, and state 8 written into slot 0x50, the stage's
// crowd: anyone at all elsewhere), 0x1F waits for the stage to let him go,
// 0x20 makes the camera follow him and 0x21 on is play, where he can be
// grabbed and thrown. His own state 0 already has a way past the
// introduction (3-11 uses it after a continue: +0xD8 set takes it to 0x1E,
// placed at the screen's 48,-50). So away from 3-11 he is built that way,
// kept where he was spawned, and put straight into 0x20; one already in
// the introduction or the fight's start (a state from an older build) goes
// there too. Nothing of his introduction starts. (With the introduction
// running, the talk he queued was taken away after each update and he
// waited for it for ever, hovering, never grabbable.)
constexpr uint16_t kClancerPlay = 0x20;

// Before his update: true when this one builds him (state 0; +0xD8 set so
// he takes the continue path), and his position and state are put right
// after it (dispatch).
bool clancer_skip_intro(uint8_t* rdram, gpr a) {
    const uint16_t state = MEM_HU(0xD0, a);
    if (state == 0) {
        MEM_W(0xD8, a) = 1;
        return true;
    }
    if (state < kClancerPlay) {
        MEM_H(0xD0, a) = static_cast<int16_t>(kClancerPlay);
        MEM_W(0xD8, a) = 0;
    }
    return false;
}

// What is left for his update to do to the game around him: in play
// (state 0x20) he makes the camera follow him (0x800BE544, the first word
// of the camera block; earlier builds also saw him zoom it, 0x800BE5B0).
// The camera block is put back after each of his updates.
class Quiet {
public:
    Quiet(uint8_t* rdram, uint16_t type) : rdram_(rdram), on_(type == kCatClancer) {
        if (on_) std::memcpy(saved_, host(rdram, kCamera), sizeof saved_);
    }
    void end() {
        if (!on_) return;
        on_ = false;
        std::memcpy(host(rdram_, kCamera), saved_, sizeof saved_);
    }

private:
    static constexpr uint32_t kCamera = 0x800BE544u;
    uint8_t* rdram_;
    bool on_;
    uint8_t saved_[0x74];
};

bool known(uint16_t type) {
    const Package* p = package_of(type);
    return p != nullptr && rom_ready(*p);
}

bool native(uint8_t* rdram, uint16_t type) {
    const Package* p = package_of(type);
    return p == nullptr || is_loaded(rdram, *p);   // (a package this module does not keep: the stage's own)
}

bool swapped_in() {
    return g_inside > 0;
}

uint32_t live(uint8_t* rdram, uint16_t type, uint32_t vaddr) {
    const Package* p = package_of(type);
    if (p == nullptr || !rom_ready(*p) || vaddr < p->table_ram || vaddr >= p->end_ram) return vaddr;
    if (g_swapped == p || is_loaded(rdram, *p)) return vaddr;
    ensure_copy(rdram, *p);
    return p->store + (vaddr - p->table_ram);
}

bool with_package(uint8_t* rdram, uint16_t type, const std::function<void(uint8_t*)>& body) {
    const Package* p = package_of(type);
    if (p == nullptr) {   // not one this module keeps: the stage's own, in place
        body(rdram);
        return true;
    }
    if (!rom_ready(*p)) return false;
    if (is_loaded(rdram, *p)) {
        body(rdram);
        return true;
    }
    ensure_copy(rdram, *p);
    Swap swap(rdram, *p);
    body(rdram);
    return true;
}

int span(uint16_t type) {
    switch (type) {
        case kCat: return 14;           // 3-12's cat: parts in the 13 slots after it (func_801A23C4)
        case kCatClancer: return 14;    // 3-11's boss: 13 part sprites (type 0x21) after it
        default: return 1;
    }
}

void setup(uint8_t* rdram, int slot) {
    const gpr a = addr(kActors + static_cast<uint32_t>(slot) * kStride);
    if (MEM_HU(0xD2, a) == kLunarMissile) {
        // As Lunar's launcher leaves them in 3-12 (its own setup leaves no
        // picture): the missile image, drawn.
        MEM_H(0x84, a) = static_cast<int16_t>(kMissileImage);
        MEM_H(0x94, a) = static_cast<int16_t>(kMissileDraw);
    }
}

bool dispatch(uint8_t* rdram, recomp_context* ctx, int slot) {
    if (g_inside > 0 || slot <= 0 || slot >= 208) return false;   // (the update below comes through here again)
    const uint16_t type = MEM_HU(0xD2, addr(kActors + static_cast<uint32_t>(slot) * kStride));
    const Package* p = package_of(type);
    if (p == nullptr || !rom_ready(*p) || is_loaded(rdram, *p)) return false;
    ensure_copy(rdram, *p);
    Swap swap(rdram, *p);
    // 3-12 scrolls by 10 px a frame and its objects fly at that more than
    // they move on the screen (a missile overtakes the cat at 13 px and drops
    // back at 4). The camera elsewhere stands: their code sees their speed
    // with the scroll, what moves them is their speed less it.
    const gpr a = addr(kActors + static_cast<uint32_t>(slot) * kStride);
    MEM_W(0xEC, a) += p->scroll;
    // Lunar's missile waits on its launcher (slot 53 in 3-12, func_801A1178
    // state 0) until its timer runs out: elsewhere slot 53 is anything, and
    // the missile sat wherever that was. It waits where it was spawned.
    const bool waiting = type == kLunarMissile && MEM_HU(0xD0, a) == 0;
    const bool clancer_built = type == kCatClancer && clancer_skip_intro(rdram, a);
    const int32_t x = MEM_W(0x88, a), y = MEM_W(0x8C, a);
    const gpr a0 = ctx->r4;
    ctx->r4 = static_cast<gpr>(slot);
    Quiet quiet(rdram, type);
    func_80016E70(rdram, ctx);
    quiet.end();
    ctx->r4 = a0;
    if (MEM_HU(0xD2, a) == type) {
        MEM_W(0xEC, a) -= p->scroll;
        if (waiting || clancer_built) {
            MEM_W(0x88, a) = x;
            MEM_W(0x8C, a) = y;
        }
        if (clancer_built) {
            MEM_H(0xD0, a) = static_cast<int16_t>(kClancerPlay);
            MEM_W(0xD8, a) = 0;
        }
        // 3-12's loop takes what flies off the screen behind it away; here
        // it flew on forever. Gone well off the screen (not while ridden).
        const int sx = MEM_H(0x88, a);
        if (p->scroll != 0 && (sx > kGoneX || sx < -kGoneX) && !(type == kLunarMissile && MEM_HU(0xD0, a) == kMissileRidden)) {
            for (uint32_t off = 0x80; off < kStride; off += 4) MEM_W(off, a) = 0;
        }
    }
    return true;
}

} // namespace mm::foreign_actors
