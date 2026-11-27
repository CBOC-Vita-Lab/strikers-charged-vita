// stand-in for NL/nlMath.h for render_neon_test.cpp: the types and the scalar functions render_neon.h needs, the scalar ones copied from platqmath.cpp / platvmath.cpp / math.cpp / nlMath.h
#ifndef RN_STUB_NLMATH_H
#define RN_STUB_NLMATH_H
struct nlQuaternion { float x, y, z, w; };
struct nlVector3 { float x, y, z; };
struct nlMatrix4 { float e2[4][4]; };
float nlRecipSqrt(float x, bool accurate);
void nlMultQuat(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2);
void nlQuatNLerp(nlQuaternion& out, const nlQuaternion& q1, const nlQuaternion& q2, float t);
void nlMultPosVectorMatrix(nlVector3& result, const nlVector3& pos, const nlMatrix4& m);
inline void nlVecLerp(nlVector3& result, const nlVector3& a, const nlVector3& b, float alpha)
{
    float oneMinusAlpha = 1.0f - alpha;
    result.x = oneMinusAlpha * a.x + alpha * b.x;
    result.y = oneMinusAlpha * a.y + alpha * b.y;
    result.z = oneMinusAlpha * a.z + alpha * b.z;
}
#endif
