#include "DAQ.hpp"
#include <qlogging.h>

#ifdef OCTGUI_HAS_ALAZAR

#include "datetime.hpp"
#include "defer.h"
#include "timeit.hpp"
#include <AlazarApi.h>
#include <AlazarCmd.h>
#include <AlazarError.h>
#include <QDebug>
#include <fmt/core.h>
#include <ios>
#include <sstream>
#include <string>

// NOLINTBEGIN(*-do-while)

namespace {

// Wrapper error handling for ATS-SDK call
// Need to define 1 variable before call:
// RETURN_CODE ret{ApiSuccess};
// bool success{true};
// NOLINTNEXTLINE(*-macro-usage)
#define ALAZAR_CALL(fn)                                                        \
  {                                                                            \
    ret = fn;                                                                  \
    success = ret == ApiSuccess;                                               \
    if (!success) {                                                            \
      qCritical("Error: %s failed -- %s\n", #fn, AlazarErrorToText(ret));      \
    }                                                                          \
  }

// NOLINTNEXTLINE(*-macro-usage)
#define RETURN_BOOL_IF_FAIL()                                                  \
  if (ret != ApiSuccess) {                                                     \
    return false;                                                              \
  }

// NOLINTNEXTLINE(*-macro-usage)
#define BREAK_IF_FAIL()                                                        \
  if (ret != ApiSuccess) {                                                     \
    return false;                                                              \
  }

std::string GetSystemInfo(U32 systemId);
std::string GetBoardInfo(U32 systemId, U32 boardId);
bool IsPcieDevice(HANDLE handle);
const char *BoardTypeToText(int boardType);
bool HasCoprocessorFPGA(HANDLE handle);

std::string GetSystemInfo(U32 systemId) {
  std::stringstream ss;

  U32 boardCount = AlazarBoardsInSystemBySystemID(systemId);
  if (boardCount == 0) {
    ss << "Error: No boards found in system.\n";
    return ss.str();
  }

  HANDLE handle = AlazarGetSystemHandle(systemId);
  if (handle == nullptr) {
    ss << "Error: AlazarGetSystemHandle system failed.\n";
    return ss.str();
  }

  int boardType = AlazarGetBoardKind(handle);
  if (boardType == ATS_NONE || boardType >= ATS_LAST) {
    ss << fmt::format("Error: Unknown board type {}\n", boardType);
    return ss.str();
  }

  U8 driverMajor{};
  U8 driverMinor{};
  U8 driverRev{};
  auto retCode = AlazarGetDriverVersion(&driverMajor, &driverMinor, &driverRev);
  if (retCode != ApiSuccess) {
    ss << fmt::format("Error: AlazarGetDriverVersion failed -- {}\n",
                      AlazarErrorToText(retCode));
    return ss.str();
  }

  ss << fmt::format("System ID = {}\n", systemId);
  ss << fmt::format("Board type = {}\n", BoardTypeToText(boardType));
  ss << fmt::format("Board count = {}\n", boardCount);
  ss << fmt::format("Driver version = {}.{}.{}\n", driverMajor, driverMinor,
                    driverRev);

  // Display informataion about each board in this board system

  for (U32 boardId = 1; boardId <= boardCount; boardId++) {
    ss << "\n" << GetBoardInfo(systemId, boardId);
  }
  return ss.str();
}

std::string GetBoardInfo(U32 systemId, U32 boardId) {
  HANDLE handle = AlazarGetBoardBySystemID(systemId, boardId);
  if (handle == nullptr) {
    return fmt::format("Error: Open systemId {} boardId {} failed\n", systemId,
                       boardId);
  }

  RETURN_CODE ret{ApiSuccess};
  U32 samplesPerChannel{};
  BYTE bitsPerSample{};
  ret = AlazarGetChannelInfo(handle, &samplesPerChannel, &bitsPerSample);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarGetChannelInfo failed -- {}\n",
                       AlazarErrorToText(ret));
  }

