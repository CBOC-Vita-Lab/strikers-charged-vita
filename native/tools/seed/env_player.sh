# source this before the host recordings of a shader seed (batch.sh / rec.sh):
#   . native/tools/seed/env_player.sh
# The switches the Vita player build turns on by itself (native/platform/vita/src/vita_plan.cpp: kSpeed and kQuality lists, the
# multisampled rungs), so that the recorded shader keys are the ones a console makes. The host build does not read vita_plan.cpp: when that
# file gains or loses a default that changes shaders, change this list too. Variables already set in the environment stay.
for kv in MSC_VGX_UNIDUP=2 MSC_VGX_PROMOSMALL=3 MSC_SKIN_GPU=1 MSC_VGX_SOFTINST=16 MSC_VGX_BLENDNODISCARD=1 MSC_VGX_CELLCLEAR=1 \
          MSC_DYNRES_FG=1 MSC_VGX_SHQUAD=1 MSC_DOF=lazy MSC_VGX_SOFTXFORM=256 MSC_VGX_MSAA=4 \
          MSC_GAME_FIXES=net,electro MSC_VGX_RT=1 MSC_AI_BUDGET=off \
          MSC_VGX_CROWDTRIM=1 MSC_VGX_CULL=1 MSC_VGX_FS_TRIM=1 MSC_VGX_VS_TRIM=1 MSC_VGX_VS_NOBRANCH=1 MSC_SCORE_NODEPTH=1 MSC_SHADOW_EVERY=2 MSC_CROWD_REPLAY=2 \
          MSC_FLOOR_BAKE=7,8 MSC_FLOOR_SHEEN=vertex MSC_STADIUM_HEAVY=8 MSC_STADIUM_HEAVY_CROWD=0; do
  name=${kv%%=*}
  [ -n "${!name+x}" ] || export "$kv"
done
