#include "MainWindow.hpp"
#include "AcquisitionController.hpp"
#include "DAQ.hpp"
#include "ExportSettings.hpp"
#include "FileIO.hpp"
#include "FrameController.hpp"
#include "ImageDisplay.hpp"
#include "MotorDriver.hpp"
#include "OCTRecon.hpp"
#include "OCTReconParamsController.hpp"
#include "ReconWorker.hpp"
#include "datetime.hpp"
#include "strOps.hpp"
#include "timeit.hpp"
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QStatusBar>
#include <QVBoxLayout>
#include <Qt>
#include <algorithm>
#include <fftconv/aligned_vector.hpp>
#include <filesystem>
#include <fmt/format.h>
#include <fmt/std.h>
#include <opencv2/opencv.hpp>

namespace OCT {

MainWindow::MainWindow()
    : m_menuFile(menuBar()->addMenu("&File")),
      m_menuView(menuBar()->addMenu("&View")), m_imageDisplay(new ImageDisplay),
      m_frameController(new FrameController),
      m_reconParamsController(new OCTReconParamsController),
      m_motorDriver(new MotorDriver),
      m_ringBuffer(std::make_shared<RingBuffer<OCTData<Float>>>()),
      m_worker(new ReconWorker(m_ringBuffer, DatFileReader::ALineSize,
                               m_imageDisplay)),

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

  // OCTReconParamsController
  {
    auto *dock = new QDockWidget("OCT Recon Params");
    this->addDockWidget(Qt::TopDockWidgetArea, dock);
    m_menuView->addAction(dock->toggleViewAction());
    dock->toggleViewAction()->setShortcut({Qt::CTRL | Qt::SHIFT | Qt::Key_P});

    dock->toggleViewAction();

    dock->setWidget(m_reconParamsController);
  }

  // Export settings
  {
    auto *dock = new QDockWidget("Export settings");
    this->addDockWidget(Qt::TopDockWidgetArea, dock);
    m_menuView->addAction(dock->toggleViewAction());

    dock->setWidget(m_exportSettingsWidget);
    dock->hide();
    menuBar()->addMenu(m_exportSettingsWidget->menu());
  }

  // Motor Driver
  auto *motorDock = new QDockWidget("Motor control");
  addDockWidget(Qt::TopDockWidgetArea, motorDock);
  m_menuView->addAction(motorDock->toggleViewAction());
  motorDock->setWidget(m_motorDriver);

#ifdef OCTGUI_HAS_ALAZAR
  m_acqController = new AcquisitionController(m_ringBuffer, m_motorDriver);

  // Acquisition
  {

    auto *dock = new QDockWidget("Acquisition control");
    addDockWidget(Qt::TopDockWidgetArea, dock);
    m_menuView->addAction(dock->toggleViewAction());

    dock->setWidget(m_acqController);

    // Acq signals
    connect(&m_acqController->controller(),
            &AcquisitionControllerObj::sigAcquisitionStarted, this, [this]() {
              // Set reconWorker to live (no block) mode
              m_worker->setNoBlockMode(true);

              // Clear overlay progress
              m_imageDisplay->overlay()->setProgress(0, 0);
            });

    connect(&m_acqController->controller(),
            &AcquisitionControllerObj::sigAcquisitionFinished,
            [this](const QString &qpath) {
              // Return to blocking mode where every incoming frame is processed
              m_worker->setNoBlockMode(false);
              tryLoadBinfile(qpath);
            });

    // Updated background
    connect(m_acqController, &AcquisitionController::sigUpdatedBackground, this,
            [this]() {
              const auto defaultDataDirp = toPath(defaultDataDir);
              const auto newPath =
                  defaultDataDirp /
                  fmt::format("OCTcalib {}",
                              datetime::datetimeFormat("%Y%m%d%H%M%S"));

              const auto defaultCalibDirp = toPath(defaultCalibDir);
              m_calib->saveToNewCalibDir(defaultCalibDirp);
              m_calib->saveToNewCalibDir(newPath);

              const auto msg = fmt::format(
                  "Saved new calibration files to {} and default calib dir {}",
                  newPath, defaultCalibDirp);
              statusBarMessage(QString::fromStdString(msg));
            });
  }

