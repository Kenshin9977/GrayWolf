#include "stdafx.h"

#include "VectorMath.h"

#include <numbers>

namespace
{
    constexpr double EpsilonValue = 4.37114e-05;
}

float VectorMath::DegreesToRadians(float Value)
{
    return (float)((Value * std::numbers::pi) / 180.0f);
}

float VectorMath::RadiansToDegrees(float Value)
{
    return (float)(((Value * 180.0f) / std::numbers::pi));
}

Vector2::Vector2()
{
    X = 0;
    Y = 0;
}

Vector2::Vector2(float XCoord, float YCoord)
{
    X = XCoord;
    Y = YCoord;
}

Vector3::Vector3()
{
    X = 0;
    Y = 0;
    Z = 0;
}

Vector3::Vector3(float XCoord, float YCoord, float ZCoord)
{
    X = XCoord;
    Y = YCoord;
    Z = ZCoord;
}

Vector3 Vector3::operator+(const Vector3& Value) const
{
    return Vector3(X + Value.X, Y + Value.Y, Z + Value.Z);
}

Vector3 Vector3::operator-(const Vector3& Value) const
{
    return Vector3(X - Value.X, Y - Value.Y, Z - Value.Z);
}

Vector3 Vector3::operator*(float Value) const
{
    return Vector3(X * Value, Y * Value, Z * Value);
}

Vector3& Vector3::operator*=(float Value)
{
    X *= Value;
    Y *= Value;
    Z *= Value;
    return *this;
}

bool Vector3::operator==(const Vector3& Value) const
{
    return (std::fabs(X - Value.X) < EpsilonValue) && (std::fabs(Y - Value.Y) < EpsilonValue) && (std::fabs(Z - Value.Z) < EpsilonValue);
}

bool Vector3::operator!=(const Vector3& Value) const
{
    return !(*this == Value);
}

float Vector3::Length() const
{
    return (float)std::sqrt(X * X + Y * Y + Z * Z);
}

Vector3 Vector3::GetNormalized() const
{
    float Len = Length();

    // TODO: ADD TO OTHER VECTOR TYPES

    return Vector3(X / Len, Y / Len, Z / Len);
}

Matrix::Matrix()
{
    for (int i = 0; i < 16; i++) { Data[i] = (i % 5) ? 0.0f : 1.0f; }
}

Matrix Matrix::operator/(float Value) const
{
    Matrix Result;
    for (int i = 0; i < 16; i++) { Result.Data[i] = Data[i] / Value; }
    return Result;
}

float& Matrix::Mat(int X, int Y)
{
    return Data[X * 4 + Y];
}

const float& Matrix::Mat(int X, int Y) const
{
    return Data[X * 4 + Y];
}

float Matrix::Determinant()
{
    return Mat(3, 0) * Mat(2, 1) * Mat(1, 2) * Mat(0, 3) - Mat(2, 0) * Mat(3, 1) * Mat(1, 2) * Mat(0, 3)
        - Mat(3, 0) * Mat(1, 1) * Mat(2, 2) * Mat(0, 3) + Mat(1, 0) * Mat(3, 1) * Mat(2, 2) * Mat(0, 3)
        + Mat(2, 0) * Mat(1, 1) * Mat(3, 2) * Mat(0, 3) - Mat(1, 0) * Mat(2, 1) * Mat(3, 2) * Mat(0, 3)
        - Mat(3, 0) * Mat(2, 1) * Mat(0, 2) * Mat(1, 3) + Mat(2, 0) * Mat(3, 1) * Mat(0, 2) * Mat(1, 3)
        + Mat(3, 0) * Mat(0, 1) * Mat(2, 2) * Mat(1, 3) - Mat(0, 0) * Mat(3, 1) * Mat(2, 2) * Mat(1, 3)
        - Mat(2, 0) * Mat(0, 1) * Mat(3, 2) * Mat(1, 3) + Mat(0, 0) * Mat(2, 1) * Mat(3, 2) * Mat(1, 3)
        + Mat(3, 0) * Mat(1, 1) * Mat(0, 2) * Mat(2, 3) - Mat(1, 0) * Mat(3, 1) * Mat(0, 2) * Mat(2, 3)
        - Mat(3, 0) * Mat(0, 1) * Mat(1, 2) * Mat(2, 3) + Mat(0, 0) * Mat(3, 1) * Mat(1, 2) * Mat(2, 3)
        + Mat(1, 0) * Mat(0, 1) * Mat(3, 2) * Mat(2, 3) - Mat(0, 0) * Mat(1, 1) * Mat(3, 2) * Mat(2, 3)
        - Mat(2, 0) * Mat(1, 1) * Mat(0, 2) * Mat(3, 3) + Mat(1, 0) * Mat(2, 1) * Mat(0, 2) * Mat(3, 3)
        + Mat(2, 0) * Mat(0, 1) * Mat(1, 2) * Mat(3, 3) - Mat(0, 0) * Mat(2, 1) * Mat(1, 2) * Mat(3, 3)
        - Mat(1, 0) * Mat(0, 1) * Mat(2, 2) * Mat(3, 3) + Mat(0, 0) * Mat(1, 1) * Mat(2, 2) * Mat(3, 3);
}

