#include "state_registry.h"

#include <cstdio>

namespace mm::state {
namespace {

// Kept sorted by name. Registrations run at static initialisation, whose
// order across translation units is the linker's: the Linux and the MinGW
// build registered the same five modules in opposite orders, so the same
// bytes hashed differently and a Linux host and a Windows guest resynced
// every second (fingerprints "drifted" from frame 0). Everything here walks
// the modules in name order, the same on every build.
std::vector<Module>& registry() {
    static std::vector<Module> modules;   // built on first use
    return modules;
}

} // namespace

void register_module(const Module& m) {
    auto& all = registry();
    auto at = all.begin();
    while (at != all.end() && std::strcmp(at->name, m.name) < 0) ++at;
    if (at != all.end() && std::strcmp(at->name, m.name) == 0) {
        std::fprintf(stderr, "[state] module \"%s\" registered twice\n", m.name);
        std::abort();
    }
    all.insert(at, m);
}

const std::vector<Module>& modules() { return registry(); }

std::vector<Blob> save_all() {
    std::vector<Blob> out;
    out.reserve(registry().size());
    for (const Module& m : registry()) {
        Blob b;
        b.name = m.name;
        b.version = m.version;
        b.bytes.assign(static_cast<const uint8_t*>(m.data), static_cast<const uint8_t*>(m.data) + m.size);
        out.push_back(std::move(b));
    }
    return out;
}

std::vector<std::string> load_all(const std::vector<Blob>& blobs) {
    std::vector<std::string> reset;
    for (const Module& m : registry()) {
        const Blob* found = nullptr;
        for (const Blob& b : blobs) {
            if (b.name == m.name) {
                found = &b;
                break;
            }
        }
        if (found && found->version == m.version && found->bytes.size() == m.size) {
            std::memcpy(m.data, found->bytes.data(), m.size);
        } else {
            m.reset(m.data);
            reset.emplace_back(m.name);
        }
    }
    return reset;
}

uint64_t hash_blobs(uint64_t h, const std::vector<Blob>& blobs) {
    for (const Module& m : registry()) {
        for (const Blob& b : blobs) {
            if (b.name != m.name) continue;
            for (uint8_t byte : b.bytes) {
                h ^= byte;
                h *= 0x100000001B3ull;
            }
            break;
        }
    }
    return h;
}

uint64_t hash_all(uint64_t h) {
    for (const Module& m : registry()) {
        const auto* p = static_cast<const uint8_t*>(m.data);
        for (size_t i = 0; i < m.size; ++i) {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
    }
    return h;
}

} // namespace mm::state
