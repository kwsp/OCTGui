#include "MainWindow.hpp"
#include "ExportSettings.hpp"
#include "FileIO.hpp"
#include "FrameController.hpp"
#include "OCTRecon.hpp"
#include "ReconWorker.hpp"
#include "strOps.hpp"
#include "timeit.hpp"
#include <QDockWidget>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMimeData>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
#include <filesystem>
#include <fmt/format.h>
#include <opencv2/opencv.hpp>
#include <qnamespace.h>

namespace OCT {

MainWindow::MainWindow()
    : m_menuFile(menuBar()->addMenu("&File")),
      m_menuView(menuBar()->addMenu("&View")), m_imageDisplay(new ImageDisplay),
      m_frameController(new FrameController),
      m_exportSettingsWidget(new ExportSettingsWidget) {

  // Configure MainWindow
  // --------------------
  // Enable status bar
  statusBar();

  // Enable drag and drop
  setAcceptDrops(true);

  // Define GUI
  // ----------

  // Central widget
  // --------------
  auto *centralWidget = new QWidget;
  setCentralWidget(centralWidget);
  auto *centralLayout = new QStackedLayout;
  centralWidget->setLayout(centralLayout);
  centralLayout->addWidget(m_imageDisplay);
  m_imageDisplay->overlay()->setModality("OCT");

  // Dock widgets
  // ------------
  {
    auto *dock = new QDockWidget("Frames");
    this->addDockWidget(Qt::TopDockWidgetArea, dock);
    m_menuView->addAction(dock->toggleViewAction());

    dock->setWidget(m_frameController);

    connect(m_frameController, &FrameController::posChanged, this,
            &MainWindow::loadFrame);

    menuBar()->addMenu(m_frameController->menu());
  }
  {
    auto *dock = new QDockWidget("Export settings");
    this->addDockWidget(Qt::TopDockWidgetArea, dock);
    m_menuView->addAction(dock->toggleViewAction());

    dock->setWidget(m_exportSettingsWidget);
    menuBar()->addMenu(m_exportSettingsWidget->menu());
  }
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
  const auto *mimeData = event->mimeData();
  if (mimeData->hasUrls()) {
    const auto &urls = mimeData->urls();

    if (urls.size() == 1) {
      // Accept folder drop
      const auto filePath = urls[0].toLocalFile();
      QFileInfo fileInfo(filePath);

      if (fileInfo.isDir()) {
        event->acceptProposedAction();
      }
    }
  }
}

void MainWindow::dropEvent(QDropEvent *event) {
  const auto *mimeData = event->mimeData();
  if (mimeData->hasUrls()) {
    const auto &urls = mimeData->urls();
    const auto stdPath = toPath(urls[0].toLocalFile());

    if (fs::is_directory(stdPath)) {

      auto dirName = getDirectoryName(stdPath);
      toLower_(dirName);
      if (dirName.find("calib") != std::string::npos) {
        // Check if it is a calibration directory (a directory that contains
        // "SSOCTBackground.txt" and "SSOCTCalibration180MHZ.txt")

        // Load calibration files
        tryLoadCalibDirectory(stdPath);
      } else {
        // Load Dat sequence directory
        tryLoadDatDirectory(stdPath);
      }
    }
  }
}

void MainWindow::tryLoadCalibDirectory(const fs::path &calibDir) {
  const auto backgroundFile = calibDir / "SSOCTBackground.txt";
  const auto phaseFile = calibDir / "SSOCTCalibration180MHZ.txt";

  constexpr int statusTimeoutMs = 5000;

  if (fs::exists(backgroundFile) && fs::exists(phaseFile)) {
    m_calib = std::make_unique<Calibration<Float>>(DatReader::ALineSize,
                                                   backgroundFile, phaseFile);

    ;
    const auto msg =
        QString("Loaded calibration files from ") + toQString(calibDir);

    statusBar()->showMessage(msg, statusTimeoutMs);
  } else {
    // TODO more logging
    const auto msg = QString("Failed to load calibration files from ") +
                     QString::fromStdString(calibDir.string());
    statusBar()->showMessage(msg, statusTimeoutMs);
  }
}

void MainWindow::tryLoadDatDirectory(const fs::path &dir) {
  m_datReader = std::make_unique<DatReader>(dir);

  constexpr int statusTimeoutMs = 5000;
  if (m_datReader->ok()) {

    // Update status bar
    const auto msg = QString("Loaded dat directory ") + toQString(dir);
    statusBar()->showMessage(msg, statusTimeoutMs);

    // Update image overlay sequence label
    m_imageDisplay->overlay()->setSequence(
        QString::fromStdString(m_datReader->seq));
    m_imageDisplay->overlay()->setProgress(0, m_datReader->size());

    // Update frame controller slider
    m_frameController->setSize(m_datReader->size());
    m_frameController->setPos(0);

    // Set export directory
    m_exportDir = toPath(QStandardPaths::writableLocation(
                      QStandardPaths::DesktopLocation)) /
                  m_datReader->seq;
    fs::create_directories(m_exportDir);

    // Load the first frame
    loadFrame(0);

  } else {
    const auto msg = QString("Failed to load dat directory ") + toQString(dir);
    statusBar()->showMessage(msg, statusTimeoutMs);

    m_exportDir.clear();
    m_datReader = nullptr;
  }
}

void MainWindow::loadFrame(size_t i) {
  if (m_calib != nullptr) {
    // Recon 1 frame
    i = std::clamp<size_t>(i, 0, m_datReader->size());

    TimeIt timeit;

    std::vector<uint16_t> fringe(m_datReader->samplesPerFrame());
    auto err = m_datReader->read(i, 1, fringe);
    if (err) {
      auto msg = fmt::format("While loading {}/{}, got {}", i,
                             m_datReader->size(), *err);
      statusBar()->showMessage(QString::fromStdString(msg));
      return;
    }

    cv::Mat_<uint8_t> img;

    float elapsedRecon{};
    {
      TimeIt timeitRecon;
      img = reconBscan<Float>(*m_calib, fringe, m_datReader->ALineSize);
      elapsedRecon = timeitRecon.get_ms();
    }

    cv::Mat_<uint8_t> imgRadial;
    float elapsedRadial{};
    {
      TimeIt timeit;
      makeRadialImage(img, imgRadial);
      elapsedRadial = timeit.get_ms();
    }

    const auto &settings = m_exportSettingsWidget->settings();
    if (settings.saveImages) {
      {
        auto outpath = m_exportDir / fmt::format("rect-{:03}.tiff", i);
        cv::imwrite(outpath.string(), img);
      }
      {
        auto outpath = m_exportDir / fmt::format("radial-{:03}.tiff", i);
        cv::imwrite(outpath.string(), imgRadial);
      }
    }

    cv::Mat_<uint8_t> combined(imgRadial.rows, imgRadial.cols + img.cols);
    // Copy radial to left side
    imgRadial.copyTo(combined(cv::Rect(0, 0, imgRadial.cols, imgRadial.rows)));
    // Copy rect to top right
    img.copyTo(combined(cv::Rect(imgRadial.cols, 0, img.cols, img.rows)));

    // Clear bottom right
    combined(
        cv::Rect(imgRadial.cols, img.rows, img.cols, combined.rows - img.rows))
        .setTo(0);

    // Update image display
    m_imageDisplay->imshow(matToQPixmap(combined));
    m_imageDisplay->overlay()->setProgress(i, m_datReader->size());

    auto elapsedTotal = timeit.get_ms();
    auto msg =
        fmt::format("Loaded frame {}/{}, recon {:.3f} ms, total {:.3f} ms", i,
                    m_datReader->size(), elapsedRecon, elapsedTotal);
    statusBar()->showMessage(QString::fromStdString(msg));
  } else {
    statusBar()->showMessage(
        "Please load calibration files first by dropping a directory "
        "containing the background and phase files into the GUI.");
  }
}

} // namespace OCT