  U32 aspocType{};
  ret = AlazarQueryCapability(handle, ASOPC_TYPE, 0, &aspocType);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                       AlazarErrorToText(ret));
  }

  BYTE fpgaMajor{};
  BYTE fpgaMinor{};
  ret = AlazarGetFPGAVersion(handle, &fpgaMajor, &fpgaMinor);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarGetFPGAVersion failed -- {}.\n",
                       AlazarErrorToText(ret));
  }

  BYTE cpldMajor{};
  BYTE cpldMinor{};
  ret = AlazarGetCPLDVersion(handle, &cpldMajor, &cpldMinor);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarGetCPLDVersion failed -- {}.\n",
                       AlazarErrorToText(ret));
  }

  U32 serialNumber{};
  ret = AlazarQueryCapability(handle, GET_SERIAL_NUMBER, 0, &serialNumber);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                       AlazarErrorToText(ret));
  }

  U32 latestCalDate{};
  ret = AlazarQueryCapability(handle, GET_LATEST_CAL_DATE, 0, &latestCalDate);
  if (ret != ApiSuccess) {
    return fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                       AlazarErrorToText(ret));
  }

  std::stringstream ss;
  ss << fmt::format("System ID = {}\n", systemId);
  ss << fmt::format("Board ID = {}\n", boardId);
  ss << fmt::format("Serial number = {}\n", serialNumber);
  ss << fmt::format("Bits per sample = {}\n", bitsPerSample);
  ss << fmt::format("Max samples per channel = {}\n", samplesPerChannel);
  ss << fmt::format("FPGA version = {}.{}\n", fpgaMajor, fpgaMinor);
  ss << fmt::format("CPLD version = {}.{}\n", cpldMajor, cpldMinor);
  ss << fmt::format("ASoPC signature = {:x}\n", aspocType);
  ss << fmt::format("Latest calibration date = {}\n", latestCalDate);

  if (HasCoprocessorFPGA(handle)) {
    // Display co-processor FPGA device type

    U32 deviceType{};
    ret = AlazarQueryCapability(handle, GET_CPF_DEVICE, 0, &deviceType);
    if (ret != ApiSuccess) {
      ss << fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                        AlazarErrorToText(ret));
      return ss.str();
    }

    const char *deviceName{};
    switch (deviceType) {
    case CPF_DEVICE_EP3SL50:
      deviceName = "EP3SL50";
      break;
    case CPF_DEVICE_EP3SE260:
      deviceName = "EP3SL260";
      break;
    default:
      deviceName = "Unknown";
      break;
    }
    ss << fmt::format("CPF Device = {}\n", deviceName);
  }

  if (IsPcieDevice(handle)) {
    // Display PCI Express link information

    U32 linkSpeed{};
    ret = AlazarQueryCapability(handle, GET_PCIE_LINK_SPEED, 0, &linkSpeed);
    if (ret != ApiSuccess) {
      ss << fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                        AlazarErrorToText(ret));
    }

    U32 linkWidth{};
    ret = AlazarQueryCapability(handle, GET_PCIE_LINK_WIDTH, 0, &linkWidth);
    if (ret != ApiSuccess) {
      ss << fmt::format("Error: AlazarQueryCapability failed -- {}.\n",
                        AlazarErrorToText(ret));
    }

    ss << fmt::format("PCIe link speed = {} Gbps\n",
                      2.5 * linkSpeed); // NOLINT
    ss << fmt::format("PCIe link width = {} lanes\n", linkWidth);

    float fpgaTemperature_degreesC{};
    ret = AlazarGetParameterUL(handle, CHANNEL_ALL, GET_FPGA_TEMPERATURE,
                               (U32 *)&fpgaTemperature_degreesC);
    if (ret != ApiSuccess) {
      ss << fmt::format("Error: AlazarGetParameterUL failed -- {}.\n",
                        AlazarErrorToText(ret));
      return ss.str();
    }

    ss << fmt::format("FPGA temperature = {} C\n", fpgaTemperature_degreesC);
  }

  return ss.str();
}

// Return true if board has PCIe host bus interface
bool IsPcieDevice(HANDLE handle) {
  U32 boardType = AlazarGetBoardKind(handle);
  return boardType >= ATS9462;
}

