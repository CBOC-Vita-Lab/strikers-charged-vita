// np_controls.cpp: the MODERN / CLASSIC layouts and the rear touchpad (see np_controls.h). Pure code: the unit test
// platform/sdk/tests/test_controls.cpp compiles this file on its own.
#include "np_controls.h"

#include <stdlib.h>
#include <string.h>

namespace NpControls {

const char* LayoutName(Layout layout) { return layout == kModern ? "modern" : "classic"; }

uint16_t ClassicGc(uint32_t r) {
    uint16_t b = 0;
    if (r & kCross) b |= kGcA;
    if (r & kSquare) b |= kGcB;
    if (r & kCircle) b |= kGcX;
    if (r & kTriangle) b |= kGcY;
    if (r & kL) b |= kGcL;
    if (r & kR) b |= kGcR;
    if (r & kSelect) b |= kGcZ;
    if (r & kStart) b |= kGcStart;
    if (r & kUp) b |= kGcUp;
    if (r & kDown) b |= kGcDown;
    if (r & kLeft) b |= kGcLeft;
    if (r & kRight) b |= kGcRight;
    return b;
}

uint32_t WiiHold(Layout layout, uint32_t vita, uint32_t hold, bool bareRemote) {
    if (layout != kModern) return hold;
    const uint32_t circleBit = bareRemote ? kW2 : kWC;
    hold &= ~(kWB | circleBit);
    if (vita & kCircle) hold |= kWB;
    if (vita & kSquare) hold |= circleBit;
    return hold;
}

// ---- rear touchpad -------------------------------------------------------------------------------------------------------
RearPad::Zone RearPad::ZoneOf(float x, float y) {
    if (!(x >= kEdgeX && x <= 960.0f - kEdgeX && y >= kEdgeY && y <= 544.0f - kEdgeY)) return kNone;   // also NaN
    if (x < kMidLo) return kLeftZone;
    if (x > kMidHi) return kRightZone;
    return kNone;
}

void RearPad::Reset() { n_ = 0; }

unsigned RearPad::Update(const RearPoint* points, int count, uint32_t nowMs) {
    if (count < 0) count = 0;
    if (count > kMaxPoints) count = kMaxPoints;
    const bool grip = count >= 3;
    unsigned events = 0;
    for (int i = 0; i < n_; ++i) c_[i].seen = false;
    for (int p = 0; p < count; ++p) {
        const RearPoint& pt = points[p];
        Contact* c = 0;
        for (int i = 0; i < n_; ++i)
            if (c_[i].id == pt.id) c = &c_[i];
        if (!c) {
            if (n_ >= kMaxPoints) continue;
            c = &c_[n_++];
            c->id = pt.id;
            c->zone = ZoneOf(pt.x, pt.y);
            c->valid = c->zone != kNone && !grip;
            c->fired = false;
            c->downMs = nowMs;
            c->x0 = pt.x;
            c->y0 = pt.y;
            c->maxMove = 0.0f;
        }
        c->seen = true;
        if (grip) c->valid = false;   // a grip spoils every finger on the back
        const float dx = pt.x - c->x0, dy = pt.y - c->y0;
        const float move2 = dx * dx + dy * dy;
        if (!(move2 == move2)) c->valid = false;   // NaN
        else if (move2 > c->maxMove) c->maxMove = move2;
        if (c->valid && !c->fired && c->zone == kLeftZone && nowMs - c->downMs <= kSwipeMs && c->maxMove >= kSwipePx * kSwipePx) {
            c->fired = true;   // the left swipe counts at once (the shakedown)
            events |= 1u;
        }
    }
    // fingers that lifted: a short, still contact is a tap
    for (int i = 0; i < n_;) {
        if (c_[i].seen) {
            ++i;
            continue;
        }
        const Contact& c = c_[i];
        if (c.valid && !c.fired && nowMs - c.downMs <= kTapMs && c.maxMove < kTapMovePx * kTapMovePx)
            events |= c.zone == kLeftZone ? 1u : 2u;
        c_[i] = c_[--n_];
    }
    return events;
}

// ---- MODERN ------------------------------------------------------------------------------------------------------------------
void Mapper::Reset() { *this = Mapper(); }

uint16_t Mapper::Map(uint32_t v, int* rearRight, int* rearLeft, const Context& ctx) {
    const uint32_t pressed = v & ~prev_;
    uint16_t out = 0;
    if (v & kCross) out |= kGcA;
    if (v & kCircle) out |= kGcB;
    if (v & kSelect) out |= kGcZ;
    if (v & kStart) out |= kGcStart;
    if (v & kUp) out |= kGcUp;
    if (v & kDown) out |= kGcDown;
    if (v & kLeft) out |= kGcLeft;
    if (v & kRight) out |= kGcRight;
    if (v & kR) out |= kGcL;   // the "sprint": Charged's turbo / modifier
    // the context buttons: what they are is decided when they go down and kept until they come up
    if (pressed & kSquare) squareBits_ = (uint16_t)(ctx.shakedown ? kGcZ : ctx.possession ? (kGcL | kGcA) : kGcY);
    if (!(v & kSquare)) squareBits_ = 0;
    if (pressed & kTriangle) triangleBits_ = (uint16_t)(ctx.shakedown ? kGcZ : kGcX);
    if (!(v & kTriangle)) triangleBits_ = 0;
    if (pressed & kL) lBits_ = (uint16_t)(ctx.possession ? kGcL : kGcA);
    if (!(v & kL)) lBits_ = 0;
    out |= squareBits_ | triangleBits_ | lBits_;
    // rear taps: a press of two captures, then at least one capture up, so that every tap is a new press for the engine
    if (rightPulse_ == 0 && rightGap_ == 0 && rearRight && *rearRight > 0) {
        --*rearRight;
        if (!ctx.possession) rightPulse_ = 2;   // switch player only: with the ball a tap is dropped (it would pass)
    }
    if (rightPulse_ > 0) {
        out |= kGcA;
        if (--rightPulse_ == 0) rightGap_ = 1;
    } else if (rightGap_ > 0) {
        --rightGap_;
    }
    if (leftPulse_ == 0 && leftGap_ == 0 && rearLeft && *rearLeft > 0) {
        --*rearLeft;
        leftPulse_ = 2;
    }
    if (leftPulse_ > 0) {
        out |= kGcZ;
        if (--leftPulse_ == 0) leftGap_ = 1;
    } else if (leftGap_ > 0) {
        --leftGap_;
    }
    prev_ = v;
    return out;
}

}  // namespace NpControls
