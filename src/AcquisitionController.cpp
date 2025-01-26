#include "AcquisitionController.hpp"

namespace OCT {

AcquisitionControllerObj::AcquisitionControllerObj(
    const std::shared_ptr<RingBuffer<OCTData<Float>>> &buffer)
    : m_daq(buffer) {}

void AcquisitionControllerObj::startAcquisition(AcquisitionParams params) {
  acquiring = true;
  // TODO
}

AcquisitionController::AcquisitionController(
    const std::shared_ptr<RingBuffer<OCTData<Float>>> &buffer)
    : m_controller(buffer), m_btnStartStopAcquisition(new QPushButton("Start")),
      m_btnSaveOrDisplay(new QPushButton("Saving")), m_sbMaxFrames(new QSpinBox)

{

  m_controller.moveToThread(&m_controllerThread);
  m_controllerThread.start(QThread::HighestPriority);

  auto *layout = new QVBoxLayout;
  setLayout(layout);

  auto *grid = new QGridLayout;
  layout->addLayout(grid);

  // Start/stop button
  {

    m_btnStartStopAcquisition->setStyleSheet("background-color: green");
    grid->addWidget(m_btnStartStopAcquisition, 0, 2);

    connect(m_btnStartStopAcquisition, &QPushButton::clicked, this, [this]() {
      this->setEnabled(false);

      if (m_controller.isAcquiring()) {
        m_btnStartStopAcquisition->setText("Stopping");
        m_btnStartStopAcquisition->setStyleSheet("background-color: yellow");
        m_controller.stopAcquisition();

      } else {

        m_btnStartStopAcquisition->setText("Starting");
        QMetaObject::invokeMethod(&m_controller,
                                  &AcquisitionControllerObj::startAcquisition,
                                  m_acqParams);
      }
    });

    // State changed to running
    connect(&m_controller, &AcquisitionControllerObj::sigAcquisitionStarted,
            this, [this]() {
              this->setEnabled(true);
              m_btnStartStopAcquisition->setText("Stop");
              m_btnStartStopAcquisition->setStyleSheet("background-color: red");
            });

    // State changed to stopped
    connect(&m_controller, &AcquisitionControllerObj::sigAcquisitionStarted,
            this, [this]() {
              this->setEnabled(true);
              m_btnStartStopAcquisition->setText("Start");
              m_btnStartStopAcquisition->setStyleSheet(
                  "background-color: green");
            });
  }

  // Save/display button
  {
    grid->addWidget(m_btnSaveOrDisplay, 1, 2);
    m_btnSaveOrDisplay->setCheckable(true);
    connect(m_btnSaveOrDisplay, &QPushButton::toggled, this,
            [this](bool checked) {
              auto *btn = m_btnSaveOrDisplay;
              if (checked) {
                btn->setText("Saving");
                btn->setStyleSheet("background-color: green");

                m_controller.daq().setSaveData(true);
              } else {
                btn->setText("Display only");
                btn->setStyleSheet("");

                m_controller.daq().setSaveData(false);
              }
            });
    m_btnSaveOrDisplay->setChecked(true);
  }

  // Spinbox to set maxFrames
  {
    auto *lbl = new QLabel("Max frames");
    grid->addWidget(lbl, 0, 0);
    grid->addWidget(m_sbMaxFrames, 0, 1);

    m_sbMaxFrames->setMinimum(20);
    m_sbMaxFrames->setMaximum(1000);
    m_sbMaxFrames->setSingleStep(10);
    m_sbMaxFrames->setValue(m_acqParams.maxFrames);

    connect(m_sbMaxFrames, &QSpinBox::valueChanged,
            [this](int val) { m_acqParams.maxFrames = val; });
  }
}

AcquisitionController::~AcquisitionController() {
  if (m_controllerThread.isRunning()) {
    m_controller.stopAcquisition();
    m_controllerThread.quit();
    m_controllerThread.wait();
  }
}

} // namespace OCT