// Build defaults that the plan (vita_plan.cpp) and the Settings rows (np_settings.cpp) must agree on: a row's first choice writes nothing and has to be what the build does
// when the file names nothing, so the row reads the same switch the plan does.
#pragma once

namespace VitaPlanDefaults {

// kLock60: SMOOTH 60 (MSC_LOCK60=1, docs/TECHNICAL.md "LOCKED 60") is the build's frame-rate mode when np-player.txt names none (23 Nov console A/B: live play 30.1 -> 37.8 fps Vice,
// 30.0 -> 36.9 The Dump, less judder). It must equal kLock60Default in vita_plan.cpp (the plan sets MSC_LOCK60=1 by itself when that is true; audit_settings_keys.py checks the two
// agree). The FRAME RATE row (np_settings.cpp) then offers SMOOTH 60 / 30 LOCKED, with SMOOTH 60 writing nothing; false would give AUTO / SMOOTH 60 / 30 LOCKED.
constexpr bool kLock60 = true;

}  // namespace VitaPlanDefaults
