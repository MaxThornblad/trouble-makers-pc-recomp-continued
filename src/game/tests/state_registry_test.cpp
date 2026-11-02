// The state registry's fingerprint must not depend on the order modules
// registered in: that order is the linker's (static initialisation across
// translation units), and the Linux and the MinGW build had it reversed, so
// a Linux host and a Windows guest with identical state resynced every
// second. Registering the same modules in two orders must hash the same,
// save the same blobs in the same order, and load each other's blobs.
#include "state_registry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "state_registry: %s\n", message);
        ++failures;
    }
}

struct Bytes {
    uint8_t data[8];
};

void reset(void* p) { *static_cast<Bytes*>(p) = Bytes{}; }

constexpr uint64_t kSeed = 0xCBF29CE484222325ull;
}

int main() {
    // One process has one registry; the test runs it twice from a fresh
    // process with the order flipped, and compares what each prints.
    const bool reversed = std::getenv("STATE_TEST_REVERSED") != nullptr;
    static Bytes a{{1, 2, 3, 4, 5, 6, 7, 8}}, b{{9, 10, 11, 12, 13, 14, 15, 16}}, c{{17, 18, 19, 20, 21, 22, 23, 24}};
    const mm::state::Module ma{"beta", 1, sizeof a, &a, reset};
    const mm::state::Module mb{"alpha", 1, sizeof b, &b, reset};
    const mm::state::Module mc{"gamma", 1, sizeof c, &c, reset};
    if (reversed) {
        mm::state::register_module(mc);
        mm::state::register_module(mb);
        mm::state::register_module(ma);
    } else {
        mm::state::register_module(ma);
        mm::state::register_module(mb);
        mm::state::register_module(mc);
    }
    const auto& mods = mm::state::modules();
    check(mods.size() == 3, "three modules");
    check(std::string(mods[0].name) == "alpha" && std::string(mods[1].name) == "beta" && std::string(mods[2].name) == "gamma",
          "modules kept in name order whatever the registration order");
    const std::vector<mm::state::Blob> blobs = mm::state::save_all();
    check(blobs.size() == 3 && blobs[0].name == "alpha" && blobs[2].name == "gamma", "blobs saved in name order");
    check(mm::state::hash_blobs(kSeed, blobs) == mm::state::hash_all(kSeed), "blobs hash as the live modules do");
    // Blobs in any order (a snapshot from the other build) hash the same.
    std::vector<mm::state::Blob> shuffled{blobs[2], blobs[0], blobs[1]};
    check(mm::state::hash_blobs(kSeed, shuffled) == mm::state::hash_all(kSeed), "blob order does not matter");
    check(mm::state::load_all(shuffled).empty(), "every module found by name");

    // Printed for the runner (cmake/SameOutput.cmake), which runs this once
    // plainly and once with STATE_TEST_REVERSED set: the two must agree.
    std::printf("%016llx\n", static_cast<unsigned long long>(mm::state::hash_all(kSeed)));
    return failures ? 1 : 0;
}
