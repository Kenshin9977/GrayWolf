#pragma once

// The vector and polygon helpers the Radiant writer and its texturing share

#include <array>
#include <cmath>
#include <vector>

namespace Maps::RadiantGeometry
{
    using V3 = std::array<double, 3>;

    inline V3 Sub(const V3& A, const V3& B) { return { A[0] - B[0], A[1] - B[1], A[2] - B[2] }; }
    inline V3 Add(const V3& A, const V3& B) { return { A[0] + B[0], A[1] + B[1], A[2] + B[2] }; }
    inline V3 Mul(const V3& A, double S) { return { A[0] * S, A[1] * S, A[2] * S }; }
    inline double Dot(const V3& A, const V3& B) { return A[0] * B[0] + A[1] * B[1] + A[2] * B[2]; }
    inline V3 Cross(const V3& A, const V3& B)
    {
        return { A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0] };
    }
    inline double Length(const V3& A) { return std::sqrt(Dot(A, A)); }

    // The part of a convex polygon behind the plane (N . p <= D), a point within Eps on it
    inline std::vector<V3> Clip(const std::vector<V3>& W, const V3& N, double D, double Eps)
    {
        std::vector<V3> Out;
        const size_t Count = W.size();
        for (size_t K = 0; K < Count; K++)
        {
            const V3& A = W[K];
            const V3& B = W[(K + 1) % Count];
            const double Da = Dot(N, A) - D, Db = Dot(N, B) - D;
            if (Da <= Eps) Out.push_back(A);
            if ((Da > Eps && Db < -Eps) || (Da < -Eps && Db > Eps))
                Out.push_back(Add(A, Mul(Sub(B, A), Da / (Da - Db))));
        }
        return Out;
    }

    // a planar polygon's area
    inline double Area(const std::vector<V3>& W)
    {
        V3 Sum = { 0, 0, 0 };
        for (size_t K = 1; K + 1 < W.size(); K++) Sum = Add(Sum, Cross(Sub(W[K], W[0]), Sub(W[K + 1], W[0])));
        return Length(Sum) / 2;
    }
}
