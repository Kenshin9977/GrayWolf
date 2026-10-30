#pragma once

#include <algorithm>
#include <cstdint>

class VectorMath
{
public:
    static float DegreesToRadians(float Value);
    static float RadiansToDegrees(float Value);

    // Value within [Lower, Upper]; unlike std::clamp, a NaN comes back as Lower, which the
    // exporters' bytes depend on
    template <typename T>
    static T Clamp(const T& Value, const T& Lower, const T& Upper)
    {
        return std::max(Lower, std::min(Value, Upper));
    }

};

// A class that handles math for a 2D Vector (Or UV set)
class Vector2
{
public:
    Vector2();
    Vector2(float XCoord, float YCoord);

    // Coordinate (X or UV U)
    union
    {
        float X;
        float U;
    };
    // Coordinate (Y or UV V)
    union
    {
        float Y;
        float V;
    };

};

class Vector3
{
public:
    Vector3();
    Vector3(float XCoord, float YCoord, float ZCoord);

    union
    {
        float X;
    };
    union
    {
        float Y;
    };
    union
    {
        float Z;
    };

    Vector3 operator+(const Vector3& Value) const;
    Vector3 operator-(const Vector3& Value) const;
    Vector3 operator*(float Value) const;
    Vector3& operator*=(float Value);
    bool operator==(const Vector3& Value) const;
    bool operator!=(const Vector3& Value) const;

    float Length() const;
    Vector3 GetNormalized() const;
};

class Quaternion
{
public:
    // Create a new Quaternion set to identity
    Quaternion();
    Quaternion(float XCoord, float YCoord, float ZCoord, float WCoord);

    union
    {
        float X;
    };
    union
    {
        float Y;
    };
    union
    {
        float Z;
    };
    union
    {
        float W;
    };

    Quaternion operator*(const Quaternion& Value) const;
    Quaternion operator~() const;

    float Length() const;
    void Normalize();

    Vector3 ToEulerAngles() const;

    static Quaternion Identity();
};

/*

    m11, m12, m13, m14 = 0,0 0,1 0,2 0,3
    m21, m22, m23, m24 = 1,0 1,1 1,2 1,3
    m31, m32, m33, m34 = 2,0 2,1 2,2 2,3
    m41, m42, m43, m44 = 3,0 3,1 3,2 3,3

*/

class Matrix
{
private:
    float Data[16];

public:
    Matrix();

    Matrix operator/(float Value) const;

    float& Mat(int X, int Y);
    const float& Mat(int X, int Y) const;
    float Determinant();
    Matrix Inverse();

    static Matrix CreateFromQuaternion(const Quaternion& Value);
    static Vector3 TransformVector(const Vector3& Vector, const Matrix& Value);
};

class VectorPacking
{
public:

    static Quaternion QuatPackingA(const uint64_t PackedData);
    static Quaternion QuatPacking2DA(const uint32_t PackedData);
};