Matrix Matrix::Inverse()
{
    Matrix Result;

    Result.Mat(0, 0) = + Mat(2, 1) * Mat(3, 2) * Mat(1, 3) - Mat(3, 1) * Mat(2, 2) * Mat(1, 3) + Mat(3, 1) * Mat(1, 2) * Mat(2, 3)
        - Mat(1, 1) * Mat(3, 2) * Mat(2, 3) - Mat(2, 1) * Mat(1, 2) * Mat(3, 3) + Mat(1, 1) * Mat(2, 2) * Mat(3, 3);

    Result.Mat(1, 0) = + Mat(3, 0) * Mat(2, 2) * Mat(1, 3) - Mat(2, 0) * Mat(3, 2) * Mat(1, 3) - Mat(3, 0) * Mat(1, 2) * Mat(2, 3)
        + Mat(1, 0) * Mat(3, 2) * Mat(2, 3) + Mat(2, 0) * Mat(1, 2) * Mat(3, 3) - Mat(1, 0) * Mat(2, 2) * Mat(3, 3);

    Result.Mat(2, 0) = + Mat(2, 0) * Mat(3, 1) * Mat(1, 3) - Mat(3, 0) * Mat(2, 1) * Mat(1, 3) + Mat(3, 0) * Mat(1, 1) * Mat(2, 3)
        - Mat(1, 0) * Mat(3, 1) * Mat(2, 3) - Mat(2, 0) * Mat(1, 1) * Mat(3, 3) + Mat(1, 0) * Mat(2, 1) * Mat(3, 3);

    Result.Mat(3, 0) = + Mat(3, 0) * Mat(2, 1) * Mat(1, 2) - Mat(2, 0) * Mat(3, 1) * Mat(1, 2) - Mat(3, 0) * Mat(1, 1) * Mat(2, 2)
        + Mat(1, 0) * Mat(3, 1) * Mat(2, 2) + Mat(2, 0) * Mat(1, 1) * Mat(3, 2) - Mat(1, 0) * Mat(2, 1) * Mat(3, 2);

    Result.Mat(0, 1) = + Mat(3, 1) * Mat(2, 2) * Mat(0, 3) - Mat(2, 1) * Mat(3, 2) * Mat(0, 3) - Mat(3, 1) * Mat(0, 2) * Mat(2, 3)
        + Mat(0, 1) * Mat(3, 2) * Mat(2, 3) + Mat(2, 1) * Mat(0, 2) * Mat(3, 3) - Mat(0, 1) * Mat(2, 2) * Mat(3, 3);

    Result.Mat(1, 1) = + Mat(2, 0) * Mat(3, 2) * Mat(0, 3) - Mat(3, 0) * Mat(2, 2) * Mat(0, 3) + Mat(3, 0) * Mat(0, 2) * Mat(2, 3)
        - Mat(0, 0) * Mat(3, 2) * Mat(2, 3) - Mat(2, 0) * Mat(0, 2) * Mat(3, 3) + Mat(0, 0) * Mat(2, 2) * Mat(3, 3);

    Result.Mat(2, 1) = + Mat(3, 0) * Mat(2, 1) * Mat(0, 3) - Mat(2, 0) * Mat(3, 1) * Mat(0, 3) - Mat(3, 0) * Mat(0, 1) * Mat(2, 3)
        + Mat(0, 0) * Mat(3, 1) * Mat(2, 3) + Mat(2, 0) * Mat(0, 1) * Mat(3, 3) - Mat(0, 0) * Mat(2, 1) * Mat(3, 3);

    Result.Mat(3, 1) = + Mat(2, 0) * Mat(3, 1) * Mat(0, 2) - Mat(3, 0) * Mat(2, 1) * Mat(0, 2) + Mat(3, 0) * Mat(0, 1) * Mat(2, 2)
        - Mat(0, 0) * Mat(3, 1) * Mat(2, 2) - Mat(2, 0) * Mat(0, 1) * Mat(3, 2) + Mat(0, 0) * Mat(2, 1) * Mat(3, 2);

    Result.Mat(0, 2) = + Mat(1, 1) * Mat(3, 2) * Mat(0, 3) - Mat(3, 1) * Mat(1, 2) * Mat(0, 3) + Mat(3, 1) * Mat(0, 2) * Mat(1, 3)
        - Mat(0, 1) * Mat(3, 2) * Mat(1, 3) - Mat(1, 1) * Mat(0, 2) * Mat(3, 3) + Mat(0, 1) * Mat(1, 2) * Mat(3, 3);

    Result.Mat(1, 2) = + Mat(3, 0) * Mat(1, 2) * Mat(0, 3) - Mat(1, 0) * Mat(3, 2) * Mat(0, 3) - Mat(3, 0) * Mat(0, 2) * Mat(1, 3)
        + Mat(0, 0) * Mat(3, 2) * Mat(1, 3) + Mat(1, 0) * Mat(0, 2) * Mat(3, 3) - Mat(0, 0) * Mat(1, 2) * Mat(3, 3);

    Result.Mat(2, 2) = + Mat(1, 0) * Mat(3, 1) * Mat(0, 3) - Mat(3, 0) * Mat(1, 1) * Mat(0, 3) + Mat(3, 0) * Mat(0, 1) * Mat(1, 3)
        - Mat(0, 0) * Mat(3, 1) * Mat(1, 3) - Mat(1, 0) * Mat(0, 1) * Mat(3, 3) + Mat(0, 0) * Mat(1, 1) * Mat(3, 3);

    Result.Mat(3, 2) = + Mat(3, 0) * Mat(1, 1) * Mat(0, 2) - Mat(1, 0) * Mat(3, 1) * Mat(0, 2) - Mat(3, 0) * Mat(0, 1) * Mat(1, 2)
        + Mat(0, 0) * Mat(3, 1) * Mat(1, 2) + Mat(1, 0) * Mat(0, 1) * Mat(3, 2) - Mat(0, 0) * Mat(1, 1) * Mat(3, 2);

    Result.Mat(0, 3) = + Mat(2, 1) * Mat(1, 2) * Mat(0, 3) - Mat(1, 1) * Mat(2, 2) * Mat(0, 3) - Mat(2, 1) * Mat(0, 2) * Mat(1, 3)
        + Mat(0, 1) * Mat(2, 2) * Mat(1, 3) + Mat(1, 1) * Mat(0, 2) * Mat(2, 3) - Mat(0, 1) * Mat(1, 2) * Mat(2, 3);

    Result.Mat(1, 3) = + Mat(1, 0) * Mat(2, 2) * Mat(0, 3) - Mat(2, 0) * Mat(1, 2) * Mat(0, 3) + Mat(2, 0) * Mat(0, 2) * Mat(1, 3)
        - Mat(0, 0) * Mat(2, 2) * Mat(1, 3) - Mat(1, 0) * Mat(0, 2) * Mat(2, 3) + Mat(0, 0) * Mat(1, 2) * Mat(2, 3);

    Result.Mat(2, 3) = + Mat(2, 0) * Mat(1, 1) * Mat(0, 3) - Mat(1, 0) * Mat(2, 1) * Mat(0, 3) - Mat(2, 0) * Mat(0, 1) * Mat(1, 3)
        + Mat(0, 0) * Mat(2, 1) * Mat(1, 3) + Mat(1, 0) * Mat(0, 1) * Mat(2, 3) - Mat(0, 0) * Mat(1, 1) * Mat(2, 3);

    Result.Mat(3, 3) = + Mat(1, 0) * Mat(2, 1) * Mat(0, 2) - Mat(2, 0) * Mat(1, 1) * Mat(0, 2) + Mat(2, 0) * Mat(0, 1) * Mat(1, 2)
        - Mat(0, 0) * Mat(2, 1) * Mat(1, 2) - Mat(1, 0) * Mat(0, 1) * Mat(2, 2) + Mat(0, 0) * Mat(1, 1) * Mat(2, 2);

    return Result / Determinant();
}

