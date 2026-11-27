#include "NL/nlDLListContainer.inl"
#include "NL/nlPrint.h"
#include "Game/Render/ImpostorCharacter.h"

#include "Game/Render/Impostor.h"
#include "Game/Render/ImpostorManager.h"
#include "Game/SharedStaticStorage.h"
#include "NL/gl/gl.h"
#include "NL/gl/glMemory.h"
#include "NL/gl/glView.h"
#include "NL/nlMath.h"
#include "NL/nlMemory.h"
#include "np/overlap.h"
#include "np/crowdcut.h"
#include "np/crowdgov.h"

#include <math.h>
#include "Game/TweakValue.inl"

u32 gNumImpostorTextureSetsCreated;
static int sNextCharacterSerial;   // MSC_CROWD_SIZED: the order of the characters, which spreads their groups over the pictures
static bool sSamePoseAsLastSprite;   // (MSC_CROWD_POSEONCE) UpdateSprites -> AnimatedImpostorCharacter::Render

ImpostorCharacter::ImpostorCharacter(const char* name, int budget,
    int numAngles, int numTextures, const ImpostorCharacterParams* params)
    : mNumAngles(numAngles)
    , mNumTextures(numTextures)
    , mIsCluster(false)
    , mWidth(0x40)
    , mHeight(0x40)
    , mUseAdditiveBlend(false)
    , mUseIntensityAlpha(false)
    , mBaseAngle(0)
    , mName(name)
    , mSerial(sNextCharacterSerial++)
{
    if (params != 0)
    {
        mWidth = params->mWidth;
        mHeight = params->mHeight;
        mUseAdditiveBlend = params->mUseAdditiveBlend;
        mUseIntensityAlpha = params->mUseIntensityAlpha;
        mBaseAngle = params->mBaseAngle;
    }

    char pathBuffer[0x80];
    nlSNPrintf(pathBuffer, 0x80, "/Render/Impostor/CharacterTweaks/%s", name);

    mfScale.BindWithDefault("mfScale", 1.0f, pathBuffer, true, 0.0f, 3.0f, 0.001f);
    mfCameraLookatZ.BindWithDefault("mfCameraLookatZ", 1.2f, pathBuffer, true, 0.0f, 10.0f, 0.01f);
    mfCameraDistance.BindWithDefault("mfCameraDistance", 2.3f, pathBuffer, true, 0.0f, 40.0f, 0.01f);

    glBeginResource("ImpostorCharacter");

    float angleDegrees = 360.0f / (float)mNumAngles;
    u16 angleStep = DegreesToAngle(angleDegrees);
    int j;
    for (int i = 0; i < mNumTextures; ++i)
    {
        // Retail computes this per-texture stagger value and discards it; the
        // integer conversion it forces is part of the retained object.
        float stagger = nlRandomf(0.0f, 5.0f, &nlDefaultSeed) + (float)i;
        for (j = 0; j < mNumAngles; ++j)
        {
            ImpostorSprite* sprite = new ImpostorSprite(
                this, i, budget / (mNumAngles * mNumTextures), mWidth, mHeight);
            sprite->mUseIntensityAlpha = mUseIntensityAlpha;

            char nameBuffer[0x40];
            nlSNPrintf(nameBuffer, 0x40, "Impostor-%s%d", mName, i * mNumAngles + j);
            sprite->Initialize(nameBuffer);

            sprite->mAngle = mBaseAngle + j * angleStep;
            mSprites.AddEnd(sprite);
        }
        gNumImpostorTextureSetsCreated++;
    }
    glEndResource();

    ImpostorManager::GetInstance()->AddCharacter(this);
}

static inline void DestroyImpostorSprites(
    nlDLListSlotPool<ImpostorSprite*>& sprites,
    nlDLListIterator<ImpostorSprite*>& it)
{
    while (it.m_Curr != 0)
    {
        ImpostorSprite* sprite = it.m_Curr->entry;
        delete sprite;
        sprites.Remove(&it);
    }
    sprites.Clear();
    sprites.m_Allocator.FreeBlocks();
}

