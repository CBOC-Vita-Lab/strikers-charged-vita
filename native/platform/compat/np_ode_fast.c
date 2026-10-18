/* ODE's "fast" kernels (fastdot.c, fastlsolve.c, fastltsolve.c, ldltaddTL): hand-optimised assembly in the
 * retail binary and never decompiled, so not in src/ode. Plain-C versions with the reference semantics
 * (ODE 0.5). dDot is used by dLDLTRemove only; dSolveL1/dSolveL1T by dSolveLDLT; the QuickStep solver the
 * game uses calls none of them, but the TU that defines dLDLTRemove/dSolveLDLT references them. */
#include <alloca.h>
#include <math.h>

typedef float dReal;   /* dSINGLE */

dReal dDot(const dReal *a, const dReal *b, int n)
{
    dReal sum = 0;
    int i;
    for (i = 0; i < n; i++) sum += a[i] * b[i];
    return sum;
}

/* solve L*X=B in place; L is n*n lower triangular with unit diagonal, row stride lskip1 */
void dSolveL1(const dReal *L, dReal *B, int n, int lskip1)
{
    int i, j;
    for (i = 0; i < n; i++) {
        dReal sum = 0;
        for (j = 0; j < i; j++) sum += L[i * lskip1 + j] * B[j];
        B[i] -= sum;
    }
}

/* solve L^T*X=B in place (same L) */
void dSolveL1T(const dReal *L, dReal *B, int n, int lskip1)
{
    int i, j;
    for (i = n - 2; i >= 0; i--) {
        dReal sum = 0;
        for (j = i + 1; j < n; j++) sum += L[j * lskip1 + i] * B[j];
        B[i] -= sum;
    }
}

/* rank-two update of an LDL^T factorisation: A' = A + a*e1^T + e1*a^T  (ODE 0.5, reference form) */
void dLDLTAddTL(dReal *L, dReal *d, const dReal *a, int n, int nskip)
{
    int j, p;
    dReal *W1, *W2, W11, W21, alpha1, alpha2, alphanew, gamma1, gamma2, k1, k2, Wp, ell, dee;
    const dReal sqrt1_2 = 0.7071067811865476f;
    if (n < 2) return;
    W1 = (dReal *)alloca((2 * nskip) * sizeof(dReal));
    W2 = W1 + nskip;
    W1[0] = 0; W2[0] = 0;
    for (j = 1; j < n; j++) W1[j] = W2[j] = a[j] * sqrt1_2;
    W11 = (0.5f * a[0] + 1) * sqrt1_2;
    W21 = (0.5f * a[0] - 1) * sqrt1_2;
    alpha1 = 1.0f; alpha2 = 1.0f;
    dee = d[0];
    alphanew = alpha1 + (W11 * W11) * dee;
    dee /= alphanew;
    gamma1 = W11 * dee;
    dee *= alpha1;
    alpha1 = alphanew;
    alphanew = alpha2 - (W21 * W21) * dee;
    dee /= alphanew;
    gamma2 = W21 * dee;
    alpha2 = alphanew;
    k1 = 1.0f - W21 * gamma1;
    k2 = W21 * gamma1 * W11 - W21;
    for (p = 1; p < n; p++) {
        Wp = W1[p];
        ell = L[p * nskip];
        W1[p] = Wp - W11 * ell;
        W2[p] = k1 * Wp + k2 * ell;
    }
    for (j = 1; j < n; j++) {
        dee = d[j];
        alphanew = alpha1 + (W1[j] * W1[j]) * dee;
        dee /= alphanew;
        gamma1 = W1[j] * dee;
        dee *= alpha1;
        alpha1 = alphanew;
        alphanew = alpha2 - (W2[j] * W2[j]) * dee;
        dee /= alphanew;
        gamma2 = W2[j] * dee;
        dee *= alpha2;
        d[j] = dee;
        alpha2 = alphanew;
        k1 = W1[j]; k2 = W2[j];
        for (p = j + 1; p < n; p++) {
            ell = L[p * nskip + j];
            Wp = W1[p] - k1 * ell;
            ell += gamma1 * Wp;
            W1[p] = Wp;
            Wp = W2[p] - k2 * ell;
            ell -= gamma2 * Wp;
            W2[p] = Wp;
            L[p * nskip + j] = ell;
        }
    }
}