Matrix Matrix::CreateFromQuaternion(const Quaternion& Value)
{
    Matrix Result;

    float XX = Value.X * Value.X;
    float XY = Value.X * Value.Y;
    float XZ = Value.X * Value.Z;
    float XW = Value.X * Value.W;

    float YY = Value.Y * Value.Y;
    float YZ = Value.Y * Value.Z;
    float YW = Value.Y * Value.W;

    float ZZ = Value.Z * Value.Z;
    float ZW = Value.Z * Value.W;

    Result.Mat(0, 0) = 1 - 2 * (YY + ZZ);
    Result.Mat(1, 0) = 2 * (XY - ZW);
    Result.Mat(2, 0) = 2 * (XZ + YW);
    Result.Mat(3, 0) = 0;

    Result.Mat(0, 1) = 2 * (XY + ZW);
    Result.Mat(1, 1) = 1 - 2 * (XX + ZZ);
    Result.Mat(2, 1) = 2 * (YZ - XW);
    Result.Mat(3, 1) = 0;

    Result.Mat(0, 2) = 2 * (XZ - YW);
    Result.Mat(1, 2) = 2 * (YZ + XW);
    Result.Mat(2, 2) = 1 - 2 * (XX + YY);
    Result.Mat(3, 2) = 0;

    Result.Mat(0, 3) = 0;
    Result.Mat(1, 3) = 0;
    Result.Mat(2, 3) = 0;
    Result.Mat(3, 3) = 1;

    return Result;
}

