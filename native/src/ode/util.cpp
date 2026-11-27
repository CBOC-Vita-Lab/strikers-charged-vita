/*************************************************************************
 * Open Dynamics Engine, Copyright (C) 2001-2004 Russell L. Smith.
 * Distributed under the BSD-style license in LICENSES/ODE-BSD.txt.
 *************************************************************************/

#include "NL/nlMath.h"
#include <ode/collision.h>
#include <ode/compatibility.h>
#include <ode/odemath.h>
#include <ode/rotation.h>

#include "joint.h"
#include "objects.h"
#include "util.h"
#include "np/gsplit.h"   // MSC_GSPLIT phase markers

#define ALLOCA   dALLOCA16
#define dFabs(x) ((float)__fabs(float(x)))

static inline unsigned short radiansToAngle(dReal angle)
{
    return (unsigned short)(int)(10430.378f * angle);
}

/**
 * Offset/Address/Size: 0x0 | 0x80358240 | size: 0xDC
 */
void dInternalHandleAutoDisabling(dxWorld* world, dReal stepsize)
{
    dxBody* bb;
    for (bb = world->firstbody; bb; bb = (dxBody*)bb->next)
    {
        // nothing to do unless this body is currently enabled and has
        // the auto-disable flag set
        if ((bb->flags & (dxBodyAutoDisable | dxBodyDisabled)) != dxBodyAutoDisable)
            continue;

        // see if the body is idle
        int idle = 1; // initial assumption
        dReal lspeed2 = dDOT(bb->lvel, bb->lvel);
        if (lspeed2 > bb->adis.linear_threshold)
        {
            idle = 0; // moving fast - not idle
        }
        else
        {
            dReal aspeed = dDOT(bb->avel, bb->avel);
            if (aspeed > bb->adis.angular_threshold)
            {
                idle = 0; // turning fast - not idle
            }
        }

        // if it's idle, accumulate steps and time.
        // these counters won't overflow because this code doesn't run for disabled
        // bodies.
        if (idle)
        {
            bb->adis_stepsleft--;
            bb->adis_timeleft -= stepsize;
        }
        else
        {
            bb->adis_stepsleft = bb->adis.idle_steps;
            bb->adis_timeleft = bb->adis.idle_time;
        }

        // disable the body if it's idle for a long enough time
        if (bb->adis_stepsleft < 0 && bb->adis_timeleft < 0)
        {
            bb->flags |= dxBodyDisabled;
        }
    }
}

// given a body b, apply its linear and angular rotation over the time
// interval h, thereby adjusting its position and orientation.

/**
 * Offset/Address/Size: 0xDC | 0x8035831C | size: 0x3B4
 */
// MSC_SIM26_ISLANDS: dxStepBody without the dGeomMoved calls at its end; returns whether they were due (the caller makes them, in the same order)
int dxStepBodyNoMove(dxBody* b, dReal h);
void dxStepBody(dxBody* b, dReal h)
{
    if (dxStepBodyNoMove(b, h))
    {
        for (dxGeom* geom = b->geom; geom; geom = dGeomGetBodyNext(geom))
            dGeomMoved(geom);
    }
}