  motorDock->show();

#else

  // No DAQ, hide motor control by default
  motorDock->hide();

#endif

  // Other actions
  // -------------
  m_menuView->addAction(m_imageDisplay->actResetZoom());

  {
    auto *act = new QAction("Import calibration directory");
    m_menuFile->addAction(act);

    connect(act, &QAction::triggered, this, [this]() {
      const QString filename = QFileDialog::getExistingDirectory(
          this, "Import calibration directory", defaultDataDir);

      this->tryLoadCalibDirectory(filename);
    });
  }

  {
    auto *act = new QAction("Open DAT data directory");
    m_menuFile->addAction(act);
    act->setShortcut({Qt::CTRL | Qt::SHIFT | Qt::Key_O});

    connect(act, &QAction::triggered, this, [this]() {
      const QString filename = QFileDialog::getExistingDirectory(
          this, "Import DAT data directory", defaultDataDir);
      this->tryLoadDatDirectory(filename);
    });
  }

  {
    auto *act = new QAction("Open a single bin file");
    m_menuFile->addAction(act);
    act->setShortcut({Qt::CTRL | Qt::Key_O});

    connect(act, &QAction::triggered, this, [this]() {
      const QString filename = QFileDialog::getOpenFileName(
          this, "Select a bin file", defaultDataDir, "Binfile (*.bin *.dat)");
      this->tryLoadBinfile(filename);
    });
  }

  // Recon worker thread
  {
    m_worker->moveToThread(&m_workerThread);
    connect(&m_workerThread, &QThread::finished, m_worker,
            &ReconWorker::deleteLater);
    connect(m_worker, &ReconWorker::statusMessage, this,
            &MainWindow::statusBarMessage);
    m_workerThread.start();
    QMetaObject::invokeMethod(m_worker, &ReconWorker::start);
  }

  // DAQ Info
  {
#ifdef OCTGUI_HAS_ALAZAR
    auto *actDaqInfo = new QAction("DAQ Info");
    connect(actDaqInfo, &QAction::triggered, [this]() {
      const auto daqInfo = daq::getDAQInfo();
      QMessageBox::about(this, "DAQ Info", QString::fromStdString(daqInfo));
    });
    m_menuFile->addAction(actDaqInfo);
#endif
  }

  // Auto load calibration data if exists at C:/Data/OCTcalib
  tryLoadCalibDirectory(defaultCalibDir);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event) {
  const auto *mimeData = event->mimeData();
  if (mimeData->hasUrls()) {
    const auto &urls = mimeData->urls();

    if (urls.size() == 1) {
      // Accept single drop
      event->acceptProposedAction();
    }
  }
}

void MainWindow::dropEvent(QDropEvent *event) {
  const auto *mimeData = event->mimeData();
  if (mimeData->hasUrls()) {
    const auto &urls = mimeData->urls();
    const auto qpath = urls[0].toLocalFile();
    const auto path = toPath(qpath);

    if (fs::is_directory(path)) {
      auto dirName = getDirectoryName(path);
      toLower_(dirName);
      if (dirName.find("calib") != std::string::npos) {
        // Check if it is a calibration directory (a directory that contains
        // "SSOCTBackground.txt" and "SSOCTCalibration180MHZ.txt")

        // Load calibration files
        tryLoadCalibDirectory(qpath);
      } else {
        // Load Dat sequence directory
        tryLoadDatDirectory(qpath);
      }
    } else {
      tryLoadBinfile(qpath);
    }
  }
}

