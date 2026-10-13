// "loaded resources.log" (next to the exe, started clean each run): every
// model (.scn), texture, effect (.seq) and sound the game loads or lets go,
// with its size, the live total of its kind and the process' memory - to see
// what is using memory.
#pragma once
#include <cstddef>
#include <string>

namespace reslog {
enum Kind { SCN = 0, TEX, SEQ, SOUND, KIND_COUNT };
// bytes = what it takes in memory (decoded), not the file size.
void Loaded(Kind k, const std::string& path, size_t bytes, const char* note = nullptr);
void Released(Kind k, const std::string& path, size_t bytes, const char* note = nullptr);
// A free-form line (with the totals and process memory).
void Note(const char* fmt, ...);

// Carried by a loaded object: counts its bytes while it lives (a copy counts
// again - it is a second copy in memory; a move hands the count over).
struct Tag {
    Kind kind = SCN;
    std::string path;
    size_t bytes = 0;
    Tag() = default;
    Tag(const Tag& o);
    Tag(Tag&& o) noexcept;
    Tag& operator=(const Tag& o);
    Tag& operator=(Tag&& o) noexcept;
    ~Tag();
    void Set(Kind k, const std::string& p, size_t b, const char* note = nullptr);
};
}
