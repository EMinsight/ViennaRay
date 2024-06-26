#pragma once

#include <rayMessage.hpp>
#include <rayRNG.hpp>

#if VIENNARAY_EMBREE_VERSION < 4
#include <embree3/rtcore.h>
#else
#include <embree4/rtcore.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define ARCH_X86
#include <immintrin.h>
#endif

#include <omp.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

template <typename NumericType> using rayPair = std::array<NumericType, 2>;

template <typename NumericType> using rayTriple = std::array<NumericType, 3>;

template <typename NumericType> using rayQuadruple = std::array<NumericType, 4>;

enum class rayNormalizationType : unsigned { SOURCE = 0, MAX = 1 };

enum class rayTraceDirection : unsigned {
  POS_X = 0,
  NEG_X = 1,
  POS_Y = 2,
  NEG_Y = 3,
  POS_Z = 4,
  NEG_Z = 5
};

template <class NumericType> struct rayDataLog {

  std::vector<std::vector<NumericType>> data;

  void merge(rayDataLog<NumericType> &pOther) {
    assert(pOther.data.size() == data.size() &&
           "Size mismatch when merging logs");
    for (std::size_t i = 0; i < data.size(); i++) {
      assert(pOther.data[i].size() == data[i].size() &&
             "Size mismatch when merging log data");
      for (std::size_t j = 0; j < data[i].size(); j++) {
        data[i][j] += pOther.data[i][j];
      }
    }
  }
};

struct rayTraceInfo {
  size_t numRays;
  size_t totalRaysTraced;
  size_t totalDiskHits;
  size_t nonGeometryHits;
  size_t geometryHits;
  size_t particleHits;
  double time;
  bool warning = false;
  bool error = false;
};