ImpostorCharacter::~ImpostorCharacter()
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DestroyImpostorSprites(mSprites, it);
}

u16 QuantizeImpostorAngle(u16 target, int count)
{
    u16 step = (u16)((int)(65536.0f * (360.0f / (float)count)) / 360);
    int angle;
    int bestDistance = 0x8000;
    u16 bestAngle = 0;
    angle = 0;
    for (int i = 0; i < count; ++i)
    {
        s16 distance = (s16)((u16)angle - target);
        u16 absDistance = distance < 0 ? -distance : distance;
        if (absDistance < (u16)bestDistance)
        {
            bestDistance = absDistance;
            bestAngle = angle;
        }
        angle += step;
    }
    return bestAngle;
}

void ImpostorCharacter::Acquire(Impostor* impostor)
{
    ImpostorSprite* best = 0;
    float pick = floor(nlRandomf(0.0f, (float)mNumTextures, &nlDefaultSeed));
    DLListEntry<ImpostorSprite*>* head;
    DLListEntry<ImpostorSprite*>* entry;
    int index = (int)pick;
    int current = 0;
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    head = it.m_Head;
    entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        if (sprite->mAngle == impostor->mAngle)
        {
            best = sprite;
            if (index == current)
            {
                break;
            }
            current++;
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }

    best->AddImpostorSlot(impostor->mSlot);
    impostor->mpSprite = best;
}

void ImpostorCharacter::ReleaseSprites()
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        entry->entry->ClearImpostorSlots();
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void ImpostorCharacter::UpdateView(const nlVector3* direction,
    const nlVector3* up)
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        entry->entry->UpdateView(direction, up);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

static inline bool CrowdSpriteDue(ImpostorCharacter* ch, ImpostorSprite* sprite, int key, int period, int slot, bool count);   // (crowd25, below)

void ImpostorCharacter::CollectPoseModels(int period, int slot, bool lazyView, ImpostorModel** out, int* count, int max, ImpostorSprite** sprites, int* nSprites, int maxSprites)
{
    if (mIsCluster != 0) return;
    const bool grouped = (NpCrowdGT() & 2) != 0;
    const int groupBase = mSerial * (mNumTextures + 1);
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        if (CrowdSpriteDue(this, sprite, grouped ? (groupBase + sprite->mTextureIndex) : sprite->mID, period, slot, false) && sprite->mNumRenderSlots > 0)
        {
            if (lazyView && *nSprites < maxSprites) sprites[(*nSprites)++] = sprite;   // (UpdateSprites makes its view here: bit 4 of MSC_CROWD_GT does not skip a sprite with slots)
            ImpostorModel* model = PoseModel(sprite->mTextureIndex);
            if (model != 0 && model->mPoseAccumulator != 0)
            {
                bool have = false;
                for (int i = 0; i < *count; ++i)
                {
                    if (out[i] == model) { have = true; break; }
                }
                if (!have && *count < max) out[(*count)++] = model;
            }
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0) entry = 0;
        else entry = entry->m_next;
    }
}

// crowd25 (MSC_CROWD_GOV, np/crowdgov.h). Set by ImpostorManager around the gameplay camera's UpdateSpritesWithFloor only; elsewhere the game's rule (key % period == slot) stands.
bool gCrowdGovActive;
unsigned gCrowdGovTick;
unsigned gCrowdGovWake = 0u - 1000u;   // the crowd picture of the last animation change (PlayAnimation): the crowd is left alone for a moment so a cheer plays at its own pace
unsigned gCrowdGovBankTick;            // the crowd picture on which the governor last asked for the banked clock
bool gCrowdGovBank;
static const unsigned kGovWakePictures = 48;
static const float kGovMaxAdvance = 0.25f;   // most animation time a model's clock moves in one redraw: a long-idle model plays on at a gentle pace and never jumps
static const float kGovMaxBank = 1.5f;       // what is banked beyond this is dropped (the animations are cycles: the phase of a section does not matter)