// Convert board type Id to text
const char *BoardTypeToText(int boardType) {
  // NOLINTNEXTLINE(*-default-case)
  switch (boardType) {
  case ATS850:
    return "ATS850";
  case ATS310:
    return "ATS310";
  case ATS330:
    return "ATS330";
  case ATS855:
    return "ATS855";
  case ATS315:
    return "ATS315";
  case ATS335:
    return "ATS335";
  case ATS460:
    return "ATS460";
  case ATS860:
    return "ATS860";
  case ATS660:
    return "ATS660";
  case ATS665:
    return "ATS665";
  case ATS9462:
    return "ATS9462";
  case ATS9870:
    return "ATS9870";
  case ATS9350:
    return "ATS9350";
  case ATS9325:
    return "ATS9325";
  case ATS9440:
    return "ATS9440";
  case ATS9351:
    return "ATS9351";
  case ATS9850:
    return "ATS9850";
  case ATS9625:
    return "ATS9625";
  case ATS9626:
    return "ATS9626";
  case ATS9360:
    return "ATS9360";
  case AXI9870:
    return "AXI9870";
  case ATS9370:
    return "ATS9370";
  case ATS9373:
    return "ATS9373";
  case ATS9416:
    return "ATS9416";
  case ATS9637:
    return "ATS9637";
  case ATS9120:
    return "ATS9120";
  case ATS9371:
    return "ATS9371";
  case ATS9130:
    return "ATS9130";
  case ATS9352:
    return "ATS9352";
  case ATS9353:
    return "ATS9353";
  case ATS9453:
    return "ATS9453";
  case ATS9146:
    return "ATS9146";
  case ATS9437:
    return "ATS9437";
  case ATS9618:
    return "ATS9618";
  case ATS9358:
    return "ATS9358";
  case ATS9872:
    return "ATS9872";
  case ATS9628:
    return "ATS9628";
  case ATS9364:
    return "ATS9364";
  }
  return "?";
}

// Return true if board has coprocessor FPGA
bool HasCoprocessorFPGA(HANDLE handle) {
  U32 boardType = AlazarGetBoardKind(handle);
  return (boardType == ATS9625 || boardType == ATS9626);
}

} // namespace

// NOLINTEND(*-do-while)