namespace rayInternal {

// embree uses float internally
using rtcNumericType = float;

template <int D>
constexpr double DiskFactor =
    0.5 * (D == 3 ? 1.7320508 : 1.41421356237) * (1 + 1e-5);

/* ------------- Vector operation functions ------------- */
template <typename NumericType>
[[nodiscard]] rayTriple<NumericType> Sum(const rayTriple<NumericType> &pVecA,
                                         const rayTriple<NumericType> &pVecB) {
  return {pVecA[0] + pVecB[0], pVecA[1] + pVecB[1], pVecA[2] + pVecB[2]};
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType> Sum(const rayTriple<NumericType> &pVecA,
                                         const rayTriple<NumericType> &pVecB,
                                         const rayTriple<NumericType> &pT) {
  return {pVecA[0] + pVecB[0] + pT[0], pVecA[1] + pVecB[1] + pT[1],
          pVecA[2] + pVecB[2] + pT[2]};
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType> Diff(const rayTriple<NumericType> &pVecA,
                                          const rayTriple<NumericType> &pVecB) {
  return {pVecA[0] - pVecB[0], pVecA[1] - pVecB[1], pVecA[2] - pVecB[2]};
}

template <typename NumericType>
[[nodiscard]] rayPair<NumericType> Diff(const rayPair<NumericType> &pVecA,
                                        const rayPair<NumericType> &pVecB) {
  return {pVecA[0] - pVecB[0], pVecA[1] - pVecB[1]};
}

template <typename NumericType>
[[nodiscard]] NumericType DotProduct(const rayTriple<NumericType> &pVecA,
                                     const rayTriple<NumericType> &pVecB) {
  return pVecA[0] * pVecB[0] + pVecA[1] * pVecB[1] + pVecA[2] * pVecB[2];
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType>
CrossProduct(const rayTriple<NumericType> &pVecA,
             const rayTriple<NumericType> &pVecB) {
  rayTriple<NumericType> rr;
  rr[0] = pVecA[1] * pVecB[2] - pVecA[2] * pVecB[1];
  rr[1] = pVecA[2] * pVecB[0] - pVecA[0] * pVecB[2];
  rr[2] = pVecA[0] * pVecB[1] - pVecA[1] * pVecB[0];
  return rr;
}

template <typename NumericType, size_t D>
[[nodiscard]] NumericType Norm(const std::array<NumericType, D> &vec) {
  NumericType norm = 0;
  std::for_each(vec.begin(), vec.end(),
                [&norm](NumericType entry) { norm += entry * entry; });
  return std::sqrt(norm);
}

template <typename NumericType, size_t D>
void Normalize(std::array<NumericType, D> &vec) {
  NumericType norm = 1. / Norm(vec);
  if (norm == 1.)
    return;
  std::for_each(vec.begin(), vec.end(),
                [&norm](NumericType &entry) { entry *= norm; });
}

template <typename NumericType, size_t D>
[[nodiscard]] std::array<NumericType, D>
Normalize(const std::array<NumericType, D> &vec) {
  std::array<NumericType, D> normedVec = vec;
  auto norm = 1. / Norm(normedVec);
  if (norm == 1.)
    return normedVec;
  for (size_t i = 0; i < D; ++i) {
    normedVec[i] = norm * vec[i];
  }
  return normedVec;
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType> Inv(const rayTriple<NumericType> &vec) {
  return {-vec[0], -vec[1], -vec[2]};
}

template <typename NumericType>
void Scale(const NumericType pF, rayTriple<NumericType> &pT) {
  pT[0] *= pF;
  pT[1] *= pF;
  pT[2] *= pF;
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType> Scale(const NumericType pF,
                                           const rayTriple<NumericType> &pT) {
  return {pF * pT[0], pF * pT[1], pF * pT[2]};
}

template <typename NumericType, size_t D>
[[nodiscard]] NumericType Distance(const std::array<NumericType, D> &pVecA,
                                   const std::array<NumericType, D> &pVecB) {
  auto diff = Diff(pVecA, pVecB);
  return Norm(diff);
}

template <typename NumericType>
[[nodiscard]] rayTriple<NumericType>
ComputeNormal(const rayTriple<rayTriple<NumericType>> &planeCoords) {
  auto uu = Diff(planeCoords[1], planeCoords[0]);
  auto vv = Diff(planeCoords[2], planeCoords[0]);
  return CrossProduct(uu, vv);
}

template <typename NumericType>
bool IsNormalized(const rayTriple<NumericType> &vec) {
  constexpr double eps = 1e-4;
  auto norm = Norm(vec);
  return std::fabs(norm - 1) < eps;
}
/* ------------------------------------------------------ */

/* -------------- Ray tracing preparation -------------- */
template <typename NumericType, int D>
void adjustBoundingBox(rayPair<rayTriple<NumericType>> &bdBox,
                       rayTraceDirection direction, NumericType discRadius) {
  // For 2D geometries adjust bounding box in z-direction
  if constexpr (D == 2) {
    bdBox[0][2] -= discRadius;
    bdBox[1][2] += discRadius;

    if (direction == rayTraceDirection::POS_Z ||
        direction == rayTraceDirection::NEG_Z) {
      rayMessage::getInstance().addError(
          "Ray source is set in z-direction for 2D geometry");
    }
  }

  switch (direction) {
  case rayTraceDirection::POS_X:
    bdBox[1][0] += 2 * discRadius;
    break;

  case rayTraceDirection::NEG_X:
    bdBox[0][0] -= 2 * discRadius;
    break;

  case rayTraceDirection::POS_Y:
    bdBox[1][1] += 2 * discRadius;
    break;

  case rayTraceDirection::NEG_Y:
    bdBox[0][1] -= 2 * discRadius;
    break;

  case rayTraceDirection::POS_Z:
    bdBox[1][2] += 2 * discRadius;
    break;

  case rayTraceDirection::NEG_Z:
    bdBox[0][2] -= 2 * discRadius;
    break;
  }
}

[[nodiscard]] inline std::array<int, 5>
getTraceSettings(rayTraceDirection sourceDir) {
  // Trace Settings: sourceDir, boundaryDir1, boundaryDir2, minMax bdBox
  // source, posNeg dir
  std::array<int, 5> set{0, 0, 0, 0, 0};
  switch (sourceDir) {
  case rayTraceDirection::POS_X: {
    set[0] = 0;
    set[1] = 1;
    set[2] = 2;
    set[3] = 1;
    set[4] = -1;
    break;
  }
  case rayTraceDirection::NEG_X: {
    set[0] = 0;
    set[1] = 1;
    set[2] = 2;
    set[3] = 0;
    set[4] = 1;
    break;
  }
  case rayTraceDirection::POS_Y: {
    set[0] = 1;
    set[1] = 0;
    set[2] = 2;
    set[3] = 1;
    set[4] = -1;
    break;
  }
  case rayTraceDirection::NEG_Y: {
    set[0] = 1;
    set[1] = 0;
    set[2] = 2;
    set[3] = 0;
    set[4] = 1;
    break;
  }
  case rayTraceDirection::POS_Z: {
    set[0] = 2;
    set[1] = 0;
    set[2] = 1;
    set[3] = 1;
    set[4] = -1;
    break;
  }
  case rayTraceDirection::NEG_Z: {
    set[0] = 2;
    set[1] = 0;
    set[2] = 1;
    set[3] = 0;
    set[4] = 1;
    break;
  }
  }

  return set;
}

template <typename T1, typename T2>
void fillRay(RTCRay &ray, const rayTriple<T1> &origin,
             const rayTriple<T2> &direction, const float tnear = 1e-4f,
             const float time = 0.0f) {
#ifdef ARCH_X86
  reinterpret_cast<__m128 &>(ray) =
      _mm_set_ps(tnear, (float)origin[2], (float)origin[1], (float)origin[0]);

  reinterpret_cast<__m128 &>(ray.dir_x) = _mm_set_ps(
      time, (float)direction[2], (float)direction[1], (float)direction[0]);
#else
  ray.org_x = (float)origin[0];
  ray.org_y = (float)origin[1];
  ray.org_z = (float)origin[2];
  ray.tnear = tnear;

  ray.dir_x = (float)direction[0];
  ray.dir_y = (float)direction[1];
  ray.dir_z = (float)direction[2];
  ray.time = time;
#endif
}

template <>
void fillRay<float>(RTCRay &ray, const rayTriple<float> &origin,
                    const rayTriple<float> &direction, const float tnear,
                    const float time) {
#ifdef ARCH_X86
  reinterpret_cast<__m128 &>(ray) =
      _mm_set_ps(tnear, origin[2], origin[1], origin[0]);

  reinterpret_cast<__m128 &>(ray.dir_x) =
      _mm_set_ps(time, direction[2], direction[1], direction[0]);
#else
  ray.org_x = origin[0];
  ray.org_y = origin[1];
  ray.org_z = origin[2];
  ray.tnear = tnear;

  ray.dir_x = direction[0];
  ray.dir_y = direction[1];
  ray.dir_z = direction[2];
  ray.time = time;
#endif
}

/* ------------------------------------------------------ */

template <typename NumericType>
[[nodiscard]] static rayTriple<NumericType>
pickRandomPointOnUnitSphere(rayRNG &RNG) {
  std::uniform_real_distribution<NumericType> uniDist;
  NumericType x, y, z, x2, y2, x2py2;
  do {
    x = 2 * uniDist(RNG) - 1.;
    x2 = x * x;
    y = 2 * uniDist(RNG) - 1.;
    y2 = y * y;
    x2py2 = x2 + y2;
  } while (x2py2 >= 1.);
  NumericType tmp = 2 * std::sqrt(1. - x2py2);
  x *= tmp;
  y *= tmp;
  z = 1. - 2 * x2py2;
  return rayTriple<NumericType>{x, y, z};
}

// Returns some orthonormal basis containing a the input vector
// (possibly scaled) as the first element of the return value.
// This function is deterministic, i.e., for one input it will return always
// the same result.
template <typename NumericType>
[[nodiscard]] rayTriple<rayTriple<NumericType>>
getOrthonormalBasis(const rayTriple<NumericType> &vec) {
  rayTriple<rayTriple<NumericType>> rr;
  rr[0] = vec;

  // Calculate a vector (rr[1]) which is perpendicular to rr[0]
  // https://math.stackexchange.com/questions/137362/how-to-find-perpendicular-vector-to-another-vector#answer-211195
  rayTriple<NumericType> candidate0{rr[0][2], rr[0][2], -(rr[0][0] + rr[0][1])};
  rayTriple<NumericType> candidate1{rr[0][1], -(rr[0][0] + rr[0][2]), rr[0][1]};
  rayTriple<NumericType> candidate2{-(rr[0][1] + rr[0][2]), rr[0][0], rr[0][0]};
  // We choose the candidate which maximizes the sum of its components,
  // because we want to avoid numeric errors and that the result is (0, 0, 0).
  std::array<rayTriple<NumericType>, 3> cc = {candidate0, candidate1,
                                              candidate2};
  auto sumFun = [](const rayTriple<NumericType> &oo) {
    return oo[0] + oo[1] + oo[2];
  };
  int maxIdx = 0;
  for (size_t idx = 1; idx < cc.size(); ++idx) {
    if (sumFun(cc[idx]) > sumFun(cc[maxIdx])) {
      maxIdx = idx;
    }
  }
  assert(maxIdx < 3 && "Error in computation of perpendicular vector");
  rr[1] = cc[maxIdx];

  rr[2] = rayInternal::CrossProduct(rr[0], rr[1]);
  rayInternal::Normalize(rr[0]);
  rayInternal::Normalize(rr[1]);
  rayInternal::Normalize(rr[2]);

  // Sanity check
  assert(std::abs(rayInternal::DotProduct(rr[0], rr[1])) < 1e-6 &&
         "Error in orthonormal basis computation");
  assert(std::abs(rayInternal::DotProduct(rr[1], rr[2])) < 1e-6 &&
         "Error in orthonormal basis computation");
  assert(std::abs(rayInternal::DotProduct(rr[2], rr[0])) < 1e-6 &&
         "Error in orthonormal basis computation");
  return rr;
}

/* -------- Create or read simple geometries for testing -------- */
template <typename NumericType>
void createPlaneGrid(const NumericType gridDelta, const NumericType extent,
                     const std::array<int, 3> direction,
                     std::vector<std::array<NumericType, 3>> &points,
                     std::vector<std::array<NumericType, 3>> &normals) {
  std::array<NumericType, 3> point = {-extent, -extent, -extent};
  std::array<NumericType, 3> normal = {0., 0., 0.};
  point[direction[2]] = 0;
  normal[direction[2]] = 1.;

  points.clear();
  normals.clear();
  points.reserve(int(extent / gridDelta) * int(extent / gridDelta));
  normals.reserve(int(extent / gridDelta) * int(extent / gridDelta));
  while (point[direction[0]] <= extent) {
    while (point[direction[1]] <= extent) {
      points.push_back(point);
      normals.push_back(normal);
      point[direction[1]] += gridDelta;
    }
    point[direction[1]] = -extent;
    point[direction[0]] += gridDelta;
  }
  points.shrink_to_fit();
  normals.shrink_to_fit();
}

template <typename NumericType>
void readGridFromFile(std::string fileName, NumericType &gridDelta,
                      std::vector<rayTriple<NumericType>> &points,
                      std::vector<rayTriple<NumericType>> &normals) {
  std::ifstream dataFile(fileName);
  if (!dataFile.is_open()) {
    std::cout << "Cannot read file " << fileName << std::endl;
    return;
  }
  size_t numPoints;
  dataFile >> numPoints;
  dataFile >> gridDelta;
  points.resize(numPoints);
  normals.resize(numPoints);
  for (size_t i = 0; i < numPoints; ++i)
    dataFile >> points[i][0] >> points[i][1] >> points[i][2];
  for (size_t i = 0; i < numPoints; ++i)
    dataFile >> normals[i][0] >> normals[i][1] >> normals[i][2];
  dataFile.close();
}

template <typename NumericType, int D = 3>
void writeVTK(std::string filename,
              const std::vector<rayTriple<NumericType>> &points,
              const std::vector<NumericType> &flux) {
  std::ofstream f(filename.c_str());

  f << "# vtk DataFile Version 2.0" << std::endl;
  f << D << "D Surface" << std::endl;
  f << "ASCII" << std::endl;
  f << "DATASET UNSTRUCTURED_GRID" << std::endl;
  f << "POINTS " << points.size() << " float" << std::endl;

  for (unsigned int i = 0; i < points.size(); i++) {
    for (int j = 0; j < 3; j++)
      f << static_cast<float>(points[i][j]) << " ";
    f << std::endl;
  }

  f << "CELLS " << points.size() << " " << points.size() * 2 << std::endl;
  size_t c = 0;
  for (unsigned int i = 0; i < points.size(); i++) {
    f << 1 << " " << c++ << std::endl;
  }

  f << "CELL_TYPES " << points.size() << std::endl;
  for (unsigned i = 0; i < points.size(); ++i)
    f << 1 << std::endl;

  f << "CELL_DATA " << flux.size() << std::endl;
  f << "SCALARS flux float" << std::endl;
  f << "LOOKUP_TABLE default" << std::endl;
  for (unsigned j = 0; j < flux.size(); ++j) {
    f << ((std::abs(flux[j]) < 1e-6) ? 0.0 : flux[j]) << std::endl;
  }

  f.close();
}
/* -------------------------------------------------------------- */

template <typename NumericType, int D>
[[nodiscard]] std::vector<rayTriple<NumericType>>
createSourceGrid(const rayPair<rayTriple<NumericType>> &pBdBox,
                 const size_t pNumPoints, const NumericType pGridDelta,
                 const std::array<int, 5> &pTraceSettings) {
  std::vector<rayTriple<NumericType>> sourceGrid;
  sourceGrid.reserve(pNumPoints);
  constexpr double eps = 1e-4;
  // Trace settings
  // sourceDir, boundaryDir1, boundaryDir2, minMax bdBox source, posNeg dir
  auto rayDir = pTraceSettings[0];
  auto firstDir = pTraceSettings[1];
  auto secondDir = pTraceSettings[2];
  auto minMax = pTraceSettings[3];
  assert((!(D == 2) || rayDir != 2) && "Source direction z in 2D geometry");

  auto len1 = pBdBox[1][firstDir] - pBdBox[0][firstDir];
  auto len2 = pBdBox[1][secondDir] - pBdBox[0][secondDir];
  size_t numPointsInFirstDir = static_cast<size_t>(round(len1 / pGridDelta));
  size_t numPointsInSecondDir = static_cast<size_t>(round(len2 / pGridDelta));
  auto ratio = numPointsInFirstDir / numPointsInSecondDir;
  numPointsInFirstDir = static_cast<size_t>(std::sqrt(pNumPoints * ratio));
  numPointsInSecondDir = static_cast<size_t>(std::sqrt(pNumPoints / ratio));

  auto firstGridDelta =
      (len1 - 2 * eps) / (NumericType)(numPointsInFirstDir - 1);
  auto secondGridDelta =
      (len2 - 2 * eps) / (NumericType)(numPointsInSecondDir - 1);

  rayTriple<NumericType> point;
  point[rayDir] = pBdBox[minMax][rayDir];
  for (auto uu = pBdBox[0][secondDir] + eps; uu <= pBdBox[1][secondDir] - eps;
       uu += secondGridDelta) {
    if constexpr (D == 2) {
      point[secondDir] = 0.;
    } else {
      point[secondDir] = uu;
    }

    for (auto vv = pBdBox[0][firstDir] + eps; vv <= pBdBox[1][firstDir] - eps;
         vv += firstGridDelta) {
      point[firstDir] = vv;
      sourceGrid.push_back(point);
    }
  }
  sourceGrid.shrink_to_fit();
  return sourceGrid;
}

template <typename TimeUnit> const static uint64_t timeStampNow() {
  return std::chrono::duration_cast<TimeUnit>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/* ------------- Debug convenience functions ------------- */
template <typename NumericType>
void printTriple(const rayTriple<NumericType> &vec, bool endl = true) {
  std::cout << "(" << vec[0] << ", " << vec[1] << ", " << vec[2] << ")"
            << (endl ? "\n" : "");
}

template <typename NumericType>
void printPair(const rayPair<NumericType> &vec) {
  std::cout << "(" << vec[0] << ", " << vec[1] << ")" << std::endl;
}

template <typename NumericType>
void printBoundingBox(const rayPair<rayTriple<NumericType>> &bdBox) {
  std::cout << "Bounding box min coords: ";
  printTriple(bdBox[0]);
  std::cout << "Bounding box max coords: ";
  printTriple(bdBox[1]);
}
/* ------------------------------------------------------- */
} // namespace rayInternal