static inline bool GovAwake() { return gCrowdGovTick - gCrowdGovWake < kGovWakePictures; }
bool CrowdGovBanking() { return gCrowdGovBank && gCrowdGovTick - gCrowdGovBankTick <= 2u && !GovAwake(); }

// The model's clock moves to the pose about to be drawn: its banked time, at most kGovMaxAdvance of it (once a picture however many of its sprites are redrawn).
static inline void GovAdvance(ImpostorModel* m)
{
    if (m == 0 || m->mGovAdvTick == gCrowdGovTick) return;
    m->mGovAdvTick = gCrowdGovTick;
    float p = m->mGovPending;
    if (p <= 0.0f) return;
    const float adv = p < kGovMaxAdvance ? p : kGovMaxAdvance;
    p -= adv;
    m->mGovPending = p < kGovMaxBank ? p : kGovMaxBank;
    m->UpdateAnimation(adv);
}

// Is this sprite redrawn on this picture? The game's rule, unless the governor gave the sprite's model a longer interval for its size class. A model's sprites then share one interval and key
// (all the angles of a figure are redrawn together), so the model's clock stops while none is.
static inline bool CrowdSpriteDue(ImpostorCharacter* ch, ImpostorSprite* sprite, int key, int period, int slot, bool count)
{
    bool due = key % period == slot;
    if (!gCrowdGovActive) return due;
    ImpostorModel* m = ch->PoseModel(sprite->mTextureIndex);
    if (m == 0) return due;
    if (m->mGovLevel > 0 && !GovAwake())
    {
        const int mult2 = NpCrowdGovMult2(m->mGovLevel, m->mGovClass);
        if (mult2 > 2)
        {
            const bool slow = !m->mGovCaptured || NpCrowdGovDue(gCrowdGovTick, ch->mSerial * (ch->mNumTextures + 1) + sprite->mTextureIndex, NpCrowdGovPeriod(period, mult2));
            if (count && sprite->mNumRenderSlots > 0)
            {
                if (due && !slow) NpCrowdGovCount(3, 1);
                else if (slow)
                {
                    NpCrowdGovCount(2, 1);
                    if (mult2 >= 20) NpCrowdGovCount(4, 1);
                }
            }
            due = slow;
        }
    }
    if (due && sprite->mNumRenderSlots > 0)
    {
        m->mGovCaptured = true;
        GovAdvance(m);
    }
    return due;
}

