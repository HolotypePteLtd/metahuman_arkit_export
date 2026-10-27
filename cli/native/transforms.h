#pragma once
#include <array>
#include <cmath>
#include <stdexcept>
namespace arkit {
using Vec3 = std::array<double, 3>;
inline Vec3 operator+(Vec3 a, const Vec3 &b) {
  for (int i = 0; i < 3; ++i)
    a[i] += b[i];
  return a;
}
inline Vec3 operator-(Vec3 a, const Vec3 &b) {
  for (int i = 0; i < 3; ++i)
    a[i] -= b[i];
  return a;
}
inline Vec3 operator*(Vec3 a, double w) {
  for (auto &x : a)
    x *= w;
  return a;
}
inline double norm(const Vec3 &v) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
struct Mat4 {
  std::array<double, 16> v{};
  double &operator()(int r, int c) { return v[r * 4 + c]; }
  double operator()(int r, int c) const { return v[r * 4 + c]; }
  static Mat4 identity() {
    Mat4 m;
    for (int i = 0; i < 4; ++i)
      m(i, i) = 1;
    return m;
  }
};
inline Mat4 operator*(const Mat4 &a, const Mat4 &b) {
  Mat4 c;
  for (int r = 0; r < 4; ++r)
    for (int s = 0; s < 4; ++s)
      for (int k = 0; k < 4; ++k)
        c(r, s) += a(r, k) * b(k, s);
  return c;
}
inline Vec3 transform(const Mat4 &a, const Vec3 &p) {
  Vec3 r{};
  for (int i = 0; i < 3; ++i) {
    r[i] = a(i, 3);
    for (int j = 0; j < 3; ++j)
      r[i] += a(i, j) * p[j];
  }
  return r;
}
inline Mat4 inverse(Mat4 a) {
  auto b = Mat4::identity();
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::abs(a(r, c)) > std::abs(a(pivot, c)))
        pivot = r;
    if (std::abs(a(pivot, c)) < 1e-15)
      throw std::runtime_error("Singular bind transform");
    for (int j = 0; j < 4; ++j) {
      std::swap(a(c, j), a(pivot, j));
      std::swap(b(c, j), b(pivot, j));
    }
    double scale = a(c, c);
    for (int j = 0; j < 4; ++j) {
      a(c, j) /= scale;
      b(c, j) /= scale;
    }
    for (int r = 0; r < 4; ++r)
      if (r != c) {
        double w = a(r, c);
        for (int j = 0; j < 4; ++j) {
          a(r, j) -= w * a(c, j);
          b(r, j) -= w * b(c, j);
        }
      }
  }
  return b;
}
inline Mat4 quaternion(const float *q) {
  double x = q[0], y = q[1], z = q[2], w = q[3];
  double n = std::sqrt(x * x + y * y + z * z + w * w);
  if (n < 1e-12)
    throw std::runtime_error(
        "RigLogic returned a zero-length joint quaternion");
  x /= n;
  y /= n;
  z /= n;
  w /= n;
  auto m = Mat4::identity();
  m(0, 0) = 1 - 2 * (y * y + z * z);
  m(0, 1) = 2 * (x * y - z * w);
  m(0, 2) = 2 * (x * z + y * w);
  m(1, 0) = 2 * (x * y + z * w);
  m(1, 1) = 1 - 2 * (x * x + z * z);
  m(1, 2) = 2 * (y * z - x * w);
  m(2, 0) = 2 * (x * z - y * w);
  m(2, 1) = 2 * (y * z + x * w);
  m(2, 2) = 1 - 2 * (x * x + y * y);
  return m;
}
} // namespace arkit