int dxStepBodyNoMove(dxBody* b, dReal h)
{
    int j;
    unsigned char changed = 0;

    // handle linear velocity
    if (!(b->flags & dxBodyLinearUpdateDisabled))
    {
        changed = 1;
        b->pos[0] += h * b->lvel[0];
        b->pos[1] += h * b->lvel[1];
        b->pos[2] += h * b->lvel[2];
    }

    if (!(b->flags & dxBodyAngularUpdateDisabled))
    {
        if (b->flags & dxBodyFlagFiniteRotation)
        {
            dVector3 irv;  // infitesimal rotation vector
            dQuaternion q; // quaternion for finite rotation

            if (b->flags & dxBodyFlagFiniteRotationAxis)
            {
                // split the angular velocity vector into a component along the finite
                // rotation axis, and a component orthogonal to it.
                dVector3 frv; // finite rotation vector
                dReal k = dDOT(b->finite_rot_axis, b->avel);
                frv[0] = b->finite_rot_axis[0] * k;
                frv[1] = b->finite_rot_axis[1] * k;
                frv[2] = b->finite_rot_axis[2] * k;
                irv[0] = b->avel[0] - frv[0];
                irv[1] = b->avel[1] - frv[1];
                irv[2] = b->avel[2] - frv[2];

                // make a rotation quaternion q that corresponds to frv * h.
                // compare this with the full-finite-rotation case below.
                h *= REAL(0.5);
                dReal theta = k * h;
                q[0] = nlSin((unsigned short)(radiansToAngle(theta) + 0x4000));
                dReal s;
                if (dFabs(theta) < 1.0e-4f)
                {
                    s = REAL(1.0) - theta * theta * REAL(0.166666666666666666667);
                }
                else
                {
                    s = nlSin(radiansToAngle(theta)) / theta;
                }
                s = h * s;
                q[1] = frv[0] * s;
                q[2] = frv[1] * s;
                q[3] = frv[2] * s;
            }
            else
            {
                // make a rotation quaternion q that corresponds to w * h
                dReal wlen = dSqrt(b->avel[0] * b->avel[0] + b->avel[1] * b->avel[1] + b->avel[2] * b->avel[2]);
                h *= REAL(0.5);
                dReal theta = wlen * h;
                q[0] = nlSin((unsigned short)(radiansToAngle(theta) + 0x4000));
                dReal s;
                if (dFabs(theta) < 1.0e-4f)
                {
                    s = REAL(1.0) - theta * theta * REAL(0.166666666666666666667);
                }
                else
                {
                    s = nlSin(radiansToAngle(theta)) / theta;
                }
                s = h * s;
                q[1] = b->avel[0] * s;
                q[2] = b->avel[1] * s;
                q[3] = b->avel[2] * s;
            }

            // do the finite rotation
            dQuaternion q2;
            dQMultiply0(q2, q, b->q);
            for (j = 0; j < 4; j++)
                b->q[j] = q2[j];

            // do the infitesimal rotation if required
            if (b->flags & dxBodyFlagFiniteRotationAxis)
            {
                dReal dq[4];
                dWtoDQ(irv, b->q, dq);
                for (j = 0; j < 4; j++)
                    b->q[j] += h * dq[j];
            }
        }
        else
        {
            // the normal way - do an infitesimal rotation
            dReal dq[4];
            dWtoDQ(b->avel, b->q, dq);
            for (j = 0; j < 4; j++)
                b->q[j] += h * dq[j];
        }

        // normalize the quaternion and convert it to a rotation matrix
        dNormalize4(b->q);
        dQtoR(b->q, b->R);
        changed = 1;
    }

    // notify all attached geoms that this body has moved (dxStepBody, or the island step of MSC_SIM26_ISLANDS after its join)
    return changed;
}

// this groups all joints and bodies in a world into islands. all objects
// in an island are reachable by going through connected bodies and joints.
// each island can be simulated separately.
// note that joints that are not attached to anything will not be included
// in any island, an so they do not affect the simulation.
//
// this function starts new island from unvisited bodies. however, it will
// never start a new islands from a disabled body. thus islands of disabled
// bodies will not be included in the simulation. disabled bodies are
// re-enabled if they are found to be part of an active island.

/**
 * Offset/Address/Size: 0x490 | 0x803586D0 | size: 0x438
 */
// ---- MSC_SIM26_ISLANDS (wave 12, sim26): the islands of a step on the job lanes ----
// Each island (the bodies and joints reachable from one another; here mostly one character body with its character joint, or the ball) is
// stepped by dxQuickStepper from its own bodies and joints only, and no body or joint is in two islands. 1: the island search runs first, as
// before but without stepping (it reads only tags and the joint graph; the stepper's tag writes are undone right after it anyway); then the
// islands are stepped on the lanes (NpParallelFor, the caller is lane 0), each with the stepper's own code on its own data; then, on this thread,
// in island order, the dGeomMoved calls each body's integration would have made (they move geoms to the front of the shared space list: the order
// matters, so they are made after the join, in the old order) and the old tag / enabled-flag resets. =2 verify: the islands are stepped on the
// lanes, the results (every body's position, rotation, quaternion and velocities, and the joint feedback) hashed, restored, stepped again
// serially and compared ([sim26isl] wrong must be 0). No lanes, a stepper other than dxQuickStepper, or more islands than kNpIslMax: the old loop.
// MSC_SIM26_ISLANDS_BREAK=1: proof mutant (the first island's first body skips its geom notification).
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "quickstep.h"
extern "C" {
typedef void (*NpRangeFn)(void* ctx, size_t begin, size_t end, uint32_t lane);
unsigned NpJobsLanes(void) __attribute__((weak));
unsigned NpParallelFor(size_t count, size_t minPerLane, NpRangeFn fn, void* ctx) __attribute__((weak));
}
void NpSim26QuickStepperDeferred(dxWorld* world, dxBody* const* body, int nb, dxJoint* const* _joint, int nj, dReal stepsize, unsigned char* changedOut);

