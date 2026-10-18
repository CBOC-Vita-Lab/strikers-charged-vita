/* np/itemtest.h: the test-only item hook of the native port (W5-ITEM, wave 5). Not part of the decomp; docs/wave5-ITEM.md.
 *
 * MSC_ITEM_TEST=1 (environment, env.txt on the Vita) switches it on; without it nothing is queued, nothing is logged and the game is retail.
 *
 *   give         the harness script command `item <side> <type> [count] [set|add]` (platform/harness) calls NpItemTestQueue on the game
 *                thread. The request goes into the game's OWN deterministic data stream (InputRouter::QueueDetermData, the way the game sends
 *                the Mega Strike results and the on-screen flags): it travels with the input of the tick it was captured in, and every
 *                console applies it at the same simulation step (cGame::ReceiveCustomDetermData, custom determ type 240). So a LAN match
 *                stays in step; single player takes the same path through the local router. On receipt the item is given with the game's
 *                own call (cTeam::SetCurrentPowerUp, what PowerupBase::AwardPowerup does after its draw; `set` empties both slots first).
 *                A console applies a received request whatever its own switch says (the request exists only because the sender's
 *                switch was on, and skipping it would desynchronise the match).
 *   observe      per presented frame (harness frame end, read only): the teams' item slots, the live item objects (g_pPowerups), the
 *                Chain Chomp, and per fielder the status effects (photo-frozen, shrunk, star, mushroom, giant, super ability, electrocuted,
 *                fallen, iced, burning, confused, slippery, stuck, gooey, shattered) -> "[item]" lines on change; item hits on
 *                players and keepers are noted where the game detects them (PowerupBase::CollisionCallback); "[itemsum]" at exit.
 *   aim          NpItemTestAim: the left stick that walks the local controlled fielder toward a target (nearest opponent, ball
 *                owner, the goal it attacks, the other keeper, the ball), for the harness command `aim`.
 *   variables    NpItemTestVar: item0 item0b itemn0 item1 item1b itemn1 pups chomp fx0 fx1 hits hitk dopp downer dgoal dkeep dball
 *                own side ctl for the harness's wait / if conditions.
 */
#ifndef NP_ITEMTEST_H
#define NP_ITEMTEST_H

#define NP_ITEMTEST_DETERM_TYPE 240

#ifdef __cplusplus
extern "C" {
#endif
int NpItemTestEnabled(void);
/* harness: queue a give request. side 0/1, or -1 = the side of this console's controlled player; type = ePowerUpType (0..20) or
   -2 = the captain item of that team; mode 0 = add (first free slot), 1 = set (both slots emptied first). Returns 1 if queued. */
int NpItemTestQueue(int side, int type, int count, int mode);
/* cGame::ReceiveCustomDetermData, determ type 240 */
void NpItemTestReceive(const unsigned char* data, int size);
/* PowerupBase::CollisionCallback: an item touched a player (keeper = 1 for a goalie) */
void NpItemTestNoteHit(int type, int size, const void* thrower, const void* target, int keeper);
/* harness frame end */
void NpItemTestFrame(unsigned long long frame);
/* harness: stick toward target 0 opp, 1 owner, 2 goal, 3 keeper, 4 ball; returns 0 when there is no controlled fielder */
int NpItemTestAim(int target, int* sx, int* sy);
/* harness: a script variable; returns 0 if the name is not one of this module's */
int NpItemTestVar(const char* name, double* out);
#ifdef __cplusplus
}
#endif

#endif