namespace OCT::daq {

std::string getDAQInfo() {
  std::stringstream ss;

  U8 sdkMajor{};
  U8 sdkMinor{};
  U8 sdkRevision{};
  auto retCode = AlazarGetSDKVersion(&sdkMajor, &sdkMinor, &sdkRevision);
  if (retCode != ApiSuccess) {
    return "";
  }

  auto systemCount = AlazarNumOfSystems();

  ss << fmt::format("Alazar SDK version = {}.{}.{}\n", sdkMajor, sdkMinor,
                    sdkRevision);
  ss << fmt::format("Alazar system count = {}\n", systemCount);

  if (systemCount < 1) {
    ss << fmt::format("No Alazar system found.\n");
  }

  else {
    for (U32 systemId = 1; systemId <= systemCount; ++systemId) {
      ss << GetSystemInfo(systemId);
    }
  }

  return ss.str();
}

bool DAQ::initHardware() noexcept {
  RETURN_CODE ret{ApiSuccess};
  bool success{true};

  board = AlazarGetBoardBySystemID(1, 1);
  if (board == nullptr) {
    m_errMsg = "Failed to initialize Alazar board.";
  }

  // Samples per sec
  samplesPerSec = 180e6;

  // Select clock parameters required to generate this sample rate
  ALAZAR_CALL(AlazarSetCaptureClock(board, INTERNAL_CLOCK, SAMPLE_RATE_180MSPS,
                                    CLOCK_EDGE_RISING, 0));
  RETURN_BOOL_IF_FAIL();

  // Channel A
  channelMask = CHANNEL_A;
  ALAZAR_CALL(AlazarInputControl(board, CHANNEL_A, DC_COUPLING,
                                 INPUT_RANGE_PM_2_V, IMPEDANCE_50_OHM));
  RETURN_BOOL_IF_FAIL();

  // Channel B (unused)
  ALAZAR_CALL(AlazarInputControl(board, CHANNEL_B, DC_COUPLING,
                                 INPUT_RANGE_PM_800_MV, IMPEDANCE_50_OHM));
  RETURN_BOOL_IF_FAIL();

  // External trigger channel (5V DC Coupling)
  ALAZAR_CALL(AlazarSetExternalTrigger(board, DC_COUPLING, ETR_5V));
  RETURN_BOOL_IF_FAIL();

  // Trigger op
  ALAZAR_CALL(AlazarSetTriggerOperation(board, TRIG_ENGINE_OP_J, TRIG_ENGINE_J,
                                        TRIG_EXTERNAL, TRIGGER_SLOPE_NEGATIVE,
                                        160, TRIG_ENGINE_K, TRIG_DISABLE,
                                        TRIGGER_SLOPE_POSITIVE, 128));
  RETURN_BOOL_IF_FAIL();

  ALAZAR_CALL(AlazarSetTriggerDelay(board, 0));
  RETURN_BOOL_IF_FAIL();

  ALAZAR_CALL(AlazarSetTriggerTimeOut(board, 0));
  RETURN_BOOL_IF_FAIL();

  ALAZAR_CALL(AlazarConfigureAuxIO(board, AUX_OUT_TRIGGER, 0));
  return success;
}

bool DAQ::prepareAcquisition(int maxBuffersToAcquire) noexcept {
  m_errMsg.clear();

  if (m_saveData) {
    const auto fname =
        fmt::format("OCT{}_{}.bin", datetime::datetimeFormat("%Y%m%d%H%M%S"),
                    recordsPerBuffer);
    m_lastBinfile = m_savedir / fname;
    m_fs = std::fstream(m_lastBinfile, std::ios::out | std::ios::binary);

    if (!m_fs.is_open()) {
      m_errMsg =
          "Failed to open binfile for writing: " + m_lastBinfile.string();
      return false;
    }
  } else {
    m_lastBinfile.clear();
  }

  // Prime the board

  uint8_t bitsPerSample{};
  U32 maxSamplesPerChannel{};

  RETURN_CODE ret{ApiSuccess};
  bool success{true};

  ALAZAR_CALL(
      AlazarGetChannelInfo(board, &maxSamplesPerChannel, &bitsPerSample));
  RETURN_BOOL_IF_FAIL();

  const auto channelCount = 1;

  const auto bytesPerSample = (float)((bitsPerSample + 7) / 8);
  const U32 bytesPerRecord = (U32)(bytesPerSample * recordSize + 0.5);
  const U32 bytesPerBuffer = bytesPerRecord * recordsPerBuffer * channelCount;

  // Free all memory allocated
  for (auto &buf : buffers) {
    if (buf.size() > 0) {
      AlazarFreeBufferU16(board, buf.data());
      buf = {};
    }
  }

  for (auto &buf : buffers) {
    // Allocate page aligned memory
    auto *ptr = AlazarAllocBufferU16(board, bytesPerBuffer);
    if (ptr == nullptr) {
      qCritical("Error: Alloc %u bytes failed", bytesPerBuffer);
      return false;
    }
    buf = std::span(ptr, bytesPerBuffer / 2);
    qDebug("Allocated %d bytes of memory", bytesPerBuffer);
  }

  // Configure the record size
  ALAZAR_CALL(AlazarSetRecordSize(board, 0, recordSize));
  RETURN_BOOL_IF_FAIL();

  return success;
}

bool DAQ::acquire(int buffersToAcquire,
                  const std::function<void()> &callback) noexcept {
  shouldStopAcquiring = false;
  acquiringData = true;
  defer { acquiringData = false; };

  RETURN_CODE ret{ApiSuccess};
  bool success{true};

  // Configure the board to make an NPT AutoDMA acquisition
  U32 recordsPerAcquisition = recordsPerBuffer * buffersToAcquire;
  U32 admaFlags =
      ADMA_EXTERNAL_STARTCAPTURE | ADMA_NPT | ADMA_FIFO_ONLY_STREAMING;
  ALAZAR_CALL(AlazarBeforeAsyncRead(board, channelMask, 0, recordSize,
                                    recordsPerBuffer, recordsPerAcquisition,
                                    admaFlags));
  RETURN_BOOL_IF_FAIL();

  // Add the buffers to a list of buffers available to be filled by the board
  for (auto &buf : buffers) {
    const auto bytesPerBuffer = buf.size() * sizeof(uint16_t);
    ALAZAR_CALL(AlazarPostAsyncBuffer(board, buf.data(), bytesPerBuffer));
    RETURN_BOOL_IF_FAIL();
  }

  // Arm the board system to wait for a trigger event to begin acquisition
  defer {
    // Abort the acquisition at the end
    ALAZAR_CALL(AlazarAbortAsyncRead(board));
  };

  ALAZAR_CALL(AlazarStartCapture(board));
  RETURN_BOOL_IF_FAIL();

  uint32_t buffersCompleted = 0;
  uint32_t bufferIdx = 0;
  while (success && !shouldStopAcquiring &&
         buffersCompleted < buffersToAcquire) {
    if (callback) {
      callback();
    }

    bufferIdx = buffersCompleted % buffers.size();
    auto &buf = buffers[bufferIdx]; // NOLINT(*-array-index)
    const auto bytesPerBuffer = buf.size() * sizeof(uint16_t);

    constexpr uint32_t timeout_ms = 1000;
    ret = AlazarWaitAsyncBufferComplete(board, buf.data(), timeout_ms);

    success = false;
    switch (ret) {
    case ApiSuccess: {
      success = true;
      buffersCompleted++;

      m_ringBuffer->produce_nolock(
          [&, this](std::shared_ptr<OCTData<Float>> &dat) {
            dat->i = buffersCompleted - 1;

            // Copy data from alazar buffer to ring buffer
            auto &fringe = dat->fringe;
            if (fringe.size() < buf.size()) {
              fringe.resize(buf.size());
            }
            std::copy(buf.data(), buf.data() + buf.size(), fringe.data());
          });

      // Save
      if (m_fs.is_open()) {
        try {
          TimeIt timeit;
          m_fs.write((char *)buf.data(), bytesPerBuffer);

          const auto time_ms = timeit.get_ms();
          const auto speed_MBps = bytesPerBuffer * 1e-3 / time_ms;

          qInfo("Wrote %d bytes to file in %f ms (%f MB/s)", bytesPerBuffer,
                time_ms, speed_MBps);

        } catch (std::ios_base::failure &e) {
          qCritical("Error: write buffer %u failed -- %u", buffersCompleted,
                    GetLastError());
          success = false;
        }
      }
    } break;

    case ApiWaitTimeout:
      m_errMsg = "DAQ: AlazarWaitAsyncBufferComplete timeout. Please make sure "
                 "the trigger is connected.";
      qCritical() << m_errMsg;
      break;

    case ApiBufferOverflow:
      m_errMsg =
          "DAQ: AlazarWaitAsyncBufferComplete buffer overflow. The data "
          "acquisition rate is higher than the transfer rate from on-board "
          "memory to host memory.";
      qCritical() << m_errMsg;
      break;

    case ApiBufferNotReady:
      m_errMsg = "DAQ: AlazarWaitAsyncBufferComplete (573) buffer not ready. "
                 "The buffer passed as argument is not ready to be called with "
                 "this API. ";
      qCritical() << m_errMsg;
      break;

    default:
      m_errMsg = fmt::format(
          "DAQ: AlazarWaitAsyncBufferComplete returned unknown code {}",
          static_cast<uint32_t>(ret));
      qCritical() << m_errMsg;
    }

    if (!success) {
      break;
    }

    ALAZAR_CALL(AlazarPostAsyncBuffer(board, buf.data(), bytesPerBuffer));
  }

  return success;
}

DAQ::~DAQ() {
  for (auto &buf : buffers) {
    if (buf.size() >= 0) {
      AlazarFreeBufferU16(board, buf.data());
      buf = {};
    }
  }
};
} // namespace OCT::daq

#endif