namespace
{
enum { kNpIslMax = 64 };
struct NpIslJob
{
    dxWorld* world;
    dReal stepsize;
    dxBody** bodies;
    dxJoint** joints;
    int bOff[kNpIslMax + 1];
    int jOff[kNpIslMax + 1];
    unsigned char* changed;
};
int sNpIslMode = -1, sNpIslBreak = 0;
unsigned long long sNpIslSteps, sNpIslParallel, sNpIslIslands, sNpIslSerial, sNpIslVerified, sNpIslWrong;

void NpIslRange(void* opaque, size_t begin, size_t end, uint32_t)
{
    NpIslJob& job = *static_cast<NpIslJob*>(opaque);
    for (size_t k = begin; k < end; k++)
        NpSim26QuickStepperDeferred(job.world, job.bodies + job.bOff[k], job.bOff[k + 1] - job.bOff[k], job.joints + job.jOff[k], job.jOff[k + 1] - job.jOff[k], job.stepsize,
            job.changed + job.bOff[k]);
}

// verify: what a step of the islands writes into the bodies and the joint feedback
uint64_t NpIslHash(dxBody** bodies, int nb, dxJoint** joints, int nj)
{
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < nb; i++)
    {
        const unsigned char* p = (const unsigned char*)bodies[i];
        for (size_t k = 0; k < sizeof(dxBody); k++)
            h = (h ^ p[k]) * 1099511628211ull;
    }
    for (int i = 0; i < nj; i++)
        if (joints[i]->feedback)
        {
            const unsigned char* p = (const unsigned char*)joints[i]->feedback;
            for (size_t k = 0; k < sizeof(dJointFeedback); k++)
                h = (h ^ p[k]) * 1099511628211ull;
        }
    return h;
}

int NpIslMode()
{
    if (sNpIslMode < 0)
    {
        const char* e = getenv("MSC_SIM26_ISLANDS");
        sNpIslMode = (e && (e[0] == '1' || e[0] == '2')) ? e[0] - '0' : 0;
        const char* k = getenv("MSC_SIM26_ISLANDS_BREAK");
        sNpIslBreak = (k && k[0] == '1') ? 1 : 0;
    }
    return sNpIslMode;
}

