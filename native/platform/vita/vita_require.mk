# Symbols the game's Vita link must DEFINE (-Wl,--require-defined, which also pulls the archive member that holds them).
#
# Why: the game and the platform layer reach these through weak declarations (`extern "C" T f(void) __attribute__((weak))`, `if (f) f();`). A weak reference
# never pulls an object out of an archive. Each one below lives in libnpsdk.a, libnpgx, libvgx-native or libvitaGL.a, and in the builds so far its object was pulled
# only because some other, strong reference happened to need it (found 12 Nov 2026 by reading the link map of build s: vgx_dlcache.o because vgx_play.o calls it, rt.o
# because the renderer calls vglSetAnisotropicFilter, tests.o because gxm.o calls vgl_mk_forget...). A change to those other references would have dropped the
# object silently: the weak call is then a no-op and the feature is off with nothing in the log (the Galaxy port met this with a dropped object). With the symbol required,
# the member is always linked, and a missing definition fails the link instead of the feature.
#
# Not listed: weak references to objects that are linked directly (platform/compat, platform/vita, platform/harness, platform/assets, sdk/net, sdk/dwc, sdk/input,
# sdk/save, the game's own files: a plain object on the link line is always included), hooks that exist only in a harness, lab, PGO or smoke build, and the
# Sce import stubs (those are weak imports by design). tests/test_link_requires.py keeps this list and the sources in step.
NP_REQUIRE_SYMS := \
  VgxStillArm VgxStillDone VgxStillFailed VgxStillRoom VgxRenderScale VgxHeavyStadium VgxStadiumChanged \
  VgxCadenceFull VgxMegaPending VgxMegaSceneHint VgxMegaSharp VgxMegaCutHint VgxMegaStep VgxMegaActive VgxShaderCompiling VgxFgCrowdThrottle \
  VgxFxPassDiv VgxFxPixelScale VgxFxGovLevel VgxFxGovFill VgxHoldShadowStep VgxLock60ShadowStep \
  VgxDynResGrace VgxDynResRoom VgxGpuMinMs VgxHoldStadium VgxGlowHalfPlay VgxLateCostTake VgxPacedSceneHint VgxWarmGoal VgxWarmScene \
  VgxDynResDescribe VgxGfxDescribe vgx_vita_describe vgx_host_memory_describe \
  vgxn_vita_set_services vgxn_vita_wait_idle vgxn_vita_start_failure vgxvita_pin_library_threads \
  vgx_host_last_swap_ms vgx_host_last_dq_ms vgx_host_last_dqadd_ms vgx_host_dq_window vgx_host_pack_hits vgx_tex_has_storage vgx_tex_swizzle vgx_vram_free_kib vgx_host_patch_take vgx_host_pool_free_kib vgx_host_gc_stats np_display_last_vblank \
  np_gx_start np_gx_touch_tex np_gx_register_ram np_gx_layer np_gx_bg30 VgxBg30GpuMs np_gx_hidden np_gx_scene np_gx_fx_lowres np_gx_notify_write np_gx_set_efb_size \
  NpSdk_GetConfig NpSdk_SetAudioSink NpSdk_SetInputSource NpSdk_SetRumbleSink NpSdk_SetQuitHook NpSdk_SetInputBlocked np_plat_set_thread_hook \
  np_time_pause np_time_real_ns np_time_ns np_sleep_ns NpVI_GetRetraceCount VIGetRetraceCount VIWaitForRetrace SCGetAspectRatio \
  NpDiscGetReadError np_in_irq np_irq_release_for_wait np_irq_reacquire_after_wait OSDisableInterrupts OSRestoreInterrupts NpPad_ReadGameCube \
  vglFramebufferDepthShare vglDepthShareStats vglSetStateCache vglSetShaderSeedPath vglSetLazyClear vglSetShaderPackLookup vglCompileShaderToCache vglShaderCacheProbe vglShaderCompilerLockInit \
  vglStrikersBindFramebufferDepth vglStrikersShaderCacheCaps vglStrikersShaderCacheVersion vglFramebufferMultisample vglRtWaitScenesDone vglRtLastGpuFrame \
  vglGetMemFreeStats vglGetMaskKeepStats vglGetMaskBackgroundStats vglSetViewportExact vglSetDrawCaches vglSetMaskKeep vglSetMemFreeMode vglSetGcPauseMs vglSetGcTestInject vglGetGcRecoveryStats vglSetMaskBackground vglSetMaskClip vglGetMaskClipStats vglSceneCensusSetup vglGetSceneCensus vglSetFreeGuard vglSetFreeGuardReferencedOnly vglGetFreeGuardStats vglFreeGuardTake vglFramebufferNoDepth vglFramebufferDepthStats vglSetRenderTargetPool vglGetRenderTargetPoolStats vglBackendTimingTake vglMemSiteDump vglMemSitesOn vglMemSiteRange VgxQuiesceForExit VgxQuiesceCancel VgxHostQuiesceGpu VgxHostWaitUs \
  vglGetUniformShadow vglUniformShadowWritten vglDrawElementsDirect glBlitFramebuffer \
  NpDisplayLastHeld VgxHostAsyncHeapRoom VgxHostAsyncWorkerPlace NpPad_ReadRaw VgxHoldShadowStepFor VgxStadiumIs \
  vgx_host_pack_misses VgxBackendLivePlay NpDvdKeepStats \
  VgxNextCopyClearOnly VgxPpaaMode VgxPpaaCutHint VgxGpuSceneEpoch VgxGpuSceneSkip VgxGpuSampleIdle VgxHeldGovLevel VgxHostMidSceneFlush VgxHostAsyncWorkerAllocStats \
  vglSetFramebufferDetachKeep vglGetGcJobStats vglGetDualRTStats vglFlushStreamArena vglFramebufferDualRT vglTexAnisoOff vglTiledStats vglSetTiledTargets vglTexMakeTiledTarget vglTexIsTiled vglSetUniHistCap vgl_mem_get_type_by_addr vglSetWriteCopyFallback vglWriteCopyStats8 vglSetTextureCopyDma vglTextureCopyStats \
  _ZN3vgx13StadiumSignalEv _ZN3vgx12TexCacheLostEj _ZN3vgx19DlCacheBranchCountsEPy _ZN3vgx12DlKeepActiveEv _ZN3vgx11DlKeepSavedEv _ZN3vgx19GetDrawSwitchCountsERNS_16DrawSwitchCountsE _ZN3vgx14IsBallFinTexIdEj
# (the last six are C++ names in namespace vgx, so they are given mangled; the test checks the plain name against the weak declarations)
NP_REQUIRE_FLAGS := $(addprefix -Wl$(comma)--require-defined=,$(NP_REQUIRE_SYMS))