// Size class of every model (1 near .. 3 far) from the nearest of up to 8 figures of each of its sprites, a quarter of the sprites a picture (by sprite number): cheap beside what a longer interval saves.
void ImpostorCharacter::MeasureNear(const Impostor* impostors, const NpCrowdViewInfo& view, unsigned tick)
{
    // (Measured at the gameplay camera, Vice and The Dump: the nearest figure of a sprite is 64-80 lines high in 80-100 % of the sprites and every model has sprites of every angle spread over the whole crowd, so
    // a split by the NEAREST figure of a model puts every model in "near" (MSC_CROWD_SIZED's 40/20 lines too). A model's class is that of the mean of its sprites' nearest figures, against thresholds on that scale.)
    static const float sT1 = 110.0f, sT2 = 55.0f;
    enum { kMaxTex = 64 };
    float sum[kMaxTex];
    int cnt[kMaxTex];
    for (int t = 0; t < kMaxTex; ++t) { sum[t] = 0.0f; cnt[t] = 0; }
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        const int n = sprite->mNumRenderSlots;
        if (n > 0 && (((unsigned)sprite->mID + tick) % 4u == 0 || sprite->mSizedStep == 0))
        {
            const int stride = n > 8 ? n / 8 : 1;
            float nearest = 0.0f;
            for (int k = 0; k < n; k += stride)
            {
                const Impostor* im = &impostors[sprite->mRenderSlots[k]];
                const nlVector3& p = im->mPosition;
                const float depth = -(p.x * view.depthRow[0] + p.y * view.depthRow[1] + (p.z + 0.5f * im->GetHeight()) * view.depthRow[2] + view.depthRow[3]);
                const float px = NpCrowdFigurePx(view.figureScale * im->GetHeight(), depth, view.projY);
                if (px > nearest) nearest = px;
            }
            sprite->mNearPx = nearest;
            sprite->mSizedStep = NpCrowdClass(nearest, sprite->mSizedStep, sT1, sT2, 0.15f);
            NpCrowdGovCount(0, 1);
            {
                static const float edge[7] = { 60.0f, 64.0f, 68.0f, 72.0f, 76.0f, 80.0f, 100.0f };
                int b = 0;
                while (b < 7 && nearest >= edge[b]) ++b;
                NpCrowdGovCount(12 + b, 1);
            }
        }
        if (n > 0 && sprite->mTextureIndex >= 0 && sprite->mTextureIndex < kMaxTex)
        {
            sum[sprite->mTextureIndex] += sprite->mNearPx;
            ++cnt[sprite->mTextureIndex];
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0) entry = 0;
        else entry = entry->m_next;
    }
    for (int t = 0; t < mNumTextures && t < kMaxTex; ++t)
    {
        ImpostorModel* m = PoseModel(t);
        if (m != 0)
        {
            if (cnt[t] > 0 && t < kMaxTex) m->mGovClass = (unsigned char)NpCrowdClass(sum[t] / (float)cnt[t], m->mGovClass, sT1, sT2, 0.15f);
            NpCrowdGovCount(7 + (int)m->mGovClass, 1);
        }
    }
}

// Moves each model's own level one notch towards the governor's, one model in twelve a picture, the far class first on a step down, the near class first on a give-back. Returns true while a model is off the target.
bool ImpostorCharacter::GovFollow(int target, unsigned tick, unsigned sinceChange)
{
    bool off = false;
    for (int t = 0; t < mNumTextures; ++t)
    {
        ImpostorModel* m = PoseModel(t);
        if (m == 0 || m->mGovLevel == target) continue;
        off = true;
        const int cls = m->mGovClass < 1 ? 1 : m->mGovClass;
        const unsigned delay = m->mGovLevel < target ? 8u * (unsigned)(3 - cls) : 8u * (unsigned)(cls - 1);
        if (sinceChange >= delay && ((unsigned)(mSerial * (mNumTextures + 1) + t) + tick) % 12u == 0)
        {
            m->mGovLevel = (signed char)(m->mGovLevel + (m->mGovLevel < target ? 1 : -1));
            NpCrowdGovCount(1, 1);
        }
    }
    return off;
}

