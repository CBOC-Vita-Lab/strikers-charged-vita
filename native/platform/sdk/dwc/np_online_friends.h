// np_online_friends.h: the friends of the online service as the menus see them (server/PROTOCOL.md section 13).
//
// One small state machine on the game thread. It keeps the last answer of GET /v1/friends (friends with presence, requests, blocked players), runs
// the held poll that keeps it fresh while the online menu is open, and performs the operations one after the other. The game's own friend screens
// reach it through the DWC friend calls (np_dwc_friend.cpp); the platform's extra screens (Played With, Requests, Blocked, "X invites you") call it
// directly. Nothing here blocks; every call returns at once and the result is read on a later frame.
//
//   NpFriends::Start();                     the online menu is open: keep the list fresh (held poll, no more than one request at a time)
//   NpFriends::Stop();                      left the online menu (also automatic while a match is being made or played)
//   NpFriends::Pump();                      once a frame (DWC_ProcessFriendsMatch does it; a screen outside the game's pump calls it itself)
//   const FriendsReply& s = NpFriends::Snapshot();     rows (Friend / Incoming / Outgoing), blocked, ownCode; NpFriends::Revision() changes when it does
//   NpFriends::Add("K7QM-4DXT" or "123456789012") ...  one operation at a time; NpFriends::LastOp() tells how it ended
#ifndef NP_ONLINE_FRIENDS_H
#define NP_ONLINE_FRIENDS_H

#include <stdint.h>

#include <vector>

#include "platform/sdk/dwc/np_online_api.h"

namespace NpFriends {

void Start();
void Stop();
bool Running();
// A hand refresh (O4): drops the held request in flight and asks for the whole list at once. No-op when the poll is not running.
void RefreshNow();
uint32_t ManualRefreshes();   // how many times it ran (tests, the log)
void Pump();
// Free the state (sign-out, tests).
void Reset();

const NpOnline::FriendsReply& Snapshot();
uint32_t Revision();                  // changes whenever Snapshot() or PlayedWith() changed
bool HaveSnapshot();
const NpOnline::FriendRow* Find(int32_t pid);
// Presence for the screens: "In the menu", "Looking for a match" ... are the menus' words; these are the facts.
bool HasInvite(int32_t* fromPid, char* roomCode, size_t size);   // the first friend who invites you, if any

// Operations. Each returns false when it cannot be queued (nothing signed in, queue full, bad text); the answer comes through LastOp().
// `who`: the player's code "XXXX-XXXX" (any case, the dash optional), or the 12 digits of the game's friend key; the other forms take the profile id.
bool Add(const char* who);
bool AddPid(int32_t pid);             // also "Accept" for an incoming request
bool Decline(int32_t pid);
bool Remove(int32_t pid);
bool Block(int32_t pid);
bool Unblock(int32_t pid);
bool Invite(int32_t pid, const char* roomCode);   // "X invites you" for a friend; your own open room
bool ClearInvite(int32_t pid);
bool SetStatus(const char* statusString);         // the game's own status string (DWC_SetOwnStatusData); only the newest is sent

struct OpResult {
    uint32_t serial;                  // counts finished operations
    bool done, ok;
    NpOnline::FriendOp op;
    NpOnline::Fail fail;
    char code[28];                    // the service's error code ("friends_full", "bad_friend", "blocked", "not_friends", "player_not_found" ...)
    NpOnline::FriendState state;      // Add: Friend or Outgoing
    int32_t pid;
    char name[NpOnline::kNameBytes];
    OpResult() : serial(0), done(false), ok(false), op(NpOnline::FriendOp::Add), fail(NpOnline::Fail::None), state(NpOnline::FriendState::Outgoing), pid(0) { code[0] = name[0] = 0; }
};
const OpResult& LastOp();
bool Busy();                          // an operation is queued or running

// "Played with": the last 20 opponents. Refresh asks the service; the rows are valid once PlayedWithReady().
void RefreshPlayedWith();
bool PlayedWithReady();
const std::vector<NpOnline::PlayedRow>& PlayedWith();

// The game's friend key of this account (12 digits) and the key text of another player's profile id; "" when not signed in.
bool OwnKeyText(char* out, size_t size);

// Called by the online match layer: no friends traffic while a match is made or played.
void SetPaused(bool paused);

// Tests: virtual clock for the poll schedule is NpOnline::NowMs().
}  // namespace NpFriends

#endif