Vector3 Matrix::TransformVector(const Vector3& Vector, const Matrix& Value)
{
    Vector3 Result;

    Result.X = (Vector.X * Value.Mat(0, 0)) + (Vector.Y * Value.Mat(1, 0)) + (Vector.Z * Value.Mat(2, 0)) + Value.Mat(3, 0);
    Result.Y = (Vector.X * Value.Mat(0, 1)) + (Vector.Y * Value.Mat(1, 1)) + (Vector.Z * Value.Mat(2, 1)) + Value.Mat(3, 1);
    Result.Z = (Vector.X * Value.Mat(0, 2)) + (Vector.Y * Value.Mat(1, 2)) + (Vector.Z * Value.Mat(2, 2)) + Value.Mat(3, 2);

    return Result;
}

Quaternion::Quaternion()
{
    X = 0;
    Y = 0;
    Z = 0;
    W = 1;
}

Quaternion::Quaternion(float XCoord, float YCoord, float ZCoord, float WCoord)
{
    X = XCoord;
    Y = YCoord;
    Z = ZCoord;
    W = WCoord;
}

Quaternion Quaternion::operator*(const Quaternion& Value) const
{
    return Quaternion(
        W * Value.X + X * Value.W + Y * Value.Z - Z * Value.Y,
        W * Value.Y - X * Value.Z + Y * Value.W + Z * Value.X,
        W * Value.Z + X * Value.Y - Y * Value.X + Z * Value.W,
        W * Value.W - X * Value.X - Y * Value.Y - Z * Value.Z);
}

Quaternion Quaternion::operator~() const
{
    return Quaternion(-X, -Y, -Z, W);
}

float Quaternion::Length() const
{
    return (float)std::sqrt(X * X + Y * Y + Z * Z + W * W);
}