void ImpostorCharacter::UpdateSprites(int period, int slot, bool lazyView, const nlVector3* direction, const nlVector3* up)
{
    DLListEntry<ImpostorSprite*>* head;
    DLListEntry<ImpostorSprite*>* entry;
    int lastTexture = -1;
    // MSC_CROWD_GT bit 2: the sprites of one texture (one model, one pose, seen from numAngles angles) are captured on the same picture, so the pose (animation, node and skin
    // matrices) is made once for all of them. By sprite ID a picture got every other angle of every texture, and each texture was posed on every picture. Each sprite is still
    // captured once in `period` pictures: only the picture inside the period moves, and the animation phase of an angle shifts by up to one picture.
    // The cluster keeps the ID rule.
    const bool grouped = (NpCrowdGT() & 2) != 0 && mIsCluster == 0;
    const int groupBase = mSerial * (mNumTextures + 1);
    const int gtMode = NpCrowdGT();

    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    head = it.m_Head;
    entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        if (CrowdSpriteDue(this, sprite, grouped ? (groupBase + sprite->mTextureIndex) : sprite->mID, period, slot, true))
        {
            if (lazyView && ((gtMode & 4) == 0 || sprite->mNumRenderSlots > 0 || mIsCluster != 0))
            {
                if (sprite->mViewPrepared)
                {
                    sprite->mViewPrepared = false;   // MSC_POSE_JOBS: made on a lane
                }
                else
                {
                    sprite->UpdateView(direction, up);   // MSC_CROWD_GT
                }
                NpCrowdGTCount(1, 1);
            }
            else if (lazyView)
            {
                NpCrowdGTCount(0, 1);   // bit 3: nothing is drawn into this sprite's picture, so its view is not made
            }
            // MSC_CROWD_BLANKSKIP (gem2a): a due sprite without figures is captured as the blank picture the game makes (clear + copy with nothing drawn). Once its picture is that blank one
            // (the last capture was the empty one) the same capture would write the same pixels again, so it is not made: the sprite is left as one that is not due (no target).
            const bool emptyDue = sprite->mNumRenderSlots == 0 && mIsCluster == 0;
            if (emptyDue && NpCrowdBlankSkip() != 0 && (sprite->mBlankCaptured || NpCrowdBlankSkip() == 3))   // (3: Gemini's form, an empty due sprite is never captured, whatever its picture holds; proof only)
            {
                NpCrowdBlankCount(1);
                sprite->SuspendCapture();
            }
            else
            {
            sprite->ResumeCapture();
            if (NpCrowdBlankSkip())
            {
                if (!emptyDue) { if (NpCrowdBlankSkip() != 2) sprite->mBlankCaptured = false; }   // (2: the proof mutant never takes it down)
                else if (sprite->mView->m_Target == GLViewTarget_Mode9 && sprite->mView->m_Viewport.width != 0) { sprite->mBlankCaptured = true; NpCrowdBlankCount(0); }
            }
            if (sprite->mNumRenderSlots > 0 || mIsCluster != 0)
            {
                sSamePoseAsLastSprite = lastTexture == sprite->mTextureIndex;
                if (lastTexture != sprite->mTextureIndex)
                {
                    EvaluatePose(sprite->mTextureIndex);
                    lastTexture = sprite->mTextureIndex;
                    NpCrowdGTCount(6, 1);
                }
                NpCrowdGTCount(7, 1);
                Render(sprite->mView,
                    sprite->mTextureIndex);
                sSamePoseAsLastSprite = false;
            }
            }
        }
        else
        {
            if (lazyView) NpCrowdGTCount(0, 1);
            sprite->SuspendCapture();
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

// MSC_CROWD_SIZED. The height in lines of the nearest figure of each sprite that has figures to show: measured on the picture after the first visibility pass (the slots of the
// main view), at most every second picture, from at most 24 of its figures (every k-th, so the cost does not follow the crowd). A sprite with none has 0.
void ImpostorCharacter::MeasureSprites(const Impostor* impostors, const NpCrowdViewInfo& view, unsigned frame)
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        const int n = sprite->mNumRenderSlots;
        if (n == 0)
        {
            sprite->mNearPx = 0.0f;
            sprite->mNearFrame = (int)frame;
        }
        else if (sprite->mNearFrame < 0 || (int)frame - sprite->mNearFrame >= 2 || sprite->mNearPx == 0.0f)
        {
            const int stride = n > 24 ? n / 24 : 1;
            float best = 0.0f;
            for (int k = 0; k < n; k += stride)
            {
                const Impostor* im = &impostors[sprite->mRenderSlots[k]];
                const nlVector3& p = im->mPosition;
                const float depth = -(p.x * view.depthRow[0] + p.y * view.depthRow[1] + (p.z + 0.5f * im->GetHeight()) * view.depthRow[2] + view.depthRow[3]);
                const float px = NpCrowdFigurePx(view.figureScale * im->GetHeight(), depth, view.projY);
                if (px > best) best = px;
            }
            sprite->mNearPx = best;
            sprite->mNearFrame = (int)frame;
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

// MSC_CROWD_SIZED: what UpdateSprites does with a period, with the period chosen per group of sprites (one model and texture, seen from every angle) by the size of the nearest
// figure of any of them. A group that is not drawn on this picture keeps its pictures (SuspendCapture) and nothing is posed. The pose of a group is evaluated once, as
// MSC_CROWD_POSEONCE does for the sprites in a row. A sprite that had no figures on the previous picture is drawn at once. Returns the number of sprites drawn.
int ImpostorCharacter::UpdateSpritesSized(unsigned frame, int minStep, bool at60, bool lazyView, const nlVector3* direction, const nlVector3* up, int* inView, int* groups)
{
    enum { kMaxGroups = 8 };
    static const int sMax = NpCrowdSizedMax();
    static const float sT1 = NpCrowdSizedPx(0), sT2 = NpCrowdSizedPx(1);
    int groupClass[kMaxGroups];
    bool groupFresh[kMaxGroups];
    bool groupHasFigures[kMaxGroups];
    for (int g = 0; g < kMaxGroups; ++g)
    {
        groupClass[g] = 3;
        groupFresh[g] = false;
        groupHasFigures[g] = false;
    }

    DLListEntry<ImpostorSprite*>* head;
    DLListEntry<ImpostorSprite*>* entry;
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    head = it.m_Head;
    entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        const int g = sprite->mTextureIndex;
        if (g >= 0 && g < kMaxGroups && sprite->mNumRenderSlots > 0 && !mIsCluster)
        {
            groupHasFigures[g] = true;
            const int c = NpCrowdClass(sprite->mNearPx, sprite->mSizedStep, sT1, sT2, 0.15f);
            sprite->mSizedStep = c;
            groupClass[g] = NpCrowdGroupClass(groupClass[g], c);
            if (!sprite->mSeenLast) groupFresh[g] = true;
        }
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }

    int lastTexture = -1;
    int drawn = 0;
    bool groupDrawn[kMaxGroups] = { false };
    it = mSprites.Begin();
    head = it.m_Head;
    entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        const int g = sprite->mTextureIndex;
        const bool hasFigures = sprite->mNumRenderSlots > 0 || mIsCluster != 0;
        bool due;
        if (mIsCluster)
        {
            // the cluster's own cadence (the far crowd of the stands: small figures, one picture shared by the stadium's far stands)
            due = NpCrowdDue(frame, (unsigned)mSerial, NpCrowdSizedCluster()) || !sprite->mSeenLast;
        }
        else if (g < 0 || g >= kMaxGroups)
        {
            due = true;   // (a model with more textures than the groups hold: every picture)
        }
        else
        {
            int step = NpCrowdStepOfClass(groupClass[g], at60, sMax);
            if (step < minStep) step = minStep;
            due = groupHasFigures[g] && (groupFresh[g] || NpCrowdDue(frame, (unsigned)(mSerial * 8 + g), step));
        }
        if (inView && hasFigures) ++*inView;
        if (due && hasFigures)
        {
            if (lazyView) sprite->UpdateView(direction, up);
            sprite->ResumeCapture();
            sSamePoseAsLastSprite = lastTexture == sprite->mTextureIndex;
            if (lastTexture != sprite->mTextureIndex)
            {
                EvaluatePose(sprite->mTextureIndex);
                lastTexture = sprite->mTextureIndex;
            }
            Render(sprite->mView, sprite->mTextureIndex);
            sSamePoseAsLastSprite = false;
            ++drawn;
            if (g >= 0 && g < kMaxGroups) groupDrawn[g] = true;
        }
        else
        {
            sprite->SuspendCapture();
        }
        sprite->mSeenLast = hasFigures;
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
    if (groups)
    {
        for (int g = 0; g < kMaxGroups; ++g)
            if (groupDrawn[g]) ++*groups;
    }
    return drawn;
}

void ImpostorCharacter::EnableSprites(bool enable)
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        ImpostorSprite* sprite = entry->entry;
        sprite->mEnabled = enable;
        sprite->UpdateViewport();
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void ImpostorCharacter::RegisterSprites(GLView* parentView)
{
    nlDLListIterator<ImpostorSprite*> it;
    it = mSprites.Begin();
    DLListEntry<ImpostorSprite*>* head = it.m_Head;
    DLListEntry<ImpostorSprite*>* entry = it.m_Curr;
    while (entry != 0)
    {
        parentView->AddChild(entry->entry->mView);
        if (nlDLRingIsEnd(head, entry) || entry == 0)
        {
            entry = 0;
        }
        else
        {
            entry = entry->m_next;
        }
    }
}

void AnimatedImpostorCharacter::EvaluatePose(int texture)
{
    if (NpPoseJobs() && ImpostorModel::IsPosed(mModels[texture]))
    {
        NpPoseJobsCount(6, 1);   // MSC_POSE_JOBS: a job made this picture's pose of the model
        return;
    }
    mModels[texture]->EvaluatePose();
}

void AnimatedImpostorCharacter::SetAnimationTime(int index,
    float phase)
{
    mModels[index]->SetAnimationTime(phase);
}

void AnimatedImpostorCharacter::Render(GLView* target,
    int texture)
{
    // MSC_CROWD_POSEONCE (np/overlap.h): the sprites of a texture are one model seen from several angles, its world matrix does not change, and the loop in
    // UpdateSprites evaluated the pose once for them: the node and skin matrices of the last sprite are this sprite's, nothing else ran in between. (The sprite's own
    // copy of the matrices is still made: PrepareToRender.)
    if (NpPoseJobs() && ImpostorModel::IsPosed(mModels[texture]))
    {
        NpOverlapCrowdPose(1);
        mModels[texture]->RenderSamePose(target);
        return;
    }
    if (sSamePoseAsLastSprite && NpCrowdPoseOnce())
    {
        NpOverlapCrowdPose(1);
        mModels[texture]->RenderSamePose(target);
        return;
    }
    NpOverlapCrowdPose(0);
    mModels[texture]->Render(target, 0);
}

AnimatedImpostorCharacter::AnimatedImpostorCharacter(
    const char* name, ImpostorModel* model, void* animation,
    int budget, int numAngles, int numTextures,
    const ImpostorCharacterParams* params)
    : ImpostorCharacter(name, budget, numAngles, numTextures, params)
{
    mNumModels = numTextures;
    mModels = (ImpostorModel**)nlMalloc(numTextures * 4, 8, false);
    mModels[0] = model;
    for (int i = 1; i < numTextures; ++i)
    {
        mModels[i] = model->Clone(glGetCurrentResourcePool());
    }
    for (int i = 0; i < numTextures; ++i)
    {
        mModels[i]->PlayAnimation((const char*)animation, 0.0f, PM_CYCLIC);
    }
}

AnimatedImpostorCharacter::~AnimatedImpostorCharacter()
{
    for (int i = 1; i < mNumModels; ++i)
    {
        delete mModels[i];
    }
    delete[] mModels;
}

void AnimatedImpostorCharacter::UpdateAnimation(float dt)
{
    const bool bank = CrowdGovBanking();   // MSC_CROWD_GOV: the clock moves when one of the model's sprites is redrawn (GovAdvance)
    for (int i = 0; i < mNumModels; ++i)
    {
        if (bank)
        {
            float p = mModels[i]->mGovPending + dt;
            mModels[i]->mGovPending = p < kGovMaxBank ? p : kGovMaxBank;
            continue;
        }
        mModels[i]->mGovPending = 0.0f;   // (a lag left over from a slowed stretch is dropped: the animations are cycles)
        mModels[i]->UpdateAnimation(dt);
    }
}

void AnimatedImpostorCharacter::PlayAnimation(float blendTime,
    const char* name)
{
    for (int i = 0; i < mNumModels; ++i)
    {
        float modelBlendTime
            = nlRandomf(0.25f * blendTime, blendTime, &nlDefaultSeed);
        mModels[i]->PlayAnimation(name, modelBlendTime, PM_CYCLIC);
    }
}