void MainWindow::tryLoadCalibDirectory(const QString &calibDir) {
  constexpr int statusTimeoutMs = 10000;
  m_calib = Calibration<Float>::fromCalibDir(DatFileReader::ALineSize,
                                             toPath(calibDir));

  if (m_calib != nullptr) {
    const auto msg = QString("Loaded calibration files from ") + calibDir;
    statusBar()->showMessage(msg, statusTimeoutMs);

    m_worker->setCalibration(m_calib);

#ifdef OCTGUI_HAS_ALAZAR
    m_acqController->setCalibration(m_calib);
#endif

    if (m_datReader.ok()) {
      loadFrame(m_frameController->pos());
    }

  } else {
    const auto msg =
        QString("Failed to load calibration files from ") + calibDir;
    statusBar()->showMessage(msg, statusTimeoutMs);
  }
}

void MainWindow::tryLoadDatDirectory(const QString &qdir) {
  m_datReader = DatFileReader::readDatDirectory(toPath(qdir));

  constexpr int statusTimeoutMs = 5000;
  if (m_datReader.ok()) {
    afterDatReaderReady();

    // Update status bar
    statusBar()->showMessage("Loaded dat directory " + qdir);

    // Load the first frame
    loadFrame(0);

  } else {
    const auto msg = QString("Failed to load dat directory ") + qdir;
    statusBar()->showMessage(msg, statusTimeoutMs);

    m_exportSettingsWidget->setExportDir({});
    m_datReader = {};
  }
}

void MainWindow::tryLoadBinfile(const QString &qpath) {
  m_datReader = DatFileReader::readBinFile(toPath(qpath));

  constexpr int statusTimeoutMs = 5000;
  if (m_datReader.ok()) {
    afterDatReaderReady();

    // Update status bar
    statusBarMessage("Loaded bin file " + qpath);

    // Load the first frame
    loadFrame(0);

  } else {
    statusBarMessage("Failed to load bin file " + qpath);

    m_exportSettingsWidget->setExportDir({});
    m_datReader = {};
  }
};

void MainWindow::loadFrame(size_t i) {
  if (m_calib != nullptr && m_datReader.ok()) {
    TimeIt timeit;

    // Recon
    const auto params = m_reconParamsController->params();
    if (params.additionalOffset != 0) {
      m_reconParamsController->clearOffset();
    }
    m_worker->setParams(params);

    if (m_exportSettingsWidget->dirty()) {
      m_worker->setExportSettings(m_exportSettingsWidget->settings());
    }

    // Read the current fringe data
    i = std::clamp<size_t>(i, 0, m_datReader.size());

    m_ringBuffer->produce([&, this](std::shared_ptr<OCTData<Float>> &dat) {
      dat->i = i;
      if (auto err = m_datReader.read(i, 1, dat->fringe); err) {
        const auto msg = fmt::format("While loading {}/{}, got {}", i,
                                     m_datReader.size(), *err);
        QMetaObject::invokeMethod(this, &MainWindow::statusBarMessage,
                                  QString::fromStdString(msg));
      }
    });

  } else {
    statusBarMessage(
        "Please load calibration files first by dropping a directory "
        "containing the background and phase files into the GUI.");
  }
}

void MainWindow::afterDatReaderReady() {

  // Update image overlay sequence label
  m_imageDisplay->overlay()->setSequence(
      QString::fromStdString(m_datReader.seq()));
  m_imageDisplay->overlay()->setProgress(0,
                                         static_cast<int>(m_datReader.size()));

  // Update frame controller slider
  m_frameController->setSize(m_datReader.size());
  m_frameController->setPos(0);

  // Set export directory
  const auto exportDir = toPath(QStandardPaths::writableLocation(
                             QStandardPaths::DesktopLocation)) /
                         m_datReader.seq();
  m_exportSettingsWidget->setExportDir(exportDir);
  fs::create_directories(exportDir);

  // Ensure the ring buffers have enough storage
  const auto fringeSize = m_datReader.samplesPerFrame();
  m_ringBuffer->forEach([fringeSize](std::shared_ptr<OCTData<Float>> &dat) {
    dat->fringe.resize(fringeSize);
  });
};

void MainWindow::closeEvent(QCloseEvent *event) {
  m_ringBuffer->quit();
  m_worker->setShouldStop(true);
  m_workerThread.quit();
  m_workerThread.wait();
  QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow() {}
} // namespace OCT