void Quaternion::Normalize()
{
    auto Len = Length();
    X /= Len;
    Y /= Len;
    Z /= Len;
    W /= Len;
}

Vector3 Quaternion::ToEulerAngles() const
{
    Vector3 Result;
    float Matrix[4][4];
    {
        float num = X * X + Y * Y + Z * Z + W * W;
        float num2 = 0;
        if (num > 0.0)
        {
            num2 = 2.0f / num;
        }
        float num3 = X * num2;
        float num4 = Y * num2;
        float num5 = Z * num2;
        float num6 = W * num3;
        float num7 = W * num4;
        float num8 = W * num5;
        float num9 = X * num3;
        float num10 = X * num4;
        float num11 = X * num5;
        float num12 = Y * num4;
        float num13 = Y * num5;
        float num14 = Z * num5;
        Matrix[0][0] = 1.0f - (num12 + num14);
        Matrix[0][1] = num10 - num8;
        Matrix[0][2] = num11 + num7;
        Matrix[1][0] = num10 + num8;
        Matrix[1][1] = 1.0f - (num9 + num14);
        Matrix[1][2] = num13 - num6;
        Matrix[2][0] = num11 - num7;
        Matrix[2][1] = num13 + num6;
        Matrix[2][2] = 1.0f - (num9 + num12);
        Matrix[3][3] = 1.0f;
    }
    {
        uint32_t i = 0, j = 1, k = 2;
        float TempX = 0, TempY = 0, TempZ = 0;
        float SqSum = std::sqrt(Matrix[i][i] * Matrix[i][i] + Matrix[j][i] * Matrix[j][i]);
        if (SqSum > 0.00016)
        {
            TempX = std::atan2(Matrix[k][j], Matrix[k][k]);
            TempY = std::atan2(-Matrix[k][i], SqSum);
            TempZ = std::atan2(Matrix[j][i], Matrix[i][i]);
        }
        else
        {
            TempX = std::atan2(-Matrix[j][k], Matrix[j][j]);
            TempY = std::atan2(-Matrix[k][i], SqSum);
            TempZ = 0.0;
        }
        Result.X = VectorMath::RadiansToDegrees(TempX);
        Result.Y = VectorMath::RadiansToDegrees(TempY);
        Result.Z = VectorMath::RadiansToDegrees(TempZ);
    }
    return Result;
}

Quaternion Quaternion::Identity()
{
    return Quaternion(0, 0, 0, 1);
}

Quaternion VectorPacking::QuatPackingA(const uint64_t PackedData)
{
    uint64_t PackedQuatData = PackedData;
    int Axis = PackedQuatData & 3;
    int WSign = PackedQuatData >> 63;
    PackedQuatData >>= 2;

    int ix = (int)(PackedQuatData & 0xfffff);
    if (ix > 0x7ffff) ix -= 0x100000;
    int iy = (int)((PackedQuatData >> 20) & 0xfffff);
    if (iy > 0x7ffff) iy -= 0x100000;
    int iz = (int)((PackedQuatData >> 40) & 0xfffff);
    if (iz > 0x7ffff) iz -= 0x100000;
    float x = ix / 1048575.f;
    float y = iy / 1048575.f;
    float z = iz / 1048575.f;

    x *= 1.41421f;
    y *= 1.41421f;
    z *= 1.41421f;

    float w = (float)std::pow<float>(1 - x * x - y * y - z * z, 0.5f);

    if (WSign)
        w = -w;

    switch (Axis)
    {
    case 0: return Quaternion(w, x, y, z);
    case 1: return Quaternion(x, y, z, w);
    case 2: return Quaternion(y, z, w, x);
    case 3: return Quaternion(z, w, x, y);
    default: return Quaternion::Identity();
    }
}

Quaternion VectorPacking::QuatPacking2DA(const uint32_t PackedData)
{
    uint32_t PackedQuatData = PackedData;
    int WSign = (PackedQuatData >> 30) & 1;
    PackedQuatData &= 0xBFFFFFFF;

    float z = *(float*)&PackedQuatData;
    float w = (float)std::sqrtf(1.f - std::pow<float>(z, 2.f));

    if (WSign)
        w = -w;

    return Quaternion(0, 0, z, w);
}