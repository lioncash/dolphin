// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cmath>
#include <cstdlib>

class Vec3
{
public:
  constexpr Vec3() = default;
  constexpr explicit Vec3(float f) : data{f, f, f} {}
  constexpr explicit Vec3(const float* f) : data{f[0], f[1], f[2]} {}
  constexpr explicit Vec3(const float x, const float y, const float z) : data{x, y, z} {}

  constexpr void set(const float x, const float y, const float z) { data = {x, y, z}; }

  constexpr float& x() { return data[0]; }
  constexpr const float& x() const { return data[0]; }
  constexpr float& y() { return data[1]; }
  constexpr const float& y() const { return data[1]; }
  constexpr float& z() { return data[2]; }
  constexpr const float& z() const { return data[2]; }

  constexpr Vec3 operator+(const Vec3& other) const
  {
    return Vec3(x() + other.x(), y() + other.y(), z() + other.z());
  }

  constexpr void operator+=(const Vec3& other)
  {
    x() += other.x();
    y() += other.y();
    z() += other.z();
  }

  constexpr Vec3 operator-(const Vec3& v) const
  {
    return Vec3(x() - v.x(), y() - v.y(), z() - v.z());
  }

  constexpr void operator-=(const Vec3& other)
  {
    x() -= other.x();
    y() -= other.y();
    z() -= other.z();
  }

  constexpr Vec3 operator-() const { return Vec3(-x(), -y(), -z()); }
  constexpr Vec3 operator*(const float f) const { return Vec3(x() * f, y() * f, z() * f); }
  constexpr Vec3 operator/(const float f) const
  {
    const float invf = (1.0f / f);
    return Vec3(x() * invf, y() * invf, z() * invf);
  }

  constexpr void operator/=(const float f) { *this = *this / f; }
  constexpr float operator*(const Vec3& other) const
  {
    return (x() * other.x()) + (y() * other.y()) + (z() * other.z());
  }
  constexpr void operator*=(const float f) { *this = *this * f; }
  constexpr Vec3 ScaledBy(const Vec3& other) const
  {
    return Vec3(x() * other.x(), y() * other.y(), z() * other.z());
  }
  constexpr Vec3 operator%(const Vec3& v) const
  {
    return Vec3((y() * v.z()) - (z() * v.y()), (z() * v.x()) - (x() * v.z()),
                (x() * v.y()) - (y() * v.x()));
  }

  constexpr float Length2() const { return (x() * x()) + (y() * y()) + (z() * z()); }
  float Length() const { return sqrtf(Length2()); }
  constexpr float Distance2To(const Vec3& other) { return (other - (*this)).Length2(); }
  Vec3 Normalized() const { return (*this) / Length(); }
  void Normalize() { (*this) /= Length(); }
  constexpr float& operator[](size_t i) { return data[i]; }
  constexpr const float& operator[](size_t i) const { return data[i]; }
  constexpr bool operator==(const Vec3& other) const
  {
    return x() == other.x() && y() == other.y() && z() == other.z();
  }
  constexpr bool operator!=(const Vec3& other) const { return !operator==(other); }
  constexpr void SetZero() { data = {}; }

private:
  std::array<float, 3> data{};
};
