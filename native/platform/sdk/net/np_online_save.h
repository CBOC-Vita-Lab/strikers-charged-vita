// np_online_save.h: when leaving internet play may write the single-player save (NetworkSession::ShutdownOnline).
//
// Old rule (the original game's): leaving online always saved. New default (owner's decision, Galaxy's rule):
//  1. save only when something changed in this session. What counts as changed: the stats manager's mSaveDataChanged (a match result recorded in the
//     win / loss counters, NetworkStatsManager.cpp: RecordResult paths and the sync of the pending counters with the leaderboard record); a save that
//     RefreshRankings already made clears it, so a session whose changes are already on the card does not save again;
//  2. a session that ended by error (link lost, desync, service refused, certificate / clock refusal, stats service failure) saves nothing at all;
//  3. LAN never saves (ShutdownLAN does not call it).
// MSC_ONLINE_SAVE_ALWAYS=1 gives the old behaviour back. Every decision is logged ("[onlinesave] ...").
#pragma once

namespace NpOnlineSave {

inline bool& ErrorLatch() { static bool latch = false; return latch; }
inline void NoteError() { ErrorLatch() = true; }
inline void Begin() { ErrorLatch() = false; }   // a new online session starts clean

// The rule as one function: `always` is MSC_ONLINE_SAVE_ALWAYS.
inline bool Wanted(bool always, bool changed, bool endedByError) { return always || (changed && !endedByError); }

}  // namespace NpOnlineSave
