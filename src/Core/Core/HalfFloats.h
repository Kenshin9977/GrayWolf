#pragma once

#include <cstdint>

class HalfFloats
{
private:
    union FloatBits
    {
        float f;
        int32_t si;
        uint32_t ui;
    };

    static int const shift = 13;
    static int const shiftSign = 16;

    static int32_t const infN = 0x7F800000;
    static int32_t const maxN = 0x477FE000;
    static int32_t const minN = 0x38800000;

    static int32_t const infC = infN >> shift;
    static int32_t const nanN = (infC + 1) << shift;
    static int32_t const maxC = maxN >> shift;
    static int32_t const minC = minN >> shift;
    static int32_t const signC = 0x8000; // a half's sign bit

    // Precalculated (1 << 23) / minN
    static int32_t const mulN = 0x52000000;
    // Precalculated minN / (1 << (23 - shift))
    static int32_t const mulC = 0x33800000;

    // Max float subnormal down shifted
    static int32_t const subC = 0x003FF;
    // Min float normal down shifted
    static int32_t const norC = 0x00400;

    static int32_t const maxD = infC - maxC - 1;
    static int32_t const minD = minC - subC - 1;

public:
    static float ToFloat(uint16_t Value);
};