// 1 = done here; 0 = take the old loop
int NpIslProcess(dxWorld* world, dReal stepsize)
{
    if (NpParallelFor == 0 || NpJobsLanes == 0 || NpJobsLanes() < 2)
        return 0;
    dxBody *b, *bb;
    dxJoint* j;
    dxBody** body = (dxBody**)ALLOCA(world->nb * sizeof(dxBody*));
    dxJoint** joint = (dxJoint**)ALLOCA((world->nj > 0 ? world->nj : 1) * sizeof(dxJoint*));
    unsigned char* changed = (unsigned char*)ALLOCA(world->nb > 0 ? world->nb : 1);
    NpIslJob job;
    job.world = world;
    job.stepsize = stepsize;
    job.bodies = body;
    job.joints = joint;
    job.changed = changed;
    int nIsl = 0, bcount = 0, jcount = 0;

    // the search of the old loop, the stepper call replaced by noting the island (the stepper's tag writes, undone right after it, never reached it)
    for (b = world->firstbody; b; b = (dxBody*)b->next)
        b->tag = 0;
    for (j = world->firstjoint; j; j = (dxJoint*)j->next)
        j->tag = 0;
    int stackalloc = (world->nj < world->nb) ? world->nj : world->nb;
    dxBody** stack = (dxBody**)ALLOCA((stackalloc > 0 ? stackalloc : 1) * sizeof(dxBody*));
    for (bb = world->firstbody; bb; bb = (dxBody*)bb->next)
    {
        if (bb->tag || (bb->flags & dxBodyDisabled))
            continue;
        if (nIsl == kNpIslMax)
            return -1;   // too many islands: the caller must redo the step the old way from untouched state (nothing was stepped yet)
        bb->tag = 1;
        int stacksize = 0;
        b = bb;
        job.bOff[nIsl] = bcount;
        job.jOff[nIsl] = jcount;
        body[bcount++] = bb;
        goto quickstart;
        while (stacksize > 0)
        {
            b = stack[--stacksize];
            body[bcount++] = b;
        quickstart:
            for (dxJointNode* n = b->firstjoint; n; n = n->next)
            {
                if (!n->joint->tag)
                {
                    n->joint->tag = 1;
                    joint[jcount++] = n->joint;
                    if (n->body && !n->body->tag)
                    {
                        n->body->tag = 1;
                        stack[stacksize++] = n->body;
                    }
                }
            }
        }
        nIsl++;
    }
    job.bOff[nIsl] = bcount;
    job.jOff[nIsl] = jcount;

    if (sNpIslMode == 2)
    {
        // verify: save every island body and feedback, step on the lanes, hash, restore, step serially (same deferred stepper), hash, compare
        dxBody* saved = (dxBody*)malloc(sizeof(dxBody) * (bcount > 0 ? bcount : 1));
        dJointFeedback* savedFb = (dJointFeedback*)malloc(sizeof(dJointFeedback) * (jcount > 0 ? jcount : 1));
        for (int i = 0; i < bcount; i++)
            memcpy(&saved[i], body[i], sizeof(dxBody));
        for (int i = 0; i < jcount; i++)
            if (joint[i]->feedback)
                memcpy(&savedFb[i], joint[i]->feedback, sizeof(dJointFeedback));
        NpParallelFor((size_t)nIsl, 1, NpIslRange, &job);
        const uint64_t h1 = NpIslHash(body, bcount, joint, jcount);
        for (int i = 0; i < bcount; i++)
            memcpy(body[i], &saved[i], sizeof(dxBody));
        for (int i = 0; i < jcount; i++)
            if (joint[i]->feedback)
                memcpy(joint[i]->feedback, &savedFb[i], sizeof(dJointFeedback));
        NpIslRange(&job, 0, (size_t)nIsl, 0);
        const uint64_t h2 = NpIslHash(body, bcount, joint, jcount);
        sNpIslVerified++;
        if (h1 != h2)
            sNpIslWrong++;
        free(saved);
        free(savedFb);
    }
    else
        NpParallelFor((size_t)nIsl, 1, NpIslRange, &job);

    // after the join, in island order: each island's geom notifications (dxStepBody's, body by body), then the old loop's resets
    for (int k = 0; k < nIsl; k++)
    {
        for (int i = job.bOff[k]; i < job.bOff[k + 1]; i++)
        {
            if (sNpIslBreak == 1 && k == 0 && i == job.bOff[k])
                continue;   // proof mutant
            if (changed[i])
            {
                for (dxGeom* geom = body[i]->geom; geom; geom = dGeomGetBodyNext(geom))
                    dGeomMoved(geom);
            }
        }
        for (int i = job.bOff[k]; i < job.bOff[k + 1]; i++)
        {
            body[i]->tag = 1;
            body[i]->flags &= ~dxBodyDisabled;
        }
        for (int i = job.jOff[k]; i < job.jOff[k + 1]; i++)
            joint[i]->tag = 1;
    }
    sNpIslParallel++;
    sNpIslIslands += (unsigned)nIsl;
    return 1;
}
} // namespace

