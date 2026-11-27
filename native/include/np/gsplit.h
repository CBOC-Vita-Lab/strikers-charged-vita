// Wave 9 (MSC_GSPLIT, logging builds, default off): where the game thread spends a picture. Phase markers at the task / top-level call level
// switch one "current phase" id; the time between two switches goes to the phase that was current (self time, nested phases subtract from their
// parent). A helper thread on another core samples the id every millisecond. One line per 300 pictures, "[gsplit] ...", from np_gsplit.cpp.
// Off: every marker is one load and one predictable branch of g_npGsOn.
// Phases whose names start with "wait" are time the thread is blocked (not CPU); they are reported separately and left out of the sum.
#pragma once
// (gt27: the ids are visible in every build, so that the platform files built without NP_NATIVE - np_framesend.cpp, vita_log.cpp - can mark through the weak calls below)
enum NpGsId {
    NPGS_OUTSIDE = 0,   // between two passes of the task list (the platform's main loop: present bookkeeping, log writes, pad)
    NPGS_RUNNER,        // the task list runner itself (state changes, ticker, movie)
    NPGS_WAIT_SWAP,     // swap_Post: draw done wait + pacing wait
    NPGS_WAIT_JOIN,     // frame-send job join
    NPGS_WAIT_HANDOVER, // renderer library: present handed over, waiting for the front end / back end
    NPGS_WAIT_PACE,     // late pacing wait after the send
    NPGS_SIM_PRE,       // FixedUpdateTask::Run outside the steps (inputs, accumulator)
    NPGS_SIM_CLOCK,     // ClockManager, router, determ events
    NPGS_SIM_AI,        // AIUpdateTask (game update: fielder desires, scripts)
    NPGS_SIM_NPC,       // AI NPCs
    NPGS_SIM_PREPHYS,
    NPGS_SIM_PHYSICS,   // PhysicsUpdate
    NPGS_SIM_POSE,      // PostPhysicsAITask: character animation pose (feeds physics)
    NPGS_SIM_NET,
    NPGS_SIM_EVENTS,    // event dispatch, peach photo, snapshot grab
    NPGS_SIM_PAD,       // pad update after the steps
    NPGS_R_PRE,         // RenderFrame start: peach, nis, lighting update
    NPGS_R_NPCS,        // world NPCs render
    NPGS_R_STAGE,       // jumbotron, crowd manager, flares, warble, indicators, blinking
    NPGS_R_REPLAYSNAP,  // RenderSnapshotAt not otherwise marked
    NPGS_R_BLEND,       // RenderSnapshot::Blend (characters, ball, items)
    NPGS_R_CHARS,       // RenderSnapshot::Render (characters draw, powerups)
    NPGS_R_STADIUM,     // UpdateStadium (world animations, stadium objects)
    NPGS_R_HUD,         // indicators, shoot meter, mega overlay
    NPGS_R_SHADOW,      // darkening, shadow volume blend
    NPGS_R_FX,          // photo flash, electric fence, blur, warble test
    NPGS_R_CROWDPOS,    // impostor positions + visibility
    NPGS_R_CROWDSPR,    // impostor sprites (pose + capture)
    NPGS_R_CROWDDRAW,   // impostor render
    NPGS_R_NUMBERS,    // score display after the frame
    NPGS_P_UPDATE,      // particle update
    NPGS_P_RENDER,      // particle render (BuildParticleQuads ...)
    NPGS_E_WARBLE,      // EndFrame before the send
    NPGS_E_SEND,        // glSendFrame (views, packets, material programs) not otherwise marked
    NPGS_E_SKIN,        // glx_LoadSkinMatrices
    NPGS_AUDIO,         // audio update task
    NPGS_DVD,           // file reads on the game thread
    NPGS_FE,            // front end (menu) task
    // v2 detail phases (self time, nested in the phase named in the comment)
    NPGS_PH_PRECOLLIDE = 38, // physics: PreCollide of every body and space
    NPGS_PH_BROAD,           // physics: the space's pair walk (broad phase), callbacks excluded
    NPGS_PH_NEAR,            // physics: near callback itself (connected test, contact info, result merge)
    NPGS_PH_CONTACTCB,       // physics: PhysicsObject::Contact of both objects
    NPGS_PH_JOINTS,          // physics: dJointCreateContact + attach
    NPGS_PH_NETBALL,         // physics: ball in the net, net AABB upkeep
    NPGS_PH_PREUPD,          // physics: PreUpdate of every body
    NPGS_PH_ISLANDS,         // physics: ODE island search (auto-disable, tagging, traversal)
    NPGS_PH_QS_PREP,         // physics: quick step: inertia, joint info
    NPGS_PH_QS_JAC,          // physics: quick step: Jacobian rows
    NPGS_PH_QS_RHS,          // physics: quick step: right hand side
    NPGS_PH_QS_SOR,          // physics: quick step: SOR LCP solver
    NPGS_PH_QS_INT,          // physics: quick step: velocity / position integration, feedback
    NPGS_PH_POSTUPD,         // physics: PostUpdate of every body
    NPGS_PH_BALLBODY,        // physics: the ball's own PreUpdate / PostUpdate
    NPGS_PH_PHYEV,           // physics: DispatchPhysicsEvents
    NPGS_AI_TEAMPRE,         // ai: cGame::PreUpdate (players' PreUpdate)
    NPGS_AI_MISC,            // ai: cGame::Update head (pad history, visibility, cached data, shockwaves)
    NPGS_AI_TEAMTIMERS,      // ai: team timers
    NPGS_AI_INTERCEPT,       // ai: ball intercept times (perception)
    NPGS_AI_TEAMAI,          // ai: UpdateTeamAI (team logic)
    NPGS_AI_TEAMREST,        // ai: shot score, chant
    NPGS_AI_GAMECTX,         // ai: the game's AI context (script machine of the game)
    NPGS_AI_F_SELF,          // ai: cFielder::Update itself + cPlayer::Update + timers
    NPGS_AI_F_SCRIPT,        // ai: fielder script machine (timers, state selection, activation)
    NPGS_AI_F_VMSTATE,       // ai: fielder script machine: UpdateStateMachine (desire evaluation)
    NPGS_AI_F_ACTION,        // ai: fielder UpdateActionState
    NPGS_AI_F_HEAD,          // ai: fielder head tracking
    NPGS_AI_F_ANIM,         // ai: fielder cCharacter::Update (movement state, scale, dirt)
    NPGS_AI_F_CTRL,          // ai: fielder UpdateController
    NPGS_AI_GOALIE,          // ai: goalie Update
    NPGS_AI_BALL,            // ai: cBall::Update
    NPGS_AI_WORLD,           // ai: weather, powerups, goal check, tilt, post game
    NPGS_EV_DISPATCH,        // events: dispatcher
    NPGS_EV_PEACH,           // events: peach photo
    NPGS_EV_SNAPSHOT,        // events: ReplayManager::GrabSnapshot
    NPGS_BF_PACE,            // begin frame: pacing and fps policy
    NPGS_BF_FILES,           // begin frame: nlServiceFileSystem
    NPGS_BF_PRES,            // begin frame: presentation + nis update
    NPGS_BF_GLBEGIN,         // begin frame: glBeginFrame
    NPGS_BF_CHARS,           // begin frame: per character fn_80022D58
    NPGS_BF_CAMERA,          // begin frame: camera manager update
    NPGS_BF_SETUP,           // begin frame: matrices and render info
    NPGS_FE_INPUT,           // front end: input
    NPGS_FE_PRES,            // front end: presentation update
    NPGS_FE_DRAW,            // front end: DrawFrontEndElements
    NPGS_RB_POSE,            // blend: pose cull decision
    NPGS_RB_CHARS,           // blend: characters + powerups (the helper-thread jobs included)
    NPGS_RC_CHARS,           // chars draw: the ten characters
    NPGS_RC_POWERUPS,        // chars draw: the 150 powerup slots
    // gt27 (18 Nov): the rest of the game thread outside the step and the render task. Waits first (their own range: IsWait), then work.
    NPGS_WAIT_PADLATE = 88,  // wait: the late pad read's sleep (MSC_PAD_LATE / press16 Lat16Wait), was booked as sim-pre work
    NPGS_WAIT_GATE,          // wait: a frame-send gate (NpFsGate / NpFsGateSlow) outside a join's own scope
    NPGS_WAIT_OTHER,         // wait: any other blocked time named by a caller (spare)
    NPGS_SP_INPUT,           // sim-pre: per-step pad / plat pad / flick / CaptureInputs / PrepareUpdate in the step loop
    NPGS_SP_EDGE,            // sim-pre: the probes around each step (det trace, diag, simt markers, fps sim timing)
    NPGS_SP_PADCTL,          // sim-pre: the late pad read's controller (no sleep)
    NPGS_SP_LAN,             // sim-pre: LAN drain + MSC_SIMT run wrapper + window pending
    NPGS_SP_TAIL,            // sim: frame lock + blend step + diag after the loop (before 'pad')
    NPGS_SP_LOOP,            // sim-pre: the step loop's own rest between the input reads and the step
    NPGS_BF_FPS,             // begin frame: NpFpsBeginFrame (the presentation controller, [head]/[lane] lines); bf-pace keeps NpPaceBeginFrame
    NPGS_BF_PROBE,           // begin frame: the controller's per-core probe (thread-info reads)
    NPGS_E_STILL,            // end frame: still backdrop + amortize before / after the send
    NPGS_E_JOINWORK,         // end frame: the frame-send join's work after its wait (stores, deferred GX calls, write-backs, deferred frees)
    NPGS_E_SNAPVIEWS,        // end frame: SnapshotViews (shadows of the live views)
    NPGS_E_ATFORK,           // end frame: swap pre-fork + stadium time / emission lights / lighting / fog captures
    NPGS_E_PKTREFRESH,       // end frame: GLRefreshPacketSnapshots (packet copies for the job)
    NPGS_E_FORK,             // end frame: arena flip + NpFsFork
    NPGS_E_HOOK,             // end frame: the frame hook (NpFramePresentedFromGame: harness / nav / [lat] bookkeeping)
    NPGS_E_TAIL,             // end frame: after the send (frame counter, HBM check)
    NPGS_E_SENDTAIL,         // end frame: the forked send after the hook (serialise join, late-wait controller outside its wait)
    NPGS_RUN_TICKER,         // runner: the per-task ticker reads and delta
    NPGS_RUN_HOOKS,          // runner: NpTaskBegin / NpTaskEnd (lane / stall bookkeeping) and the simt probes
    NPGS_RUN_MOVIE,          // runner: MoviePlay after every task
    NPGS_FE_LIGHT,           // fe draw: UpdateGameObjectLighting
    NPGS_FE_UPDATE,          // fe draw: FrontEnd::Update (HUD logic)
    NPGS_FE_RES,             // fe draw: FEResourceManager::Run
    NPGS_FE_SCUPD,           // fe draw: FESceneManager::Update (HUD scene logic / tweens)
    NPGS_FE_SCRENDER,        // fe draw: FESceneManager::RenderActiveScenes (HUD drawing)
    NPGS_FE_MODELS,          // fe draw: FEModelManager update + render
    NPGS_FE_REST,            // fe draw: menu stadium, home button fade
    NPGS_MEM,                // memory: game-heap AllocateMemory / FreeMemory on the game thread (nested anywhere: counted out of its parent)
    NPGS_LOG,                // logging: the stdio write hook (VitaLog::Write: ring + buffer copy) on the game thread
    NPGS_AUDIO_MIX,          // audio: spare id for the audio task's parts
    NPGS_FIXED_NAMED = 128,
    NPGS_TASK0 = 128,   // dynamic slots (task names, class pair names), up to NPGS_MAX
    NPGS_MAX = 224
};
// Counters (per print window, divided by the steps or pictures in the line)
enum NpGsCounter {
    NPGC_GEOMS = 0,      // enabled geoms offered to a simple space's pair walk
    NPGC_PAIRS,          // pairs walked (n(n-1)/2 with the broad-phase filter, else collideAABBs calls)
    NPGC_AABB,           // collideAABBs calls made
    NPGC_CALLBACK,       // pairs that reached the near callback
    NPGC_CB_CONNECTED,   // callback rejected: bodies joined by a joint
    NPGC_CB_NOCOLL,      // callback rejected: SetContactInfo refused
    NPGC_NARROW,         // dCollide calls
    NPGC_NARROW_HIT,     // dCollide calls that found contacts
    NPGC_CONTACTS,       // contact points returned
    NPGC_JOINTS,         // contact joints created
    NPGC_BODIES,         // bodies visited by PreUpdate
    NPGC_ISLANDS,
    NPGC_ISL_BODIES,
    NPGC_ISL_JOINTS,
    NPGC_ISL_ROWS,
    NPGC_ISL_MAXBODIES,  // largest island (maximum, not a sum)
    NPGC_AI_SCRIPTS,     // ScriptMachine::Update calls
    NPGC_AI_VMSTATES,    // UpdateStateMachine calls
    NPGC_AI_FIELDERS,
    NPGC_BLEND_CHARS,
    NPGC_DRAW_CHARS,
    NPGC_BLEND_ITEMS,
    NPGC_MEM_ALLOC,      // gt27: game-heap allocations on the game thread (per picture)
    NPGC_MEM_FREE,       // gt27: game-heap frees on the game thread
    NPGC_MEM_BYTES,      // gt27: bytes asked for by those allocations
    NPGC_LOG_LINES,      // gt27: stdio write-hook calls on the game thread (one a line)
    NPGC_LOG_BYTES,      // gt27: their bytes
    NPGC_MAX = 32
};
#ifdef NP_NATIVE
extern "C" {
extern int g_npGsOn;                 // 0 off, 1 phases, 2 phases + 1 ms sampler (set once, by NpGsPassBegin's first call)
int NpGsEnter(int id);               // makes `id` current; returns the previous id (or -1 when called from another thread than the game thread)
void NpGsLeave(int prev);
int NpGsTaskId(const char* taskName);
void NpGsPassBegin(void);            // top of nlTaskManager::RunAllTasks (first call reads MSC_GSPLIT)
void NpGsPassEnd(void);              // end of RunAllTasks: one picture; prints every 300
void NpGsWaitBegin(void);            // for the renderer library's waits (weak there)
void NpGsWaitEnd(void);
extern unsigned int g_npGsCnt[32];
int NpGsPairId(int kind, int c1, int c2);   // dynamic phase id named "<kind>:<class1>-<class2>" (kind 0 narrow phase, 1 PreUpdate by geom class, 2 PostUpdate)
}
struct NpGsScope {
    int p;
    inline NpGsScope(int id) : p(-1) { if (__builtin_expect(g_npGsOn != 0, 0)) p = NpGsEnter(id); }
    inline ~NpGsScope() { if (__builtin_expect(p >= 0, 0)) NpGsLeave(p); }
};
// A sequence of sibling phases in one function: Next() ends the current one and starts the next (no nesting).
struct NpGsSeq {
    int outer;
    inline NpGsSeq(int id) : outer(-1) { if (__builtin_expect(g_npGsOn != 0, 0)) outer = NpGsEnter(id); }
    inline void Next(int id) { if (__builtin_expect(outer >= 0, 0)) { NpGsLeave(outer); NpGsEnter(id); } }
    inline ~NpGsSeq() { if (__builtin_expect(outer >= 0, 0)) NpGsLeave(outer); }
};
#define NP_GS_CAT2(a, b) a##b
#define NP_GS_CAT(a, b) NP_GS_CAT2(a, b)
#define NP_GS(id) NpGsScope NP_GS_CAT(npGsScope_, __LINE__)(id)
#define NP_GSC(c, n) do { if (__builtin_expect(g_npGsOn != 0, 0)) g_npGsCnt[c] += (unsigned)(n); } while (0)
#else
#define NP_GS(id) ((void)0)
#define NP_GSC(c, n) ((void)0)
#define g_npGsOn 0
extern unsigned int g_npGsCnt[32];
struct NpGsSeq { inline NpGsSeq(int) {} inline void Next(int) {} };

#endif
// gt27: files built without NP_NATIVE (the Vita platform layer) mark through these weak calls; np_gsplit.cpp (platform/compat, linked directly) defines them. Off (or not linked: the unit tests): -1, nothing.
extern "C" int NpGsEnterIf(int id) __attribute__((weak));               // NpGsEnter when MSC_GSPLIT is on, else -1
extern "C" void NpGsLeaveIf(int prev) __attribute__((weak));
extern "C" void NpGsCountIf(int counter, unsigned n) __attribute__((weak));
struct NpGsSeqW {
    int outer;
    explicit NpGsSeqW(int id) : outer(&NpGsEnterIf != 0 ? NpGsEnterIf(id) : -1) {}
    void Next(int id) { if (outer >= 0) { NpGsLeaveIf(outer); NpGsEnterIf(id); } }
    bool On() const { return outer >= 0; }
    ~NpGsSeqW() { if (outer >= 0) NpGsLeaveIf(outer); }
};
