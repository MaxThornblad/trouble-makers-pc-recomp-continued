// The state registry (netcode-v2 phase 1, docs/NETCODE-V2.md).
//
// Host-side state that the game's behaviour depends on must travel with the
// game: into a save state, to the other machine when an online session
// (re)starts, back in time when a rollback restores a frame, and into the
// fingerprint that tells two machines apart. Anything kept in a plain C++
// variable has none of that, and the symptoms are subtle: a cheat that
// forgets how hurt an NPC was after a load, a joiner whose widescreen mode
// lags the host's by a few frames, a rollback that replays a frame
// differently. So a feature registers the state it keeps, once, and the
// four things above come for free.
//
// Prefer game memory for new state (player 2 keeps hers at 0x807F0000): it
// needs no registration at all. Register what has to stay host-side.
//
//   static mm::state::Pod<MyState> g_state{"my-feature", 1};
//   ... g_state->field ...
//
// A Pod is saved and restored as raw bytes (so: plain data only, no pointers,
// fixed size), reset to default-constructed when a state lacks it (an older
// file, a module that did not exist), and hashed as part of the fingerprint.
// Bump the version when the layout changes; an old version is reset, not
// read. Modules register during static initialisation; the order does not
// matter (they are matched by name).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mm::state {

struct Module {
    const char* name;
    uint32_t version;
    size_t size;                        // bytes, fixed
    void* data;                         // the bytes
    void (*reset)(void* data);          // back to the initial state
};

void register_module(const Module& m);

template <class T>
class Pod {
public:
    Pod(const char* name, uint32_t version) {
        static_assert(std::is_trivially_copyable_v<T>, "a registered state is plain data");
        // Hashed and compared as raw bytes: padding would make two builds (or
        // two compilers) hash the same state differently.
        static_assert(std::has_unique_object_representations_v<T>, "a registered state must have no padding");
        register_module(Module{name, version, sizeof(T), &value_, [](void* p) { *static_cast<T*>(p) = T{}; }});
    }
    T* operator->() { return &value_; }
    T& operator*() { return value_; }
    const T* operator->() const { return &value_; }
    const T& operator*() const { return value_; }

private:
    T value_{};
};

// Every module's bytes, for a state: (name, version, bytes).
struct Blob {
    std::string name;
    uint32_t version = 0;
    std::vector<uint8_t> bytes;
};
std::vector<Blob> save_all();
// Restores what the blobs hold; modules the blobs lack (or hold at another
// version or size) are reset. Returns the names of those, for a log line.
std::vector<std::string> load_all(const std::vector<Blob>& blobs);
// Folds every module's bytes into a running FNV-1a hash (replay fingerprints).
uint64_t hash_all(uint64_t h);
// The same for saved blobs (a snapshot's modules, in registration order).
uint64_t hash_blobs(uint64_t h, const std::vector<Blob>& blobs);
// The registered modules, in registration order (tests, logs).
const std::vector<Module>& modules();

} // namespace mm::state
