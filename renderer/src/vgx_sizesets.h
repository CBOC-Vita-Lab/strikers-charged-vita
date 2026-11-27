// held16 (wave 11) MSC_COPY_SIZESETS: one copy texture per (texture slot, picture size) kept dormant when the picture changes size, so going back to a size (play <-> replay / cut-scene under
// MSC_PICTURE_HOLD_SCENEPOL, a size step and back) takes the texture that was made for it instead of allocating a new one and deleting the old. Header-only and pure (no GL): the store keeps
// names and sizes and tells the caller which names to delete; tests/sizesets.cpp runs the shipped rules. Render only.
#pragma once
#include <cstdint>
#include <vector>

namespace vgxsizesets {

struct Entry { uint32_t slot; uint16_t w, h; uint32_t name; uint32_t lastUse; };

inline long KibOf(int w, int h) { return static_cast<long>(w) * h * 4 / 1024; }
inline long BudgetKib(long setting) { return setting <= 1 ? 6144 : (setting > 65536 ? 65536 : setting); }   // MSC_COPY_SIZESETS_KIB (default 6 MiB of dormant textures)

class Store {
public:
    // The dormant texture of this slot at this size, removed from the store; 0 when there is none.
    uint32_t Take(uint32_t slot, int w, int h) {
        for (size_t i = 0; i < v_.size(); ++i)
            if (v_[i].slot == slot && v_[i].w == w && v_[i].h == h) { const uint32_t n = v_[i].name; kib_ -= KibOf(w, h); v_.erase(v_.begin() + static_cast<long>(i)); ++taken; return n; }
        ++missed;
        return 0;
    }
    // Keep a texture dormant. False when it does not fit the budget, the free memory leaves less than `floorKib` after it, or the slot already holds one of that size (the caller deletes it as
    // before). freeKib < 0: unknown, no objection.
    bool Stash(uint32_t slot, int w, int h, uint32_t name, uint32_t now, long budgetKib, int freeKib, long floorKib) {
        const long k = KibOf(w, h);
        if (k + kib_ > budgetKib || (freeKib >= 0 && freeKib < floorKib + k)) { ++refused; return false; }
        for (const Entry& e : v_) if (e.slot == slot && e.w == w && e.h == h) { ++refused; return false; }
        v_.push_back(Entry{slot, static_cast<uint16_t>(w), static_cast<uint16_t>(h), name, now});
        kib_ += k;
        ++stashed;
        return true;
    }
    // The texture of a slot is gone (deleted, replaced): its dormant sizes go with it. Names are appended to `out`.
    void DropSlot(uint32_t slot, std::vector<uint32_t>& out) {
        for (size_t i = 0; i < v_.size();)
            if (v_[i].slot == slot) { out.push_back(v_[i].name); kib_ -= KibOf(v_[i].w, v_[i].h); v_.erase(v_.begin() + static_cast<long>(i)); }
            else ++i;
    }
    // Everything (a new stadium, a memory emergency).
    void DropAll(std::vector<uint32_t>& out) { for (const Entry& e : v_) out.push_back(e.name); v_.clear(); kib_ = 0; }
    long Kib() const { return kib_; }
    size_t Count() const { return v_.size(); }
    uint32_t taken = 0, missed = 0, stashed = 0, refused = 0;
private:
    std::vector<Entry> v_;
    long kib_ = 0;
};

} // namespace vgxsizesets