void dxProcessIslands(dxWorld* world, dReal stepsize, dstepper_fn_t stepper)
{
    dxBody *b, *bb, **body;
    dxJoint *j, **joint;

    // nothing to do if no bodies
    if (world->nb <= 0)
        return;

    NP_GS(NPGS_PH_ISLANDS);   // MSC_GSPLIT: island search; the stepper marks its own parts
    // handle auto-disabling of bodies
    dInternalHandleAutoDisabling(world, stepsize);

    // MSC_SIM26_ISLANDS: the islands on the job lanes (above); every other case falls through to the old loop
    if (NpIslMode() != 0 && stepper == dxQuickStepper)
    {
        if (++sNpIslSteps % 600 == 0)
        {
            printf("[sim26isl] mode=%d break=%d steps=%llu on the lanes=%llu islands=%llu old loop=%llu verify=%llu wrong=%llu\n", sNpIslMode, sNpIslBreak, sNpIslSteps, sNpIslParallel,
                sNpIslIslands, sNpIslSerial, sNpIslVerified, sNpIslWrong);
            fflush(stdout);
        }
        if (NpIslProcess(world, stepsize) == 1)
            return;
        sNpIslSerial++;
    }

    // make arrays for body and joint lists (for a single island) to go into
    body = (dxBody**)ALLOCA(world->nb * sizeof(dxBody*));
    joint = (dxJoint**)ALLOCA(world->nj * sizeof(dxJoint*));
    int bcount = 0; // number of bodies in `body'
    int jcount = 0; // number of joints in `joint'

    // set all body/joint tags to 0
    for (b = world->firstbody; b; b = (dxBody*)b->next)
        b->tag = 0;
    for (j = world->firstjoint; j; j = (dxJoint*)j->next)
        j->tag = 0;

    // allocate a stack of unvisited bodies in the island. the maximum size of
    // the stack can be the lesser of the number of bodies or joints, because
    // new bodies are only ever added to the stack by going through untagged
    // joints. all the bodies in the stack must be tagged!
    int stackalloc = (world->nj < world->nb) ? world->nj : world->nb;
    dxBody** stack = (dxBody**)ALLOCA(stackalloc * sizeof(dxBody*));

    for (bb = world->firstbody; bb; bb = (dxBody*)bb->next)
    {
        // get bb = the next enabled, untagged body, and tag it
        if (bb->tag || (bb->flags & dxBodyDisabled))
            continue;
        bb->tag = 1;

        // tag all bodies and joints starting from bb.
        int stacksize = 0;
        b = bb;
        body[0] = bb;
        bcount = 1;
        jcount = 0;
        goto quickstart;
        while (stacksize > 0)
        {
            b = stack[--stacksize]; // pop body off stack
            body[bcount++] = b;     // put body on body list
        quickstart:

            // traverse and tag all body's joints, add untagged connected bodies
            // to stack
            for (dxJointNode* n = b->firstjoint; n; n = n->next)
            {
                if (!n->joint->tag)
                {
                    n->joint->tag = 1;
                    joint[jcount++] = n->joint;
                    if (n->body && !n->body->tag)
                    {
                        n->body->tag = 1;
                        stack[stacksize++] = n->body;
                    }
                }
            }
            dIASSERT(stacksize <= world->nb);
            dIASSERT(stacksize <= world->nj);
        }

        // now do something with body and joint lists
        stepper(world, body, bcount, joint, jcount, stepsize);

        // what we've just done may have altered the body/joint tag values.
        // we must make sure that these tags are nonzero.
        // also make sure all bodies are in the enabled state.
        int i;
        for (i = 0; i < bcount; i++)
        {
            body[i]->tag = 1;
            body[i]->flags &= ~dxBodyDisabled;
        }
        for (i = 0; i < jcount; i++)
            joint[i]->tag = 1;
    }

    // if debugging, check that all objects (except for disabled bodies,
    // unconnected joints, and joints that are connected to disabled bodies)
    // were tagged.
#ifndef dNODEBUG
    for (b = world->firstbody; b; b = (dxBody*)b->next)
    {
        if (b->flags & dxBodyDisabled)
        {
            if (b->tag)
                dDebug(0, "disabled body tagged");
        }
        else
        {
            if (!b->tag)
                dDebug(0, "enabled body not tagged");
        }
    }
    for (j = world->firstjoint; j; j = (dxJoint*)j->next)
    {
        if ((j->node[0].body && (j->node[0].body->flags & dxBodyDisabled) == 0) || (j->node[1].body && (j->node[1].body->flags & dxBodyDisabled) == 0))
        {
            if (!j->tag)
                dDebug(0, "attached enabled joint not tagged");
        }
        else
        {
            if (j->tag)
                dDebug(0, "unattached or disabled joint tagged");
        }
    }
#endif
}
