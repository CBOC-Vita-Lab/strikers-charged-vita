// Narrow allocation/resource/view boundaries for the real owning methods.
// No ImpostorSprite, ImpostorCharacter, or ImpostorManager methods are replaced.
#include "NL/nlDLListContainer.inl"
#include "Game/TweakValueFloat.h"
#include "Game/TweakRegistry.h"
#include "NL/MemAlloc.h"
#include "NL/gl/glView.h"
#include "NL/gl/glMatrix.h"
#include "NL/platvmath.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int boundary_view_creates, boundary_view_destroys, boundary_child_removes;
static void Unexpected(const char* name) { std::fprintf(stderr, "UNEXPECTED boundary call: %s\n", name); std::abort(); }
MemoryAllocator VirtualAllocator = {}, StandardAllocator = {};
MemoryAllocator* CurrentAllocator = &VirtualAllocator;
MemoryAllocator* AllocatorStack[16] = {&VirtualAllocator};
unsigned int AllocatorStackDepth = 1;
void* nlMalloc(unsigned long size, unsigned int alignment, bool)
{
    void* pointer = 0;
    if (posix_memalign(&pointer, alignment < sizeof(void*) ? sizeof(void*) : alignment, size ? size : 1)) std::abort();
    return pointer;
}
void* nlMalloc(unsigned long size) { return nlMalloc(size, 8, false); }
void nlFree(void* pointer) { std::free(pointer); }
void nlBreak() { Unexpected("nlBreak"); }
void nlZeroMemory(void* pointer, unsigned long size) { std::memset(pointer, 0, size); }
int nlSNPrintf(char* output, unsigned long size, const char* format, ...)
{
    va_list args; va_start(args, format); int result = std::vsnprintf(output, size, format, args); va_end(args); return result;
}

const char* gLastTweakCategory = "fixture";
TweakValueAllocator3* gTweakValueAllocator = 0;
TweakValueAllocator2* gTweakBindingAllocator = 0;
char gTweakFloatBindingFormat[] = "%.3f";
TweakValueBase::TweakValueBase() : mName(0), mCreatedAfterRegistryInit(false), mFormatName(false) {}
TweakValueBase::~TweakValueBase() {}
void* TweakValueBase::ReservedValueQuery() { return 0; }
int IsTweakRegistryInitialized() { return 0; }
void QueueTweakValue(TweakPendingValue* pending, TweakValueBase*, const char*) { std::free(pending); }
void AddTweakValue(TweakEntry*, TweakValueBase*) { Unexpected("AddTweakValue"); }
TweakEntry* GetTweakRoot() { Unexpected("GetTweakRoot"); return 0; }
TweakEntry* FindOrCreateTweakPath(TweakEntry*, const char*, int) { Unexpected("FindOrCreateTweakPath"); return 0; }
bool TweakBindingBase::Bind(const char*, const char*, bool, float, float, float)
{
    static float values[32]; static unsigned index;
    if (index == 32) std::abort();
    BindValueAddress(&values[index++]);
    return false;
}
void glBeginResource(const char*) {}
void glEndResource() {}

class GLXTarget { public: unsigned id; };
static GLXTarget targets[16];
GLRenderPair glCreateTarget(const char*, const GLTargetInfo*)
{
    static unsigned index;
    if (index == 16) std::abort();
    unsigned selected = index++;
    targets[selected].id = selected + 1;
    return GLRenderPair(selected + 1, &targets[selected]);
}
void glDestroyTarget(GLRenderPair* target) { *target = GLRenderPair(); }
GLView::GLView(GLViewInterface* interface, const GLRenderPair& pair, GLViewSortMode)
    : m_CreateSorter(0), m_Sorters(0), m_RenderPair(pair), m_Enabled(true), m_ClearDepth(false),
      m_Unknown32(false), m_ClearColour(false), m_Target(GLViewTarget_None), m_Unknown38(0), m_Unknown3C(0),
      m_Visible(true), m_Name("fixture"), m_Unknown48(0), m_TriangleCount(0), m_Interface(interface), m_Parent(0)
{
    m_Viewport = GLViewViewport(); ++boundary_view_creates;
}
GLView::~GLView()
{
    if (m_Children.m_Head != 0 || m_Parent != 0) Unexpected("view destroyed while registered");
    ++boundary_view_destroys;
}
void GLView::RemoveChild(GLView* child)
{
    bool found = false;
    for (nlListIterator<GLView*> it = m_Children.Begin(); it.IsValid(); it.Next()) if (it.Current() == child) found = true;
    if (!found || child->m_Parent != this) Unexpected("unregistered RemoveChild");
    m_Children.RemoveEntry(child); child->m_Parent = 0; ++boundary_child_removes;
}

// These can be retained through owning vtables, but this lifecycle fixture must never enter rendering/math.
float nlRecipSqrt(float, bool) { Unexpected("nlRecipSqrt"); return 0; }
void glMatrixLookAt(nlMatrix4&, const nlVector3&, const nlVector3&, const nlVector3&) { Unexpected("glMatrixLookAt"); }
void glMatrixPerspective(nlMatrix4&, float, float, float, float) { Unexpected("glMatrixPerspective"); }
nlMatrix4& nlMakeScaleMatrix(nlMatrix4& output, float, float, float) { Unexpected("nlMakeScaleMatrix"); return output; }
nlMatrix4& nlMakeRotationMatrixZ(nlMatrix4& output, float) { Unexpected("nlMakeRotationMatrixZ"); return output; }
nlMatrix4& nlInvertMatrix(nlMatrix4& output, const nlMatrix4&) { Unexpected("nlInvertMatrix"); return output; }
nlMatrix4& nlMultMatrices(nlMatrix4& output, const nlMatrix4&, const nlMatrix4&) { Unexpected("nlMultMatrices"); return output; }

// Render-side switches of the crowd (np/crowdcut.h, crowdgov.h, overlap.h) and the random seam: all off, the path the game takes with no switch set.
// ImpostorModel (the pose accumulator of animated crowds) is not part of the sprite / character / manager lifecycle under test: its two entry points are
// reached only by an animated character, which this fixture never builds.
#include "Game/Render/ImpostorModel.h"
extern "C" {
int NpCrowdGT(void) { return 0; }
void NpCrowdGTCount(int, int) {}
void NpCrowdGovCount(int, unsigned) {}
int NpCrowdPoseOnce(void) { return 0; }
int NpPoseJobs(void) { return 0; }
int gNpSimtProbe = 0;
void NpSimtRandom(const unsigned int*) {}
int gNpRngRedirect = 0;
unsigned int* NpRngSeedFor(unsigned int* seed) { return seed; }
}
void ImpostorModel::UpdateAnimation(float) { Unexpected("ImpostorModel::UpdateAnimation"); }
void ImpostorModel::ClearPosed() {}
