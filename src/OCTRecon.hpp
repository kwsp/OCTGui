#pragma once

#include "fftw.hpp"
#include "phasecorr.hpp"
#include "timeit.hpp"
#include <cassert>
#include <fftw3.h>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/core/base.hpp>
#include <opencv2/opencv.hpp>
#include <span>
#include <vector>

// NOLINTBEGIN(*-pointer-arithmetic, *-magic-numbers)

namespace OCT {

namespace fs = std::filesystem;

template <typename T>
concept Floating = std::is_same_v<T, double> || std::is_same_v<T, float>;

template <Floating T> std::vector<T> getHamming(int n) {
  std::vector<T> win(n);
  for (int i = 0; i < n; ++i) {
    win[i] = 0.54 - 0.46 * std::cos(2 * 3.1415926535897932384626 * i / n);
  }
  return win;
}

template <typename T>
inline void readTextFileToArray(const fs::path &filename, std::span<T> dst) {
  std::ifstream ifs(filename);
  if (ifs.is_open()) {
    T val{};
    size_t i{};

    while (ifs >> val && i < dst.size()) {
      dst[i++] = val;
    }

    if (ifs.eof()) {
      std::cout << "Reached end of file " << filename << '\n';
    } else if (ifs.fail()) {
      std::cerr << "Failed to read value in file " << filename << '\n';
    } else if (ifs.bad()) {
      std::cerr << "Critical I/O error occured in file " << filename << '\n';
    }
  }
}

template <Floating T> struct phaseCalibUnit {
  size_t idx;
  T l_coeff;
  T r_coeff;

  friend std::istream &operator>>(std::istream &is,
                                  phaseCalibUnit<T> &calibUnit) {
    return is >> calibUnit.idx >> calibUnit.l_coeff >> calibUnit.r_coeff;
  }
};

template <Floating T> struct Calibration {
  std::vector<T> background;
  std::vector<phaseCalibUnit<T>> phaseCalib;

  Calibration(int n_samples, const fs::path &backgroundFile,
              const fs::path &phaseFile)
      : background(n_samples), phaseCalib(n_samples) {
    readTextFileToArray<T>(backgroundFile, background);
    readTextFileToArray<phaseCalibUnit<T>>(phaseFile, phaseCalib);
  }
};

template <Floating T> struct ScanConversionParams {
  T contrast;
  T brightness;
};

template <typename T, typename Tout = uint8_t>
void logCompress(Tout *outptr, size_t imageDepth, fftw::Complex<T> *cx,
                 T contrast, T brightness) {
  for (size_t i = 0; i < imageDepth; ++i) {
    const auto ro = cx[i][0];
    const auto io = cx[i][1];

    T val = ro * ro + io * io;
    val = contrast * (10 * log10(val) + brightness);
    val = val < 0 ? 0 : val > 255 ? 255 : val;
    outptr[i] = static_cast<Tout>(val);
  }
}

inline int getDistortionOffset(const cv::Mat &mat, int theoryWidth,
                               int corrWidth) {
  auto firstStrip = mat(cv::Rect(0, 0, corrWidth, mat.rows));
  auto lastStrip = mat(cv::Rect(theoryWidth, 0, corrWidth, mat.rows));
  firstStrip.convertTo(firstStrip, CV_32F);
  lastStrip.convertTo(lastStrip, CV_32F);
  return static_cast<int>(
      std::round(cvMod::phaseCorrelate(firstStrip, lastStrip).x));
}

inline void shiftXCircular(const cv::Mat &src, cv::Mat &dst, int shiftX) {
  // Normalize shift to positive range
  int width = src.cols;
  int shift = (shiftX % width + width) % width;

  // Create dst with the same size and type as src
  dst.create(src.size(), src.type());

  // Handle wrapping using modular arithmetic
  src(cv::Rect(0, 0, width - shift, src.rows))
      .copyTo(dst(cv::Rect(shift, 0, width - shift, src.rows)));

  src(cv::Rect(width - shift, 0, shift, src.rows))
      .copyTo(dst(cv::Rect(0, 0, shift, src.rows)));
}

template <typename T> inline void circshift(cv::Mat_<T> &mat, int idx) {
  assert(mat.isContinuous());
  idx = (idx + mat.cols) % mat.cols;
  const int depth = mat.channels();
  const int cols = mat.cols * depth;
  idx *= depth;
  for (int j = 0; j < mat.rows; ++j) {
    auto *ptr = mat.template ptr<T>(j);
    std::rotate(ptr, ptr + idx, ptr + cols);
  }
}

template <Floating T>
[[nodiscard]] cv::Mat_<uint8_t>
reconBscan(const Calibration<T> &calib, const std::span<const uint16_t> fringe,
           size_t ALineSize, const int imageDepth = 624,
           ScanConversionParams<T> conversionParams = ScanConversionParams<T>{
               .contrast = 9., .brightness = -57.}) {

  assert((fringe.size() % ALineSize) == 0);
  const auto nLines = fringe.size() / ALineSize;

  // cv::Mat constructor takes (height, width)
  cv::Mat_<T> mat(nLines, imageDepth);

  const auto win = getHamming<T>(ALineSize);
  const auto contrast = conversionParams.contrast;
  const auto brightness = conversionParams.brightness;

  const auto &fft = fftw::EngineR2C1D<T>::get(ALineSize);

  const cv::Range range(0, nLines);
  cv::parallel_for_(range, [&](cv::Range range) {
    fftw::R2CBuffer<T> fftBuf(ALineSize);
    std::vector<T> alineBuf(ALineSize);
    std::vector<T> linearKFringe(ALineSize);

    for (int j = range.start; j < range.end; ++j) {
      const auto offset = j * ALineSize;

      // 1. Subtract background
      for (int i = 0; i < ALineSize; ++i) {
        alineBuf[i] = fringe[offset + i] - calib.background[i];
      }

      // 2. Interpolate phase calibration data
      for (int i = 0; i < ALineSize - 1; ++i) {
        const auto idx = calib.phaseCalib[i].idx;
        const auto unit = calib.phaseCalib[idx];
        const auto l_coeff = unit.l_coeff;
        const auto r_coeff = unit.r_coeff;
        linearKFringe[i] =
            alineBuf[idx] * l_coeff + alineBuf[idx + 1] * r_coeff;
      }

      // 3. FFT
      // Window
      for (int i = 0; i < ALineSize; ++i) {
        fftBuf.in[i] = win[i] * linearKFringe[i];
      }
      fft.forward(fftBuf.in, fftBuf.out);

      // 4. Copy result into image
      T *outptr = reinterpret_cast<T *>(mat.ptr(j));
      logCompress<T>(outptr, imageDepth, fftBuf.out, contrast, brightness);
    }
  });

  mat = mat.t();

  // Distortion correction and resize to theoretical aline number
  {
    TimeIt timeit;

    size_t theoreticalALines = nLines;
    if (nLines == 2500) {
      // theoreticalALines = 2234;
      // Don't need distortion correction for the ex vivo probe.
    } else if (nLines == 2200) {
      theoreticalALines = 2000;

      const cv::Size targetSize(theoreticalALines, mat.rows);
      const int distOffset = getDistortionOffset(mat, theoreticalALines,
                                                 nLines - theoreticalALines);
      cv::resize(mat(cv::Rect(0, 0, theoreticalALines + distOffset, mat.rows)),
                 mat, targetSize);
    }

    // fmt::println("Distortion correction elapsed: {} ms", timeit.get_ms());
  }

  // Align Bscans
  {
    TimeIt timeit;
    static cv::Mat_<T> prevMat;
    if (prevMat.cols == mat.cols && prevMat.rows == mat.rows) {
      int alignOffset = std::round(cvMod::phaseCorrelate(prevMat, mat).x);
      circshift(mat, alignOffset);
    }
    mat.copyTo(prevMat);

    // fmt::println("Align correction elapsed: {} ms", timeit.get_ms());
  }

  cv::Mat_<uint8_t> outmat;
  mat.convertTo(outmat, CV_8U);
  return mat;
}

inline void makeRadialImage(const cv::Mat_<uint8_t> &in, cv::Mat_<uint8_t> &out,
                            int padTop = 625) {

  const int dim = std::min(in.rows, in.cols);

  // const cv::Size dsize{dim, dim};
  // const double radius = dim / 2;
  // const cv::Point2f center(radius, radius);

  const cv::Size dsize{dim * 2, dim * 2};
  const double radius = dim;
  const cv::Point2f center(radius, radius);

  const int flags = cv::WARP_FILL_OUTLIERS + cv::WARP_INVERSE_MAP;

  if (padTop != 0) {
    cv::copyMakeBorder(in, out, padTop, 0, 0, 0, cv::BORDER_CONSTANT);
    cv::warpPolar(out.t(), out, dsize, center, radius,
                  flags); // linear Polar, 3
  } else {
    cv::warpPolar(in.t(), out, dsize, center, radius,
                  flags); // linear Polar, 3
  }
}

} // namespace OCT

// NOLINTEND(*-pointer-arithmetic, *-magic-numbers)