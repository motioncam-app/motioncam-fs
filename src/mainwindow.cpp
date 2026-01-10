#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "settingsdialog.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QPushButton>
#include <QFileInfo>
#include <QProcess>
#include <QMessageBox>
#include <QFileDialog>
#include <QSettings>
#include <QMenu>
#include <QDateTime>
#include <QDir>
#include <algorithm>
#include <chrono>
#include <functional>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QStandardPaths>
#include <QProgressDialog>
#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QThread>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>
#include <QImageReader>
#include <QDebug>
#include <QDirIterator>
#include <QStorageInfo>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <spdlog/spdlog.h>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QDrag>
#include <QMetaObject>
#include <QRunnable>
#include <thread>
#include <atomic>
#include <motioncam/Decoder.hpp>

#ifdef _WIN32
#include "win/FuseFileSystemImpl_Win.h"
#elif __APPLE__
#include "macos/FuseFileSystemImpl_MacOS.h"
#include <sys/mount.h>
#endif

namespace {
    constexpr auto PACKAGE_NAME = "com.motioncam";
    constexpr auto APP_NAME = "MotionCam FS";

    QString fuseAppDataRoot() {
        const QString appData = qEnvironmentVariable("APPDATA");
        if (!appData.isEmpty()) {
            return QDir(appData).filePath("MotionCam Tools/Fuse");
        }
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }

    qint64 estimateInputBytes(const QString& path) {
        QFileInfo info(path);
        if (!info.exists()) {
            return 0;
        }
        if (info.isFile()) {
            return info.size();
        }
        if (!info.isDir()) {
            return 0;
        }
        qint64 totalBytes = 0;
        QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            totalBytes += it.fileInfo().size();
        }
        return totalBytes;
    }

    QString formatSpeed(double mbps) {
        if (mbps >= 1024.0) {
            const double gbps = mbps / 1024.0;
            return QString::number(gbps, 'f', 2) + " GB/s";
        }
        if (mbps >= 1.0) {
            return QString::number(mbps, 'f', 2) + " MB/s";
        }
        const double kbps = mbps * 1024.0;
        if (kbps >= 1.0) {
            return QString::number(kbps, 'f', 2) + " KB/s";
        }
        const double bps = kbps * 1024.0;
        return QString::number(bps, 'f', 0) + " B/s";
    }

    void applyRenderOptionsToUi(Ui::MainWindow& ui, motioncam::FileRenderOptions options) {
        QSignalBlocker blockDraft(ui.draftModeCheckBox);
        QSignalBlocker blockVignette(ui.vignetteCorrectionCheckBox);
        QSignalBlocker blockVignetteColor(ui.vignetteOnlyColorCheckBox);
        QSignalBlocker blockScaleRaw(ui.scaleRawCheckBox);
        QSignalBlocker blockDebugVignette(ui.debugVignetteCheckBox);
        QSignalBlocker blockNormalizeExposure(ui.normalizeExposureCheckBox);
        QSignalBlocker blockCfr(ui.cfrConversionCheckBox);
        QSignalBlocker blockCrop(ui.cropEnableCheckBox);
        QSignalBlocker blockLog(ui.logTransformCheckBox);
        QSignalBlocker blockQuadBayer(ui.quadBayerCheckBox);
        QSignalBlocker blockHighQualityFirstFrame(ui.highQualityFirstFrameCheckBox);

        ui.draftModeCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_DRAFT) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.vignetteCorrectionCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.vignetteOnlyColorCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.scaleRawCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.debugVignetteCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_DEBUG_SHADING_MAP) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.normalizeExposureCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_NORMALIZE_EXPOSURE) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.cfrConversionCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_FRAMERATE_CONVERSION) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.cropEnableCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_CROPPING) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.quadBayerCheckBox->setCheckState(
            (options & motioncam::RENDER_OPT_INTERPRET_AS_QUAD_BAYER) ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
        ui.highQualityFirstFrameCheckBox->setCheckState(Qt::CheckState::Checked);
    }

    motioncam::FileRenderOptions getRenderOptions(Ui::MainWindow& ui) {
        motioncam::FileRenderOptions options = motioncam::RENDER_OPT_NONE;      

        if(ui.draftModeCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_DRAFT;

        if(ui.vignetteCorrectionCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;

        if(ui.vignetteOnlyColorCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;

        if(ui.scaleRawCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;

        if(ui.debugVignetteCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_DEBUG_SHADING_MAP;

        if(ui.normalizeExposureCheckBox->checkState() == Qt::CheckState::Checked) {
            options |= motioncam::RENDER_OPT_NORMALIZE_EXPOSURE;
        } else {
            options |= motioncam::RENDER_OPT_FAST_MOUNT;
        }

        if(ui.cfrConversionCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_FRAMERATE_CONVERSION;

        if(ui.cropEnableCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_CROPPING;

        // Camera model override is now controlled via Settings dialog
        // Always enabled - the actual value is set via mCameraModel
        options |= motioncam::RENDER_OPT_CAMMODEL_OVERRIDE;

        if(ui.draftModeCheckBox->checkState() == Qt::CheckState::Checked)
            options |= motioncam::RENDER_OPT_HIGH_QUALITY_FIRST_FRAME;

        return options;
    }

    QString formatVolumeInfo(const QStorageInfo& storage) {
        const QString root = storage.rootPath();
        const QString fsType = QString::fromLatin1(storage.fileSystemType());
        const QString fsText = fsType.isEmpty() ? "unknown" : fsType;
        return root.isEmpty() ? fsText : QString("%1 (%2)").arg(root, fsText);
    }

    bool isSupportedMountVolume(const QString& mountPath,
                                const QString& context,
                                QString& errorMessage) {
#ifdef _WIN32
        QStorageInfo storage(mountPath);
        if (!storage.isValid() || !storage.isReady()) {
            errorMessage = QString("%1 is not on a ready volume.").arg(context);
            return false;
        }
        const QString fsType = QString::fromLatin1(storage.fileSystemType());
        if (fsType.compare("NTFS", Qt::CaseInsensitive) != 0) {
            errorMessage = QString("%1 must be on an NTFS drive. Current volume is %2.")
                               .arg(context, formatVolumeInfo(storage));
            return false;
        }
#endif
        return true;
    }

#ifdef __APPLE__
    struct ThumbnailTask : public QRunnable {
        explicit ThumbnailTask(std::function<void()> fn) : fn_(std::move(fn)) {
            setAutoDelete(true);
        }
        void run() override {
            fn_();
        }
    private:
        std::function<void()> fn_;
    };

    bool isMotionCamFuseSource(const QString& source) {
        return source.startsWith("MotionCamFuse@") || source.startsWith("MotionCam Fuse@");
    }

    QStringList listMotionCamFuseMounts() {
        struct statfs* mounts = nullptr;
        const int count = getmntinfo(&mounts, MNT_NOWAIT);
        if (count <= 0 || mounts == nullptr) {
            return {};
        }
        QStringList mountPoints;
        for (int i = 0; i < count; ++i) {
            const QString source = QString::fromLocal8Bit(mounts[i].f_mntfromname);
            if (!isMotionCamFuseSource(source)) {
                continue;
            }
            const QString mountPoint = QString::fromLocal8Bit(mounts[i].f_mntonname);
            if (!mountPoint.isEmpty()) {
                mountPoints.push_back(mountPoint);
            }
        }
        return mountPoints;
    }

    void unmountMacFusePath(const QString& mountPoint) {
        const QByteArray mountBytes = mountPoint.toUtf8();
        int res = ::unmount(mountBytes.constData(), 0);
        if (res != 0) {
            ::unmount(mountBytes.constData(), MNT_FORCE);
        }
    }
#endif
} 

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , mDraftQuality(1)
    , mSettingsDirty(false)
    , mAutoApplyPending(false)
    , mSelectionPressWidget(nullptr)
    , mSelectionPressOnSelectableLabel(false)
    , mSelectionPressToggle(false)
    , mSelectionPressAllowSelection(false)
    , mSelectionPressSuppress(false)
    , mSelectionDragActive(false)
    , mSelectionPressModifiers(Qt::NoModifier)
    , mLastSelectedMountId(-1)
    , mCacheCleanupTimer(nullptr)
    , mCachePolicy(motioncam::CachePolicy::Quota)
    , mCacheCleanupIntervalSeconds(10)
    , mCacheQuotaBytes(50ull * 1024ull * 1024ull * 1024ull)
    , mDeleteOnUnmount(false)
    , mMatrixOverrideEnabled(false)
    , mMatrixProfile("")
{
    ui->setupUi(this);
    mDragAndDropLabel = ui->dragAndDropLabel;
#ifdef __APPLE__
    const auto labelList = findChildren<QLabel*>();
    for (auto* label : labelList) {
        QString text = label->text();
        if (text.contains("font-size:9pt") || text.contains("font-size: 9pt")) {
            text.replace("font-size:9pt", "font-size:11pt");
            text.replace("font-size: 9pt", "font-size: 11pt");
            label->setText(text);
        }
    }
#endif
    mApplySelectedButtonBaseStyle = ui->applySelectedButton->styleSheet();
    mApplyAllButtonBaseStyle = ui->applyAllButton->styleSheet();
    ui->applySelectedButton->setCursor(Qt::PointingHandCursor);
    ui->applyAllButton->setCursor(Qt::PointingHandCursor);
    mAutoApplyTimer.setParent(this);
    mAutoApplyTimer.setSingleShot(true);
    connect(&mAutoApplyTimer, &QTimer::timeout, this, [this]() {
        mAutoApplyPending = false;
        if (!mMountedFiles.isEmpty()) {
            applyChanges(true);
        }
    });
    ui->highQualityFirstFrameCheckBox->setVisible(false);
    ui->highQualityFirstFrameLabel->setVisible(false);
    ui->quadBayerCheckBox->setVisible(false);
    ui->quadBayerComboBox->setVisible(false);
    ui->remosaicLabel->setVisible(false);
    ui->logTransformCheckBox->setVisible(false);
    ui->logTransformComboBox->setVisible(false);
    ui->logTransformLabel->setVisible(false);
    ui->highQualityFirstFrameCheckBox->setChecked(true);
    ui->quadBayerCheckBox->setChecked(false);
    ui->quadBayerComboBox->setCurrentText("");
    ui->logTransformCheckBox->setChecked(false);
    ui->logTransformComboBox->setCurrentText("");
    ui->centralwidget->setStyleSheet(
        "QCheckBox[localOverride=\"true\"] { border-left: 3px solid #f1c65a; padding-left: 6px; }"
        "QComboBox[localOverride=\"true\"] { border: 1px solid #f1c65a; }");
    ui->selectedFilesLabel->setText("0 selected");

#ifdef _WIN32
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_Win>();
#elif __APPLE__
    mFuseFilesystem = std::make_unique<motioncam::FuseFileSystemImpl_MacOs>();
    mThumbnailPool.setMaxThreadCount(2);
    QTimer::singleShot(0, this, &MainWindow::cleanupStaleMacFuseMounts);
#endif

    // Enable drag and drop on the scroll area
    ui->dragAndDropScrollArea->setAcceptDrops(true);
    ui->dragAndDropScrollArea->installEventFilter(this);
    ui->dragAndDropScrollArea->viewport()->installEventFilter(this);

    mCacheCleanupTimer = new QTimer(this);
    connect(mCacheCleanupTimer, &QTimer::timeout, this, &MainWindow::onCacheCleanup);

    restoreSettings();
    mGlobalRenderOptions = getRenderOptions(*ui);

    // Connect to widgets
    connect(ui->draftModeCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteCorrectionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->scaleRawCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->debugVignetteCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->vignetteOnlyColorCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->normalizeExposureCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->resetNormalizeExposureButton, &QPushButton::clicked, this, &MainWindow::onResetNormalizeExposure);
    connect(ui->cfrConversionCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->resetCfrButton, &QPushButton::clicked, this, &MainWindow::onResetCfr);
    connect(ui->cropEnableCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->logTransformCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->quadBayerCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);
    connect(ui->highQualityFirstFrameCheckBox, &QCheckBox::checkStateChanged, this, &MainWindow::onRenderSettingsChanged);

    connect(ui->draftQuality, &QComboBox::currentIndexChanged, this, &MainWindow::onDraftModeQualityChanged);
    connect(ui->cfrTarget, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCFRTargetChanged(text.toStdString());
        QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
    });
    connect(ui->exposureCompensationCombobox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onExposureCompensationChanged(text.toStdString());
    });
    connect(ui->cropTargetComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onCropTargetChanged(text.toStdString());
    });
    connect(ui->levelsComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onLevelsChanged(text.toStdString());
    });
    connect(ui->logTransformComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onLogTransformChanged(text.toStdString());
    });
    connect(ui->quadBayerComboBox, &QComboBox::currentTextChanged, this, [this](const QString& text) {
        onQuadBayerChanged(text.toStdString());
    });

    connect(ui->defaultBtn, &QPushButton::clicked, this, &MainWindow::onSetDefaultSettings);
    connect(ui->applySelectedButton, &QPushButton::clicked, this, &MainWindow::onApplySelected);
    connect(ui->applyAllButton, &QPushButton::clicked, this, &MainWindow::onApplyAll);

    // Connect session menu actions
    connect(ui->actionNewSession, &QAction::triggered, this, &MainWindow::onNewSession);
    connect(ui->actionLoadSession, &QAction::triggered, this, &MainWindow::onLoadSession);
    connect(ui->actionSaveSession, &QAction::triggered, this, &MainWindow::onSaveSession);
    connect(ui->actionSaveSessionAs, &QAction::triggered, this, &MainWindow::onSaveSessionAs);
    connect(ui->actionOpenSettings, &QAction::triggered, this, &MainWindow::onOpenSettings);

#ifdef __APPLE__
    auto* forceUnmountAction = new QAction("Force Unmount All", this);
    ui->menuFile->addSeparator();
    ui->menuFile->addAction(forceUnmountAction);
    connect(forceUnmountAction, &QAction::triggered, this, &MainWindow::forceUnmountAllMacFuseMounts);
#endif
    initSessionMenus();

    applyCacheManagementSettings();
}

#ifdef __APPLE__
void MainWindow::cleanupStaleMacFuseMounts() {
    QSet<QString> activePaths;
    for (const auto& mounted : mMountedFiles) {
        QFileInfo info(mounted.mountPath);
        const QString canonical = info.canonicalFilePath().isEmpty()
            ? info.absoluteFilePath()
            : info.canonicalFilePath();
        if (!canonical.isEmpty()) {
            activePaths.insert(canonical);
        }
        activePaths.insert(mounted.mountPath);
    }

    const QStringList mountPoints = listMotionCamFuseMounts();
    for (const auto& mountPoint : mountPoints) {
        if (mountPoint.isEmpty()) {
            continue;
        }

        QFileInfo mountInfo(mountPoint);
        const QString canonicalMount = mountInfo.canonicalFilePath().isEmpty()
            ? mountInfo.absoluteFilePath()
            : mountInfo.canonicalFilePath();

        if (activePaths.contains(canonicalMount) || activePaths.contains(mountPoint)) {
            continue;
        }

        unmountMacFusePath(mountPoint);
        QDir().rmdir(mountPoint);
    }
}

void MainWindow::forceUnmountAllMacFuseMounts() {
    if (QMessageBox::question(this,
                              "Force Unmount All",
                              "Force unmount all MotionCamFuse volumes and clear the session?",
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    QStringList mountPoints = listMotionCamFuseMounts();
    std::sort(mountPoints.begin(), mountPoints.end(),
              [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const auto& mountPoint : mountPoints) {
        unmountMacFusePath(mountPoint);
        QDir().rmdir(mountPoint);
    }

    clearCurrentSession(false);
    updateUi();
}
#endif

MainWindow::~MainWindow() {
    saveSettings();
    autoSaveSession();

    delete ui;
}

void MainWindow::saveSettings() {
    QSettings settings(PACKAGE_NAME, APP_NAME);

    settings.setValue("draftMode", (mGlobalRenderOptions & motioncam::RENDER_OPT_DRAFT) != 0);
    settings.setValue("applyVignetteCorrection", (mGlobalRenderOptions & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION) != 0);
    settings.setValue("scaleRaw", (mGlobalRenderOptions & motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP) != 0);
    settings.setValue("debugVignette", (mGlobalRenderOptions & motioncam::RENDER_OPT_DEBUG_SHADING_MAP) != 0);
    settings.setValue("vignetteOnlyColor", (mGlobalRenderOptions & motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR) != 0);
    settings.setValue("normalizeExposure", (mGlobalRenderOptions & motioncam::RENDER_OPT_NORMALIZE_EXPOSURE) != 0);
    settings.setValue("cfrConversion", (mGlobalRenderOptions & motioncam::RENDER_OPT_FRAMERATE_CONVERSION) != 0);
    settings.setValue("cropEnabled", (mGlobalRenderOptions & motioncam::RENDER_OPT_CROPPING) != 0);
    settings.setValue("interpretAsQBEnabled", false);
    settings.setValue("logTransformEnabled", false);
    settings.setValue("highQualityFirstFrame", true);
    settings.setValue("cachePath", mCacheRootFolder);
    settings.setValue("deleteOnUnmount", mDeleteOnUnmount);
    settings.setValue("draftQuality", mDraftQuality);
    settings.setValue("cfrTarget", QString::fromStdString(mCFRTarget));
    settings.setValue("cropTarget", QString::fromStdString(mCropTarget));
    settings.setValue("exposureCompensation", QString::fromStdString(mExposureCompensation));
    settings.setValue("camModelOverride", QString::fromStdString(mCameraModel));
    settings.setValue("levels", QString::fromStdString(mLevels));
    settings.setValue("logTransform", "");
    settings.setValue("quadBayerOption", "");
    settings.setValue("playerPath", mPlayerPath);
    QString cachePolicyValue = "quota";
    if (mCachePolicy == motioncam::CachePolicy::Quota) {
        cachePolicyValue = "quota";
    } else if (mCachePolicy == motioncam::CachePolicy::Off) {
        cachePolicyValue = "off";
    }
    settings.setValue("cachePolicyMode", cachePolicyValue);
    settings.setValue("cacheQuotaBytes", static_cast<qint64>(mCacheQuotaBytes));
    settings.setValue("cacheCleanupIntervalSeconds", mCacheCleanupIntervalSeconds);

    // Note: Mounted files are now saved via session system, not QSettings
}

void MainWindow::restoreSettings() {
    QSettings settings(PACKAGE_NAME, APP_NAME);

    ui->draftModeCheckBox->setCheckState(
        settings.value("draftMode").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->vignetteCorrectionCheckBox->setCheckState(
        !settings.contains("applyVignetteCorrection") ? Qt::CheckState::Checked :
        (settings.value("applyVignetteCorrection").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->scaleRawCheckBox->setCheckState(
        settings.value("scaleRaw").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    ui->debugVignetteCheckBox->setCheckState(
        settings.value("debugVignette").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->vignetteOnlyColorCheckBox->setCheckState(
        !settings.contains("vignetteOnlyColor") ? Qt::CheckState::Checked :
        (settings.value("vignetteOnlyColor").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->normalizeExposureCheckBox->setCheckState(
        !settings.contains("normalizeExposure") ? Qt::CheckState::Unchecked :
        (settings.value("normalizeExposure").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->cfrConversionCheckBox->setCheckState(
        !settings.contains("cfrConversion") ? Qt::CheckState::Checked :
        (settings.value("cfrConversion").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked));

    ui->cropEnableCheckBox->setCheckState(
        settings.value("cropEnabled").toBool() ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);

    ui->quadBayerCheckBox->setCheckState(Qt::CheckState::Unchecked);

    ui->logTransformCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->highQualityFirstFrameCheckBox->setCheckState(Qt::CheckState::Checked);

    mCacheRootFolder = settings.value("cachePath").toString();
    mDeleteOnUnmount = settings.value("deleteOnUnmount", false).toBool();
    mDraftQuality = std::max(1, settings.value("draftQuality").toInt());
    mCFRTarget = (!settings.contains("cfrTarget") ? "Prefer Drop Frame" : settings.value("cfrTarget").toString().toStdString());
    mExposureCompensation = (!settings.contains("exposureCompensation") ? "0ev" : settings.value("exposureCompensation").toString().toStdString());
    mQuadBayerOption = "";
    mCropTarget = settings.value("cropTarget").toString().toStdString();
#ifdef __APPLE__
    mCameraModel = (!settings.contains("camModelOverride") ? "" : settings.value("camModelOverride").toString().toStdString());
#else
    mCameraModel = (!settings.contains("camModelOverride") ? "Panasonic" : settings.value("camModelOverride").toString().toStdString());
#endif
    mLevels = (!settings.contains("levels") ? "Dynamic" : settings.value("levels").toString().toStdString());
    mLogTransform = "";

#ifdef __APPLE__
    QString defaultPlayerPath;
    if (QFile::exists("/Applications/MotionCam Player.app")) {
        defaultPlayerPath = "/Applications/MotionCam Player.app";
    }
#else
    // Default player path
    QString appDir = QCoreApplication::applicationDirPath();
    QString defaultPlayerPath = QDir(appDir).absoluteFilePath("../Player/MotionCamPlayer.exe");
#endif
    mPlayerPath = settings.value("playerPath", defaultPlayerPath).toString();

    const QString cachePolicyMode = settings.value("cachePolicyMode").toString();
    if (!cachePolicyMode.isEmpty()) {
        if (cachePolicyMode == "quota") {
            mCachePolicy = motioncam::CachePolicy::Quota;
        } else if (cachePolicyMode == "off") {
            mCachePolicy = motioncam::CachePolicy::Off;
        } else {
            mCachePolicy = motioncam::CachePolicy::Quota;
        }
        mCacheCleanupIntervalSeconds = settings.value("cacheCleanupIntervalSeconds", 10).toInt();
        const qint64 defaultQuota = static_cast<qint64>(50ull * 1024ull * 1024ull * 1024ull);
        mCacheQuotaBytes = settings.value("cacheQuotaBytes", defaultQuota).toLongLong();
    } else {
        mCachePolicy = motioncam::CachePolicy::Quota;
        mCacheCleanupIntervalSeconds = 10;
        mCacheQuotaBytes = 50ull * 1024ull * 1024ull * 1024ull;
    }
    if (mCacheCleanupIntervalSeconds <= 0) {
        mCacheCleanupIntervalSeconds = 10;
    }

    if(mDraftQuality == 2)
        ui->draftQuality->setCurrentIndex(0);
    else if(mDraftQuality == 4)
        ui->draftQuality->setCurrentIndex(1);
    else if(mDraftQuality == 8)
        ui->draftQuality->setCurrentIndex(2);
    
    ui->cfrTarget->setCurrentText(QString::fromStdString(mCFRTarget));          // Set CFR target ComboBox to match restored value
    ui->exposureCompensationCombobox->setCurrentText(QString::fromStdString(mExposureCompensation));
    ui->quadBayerComboBox->setCurrentText(QString::fromStdString(mQuadBayerOption));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mCropTarget));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(mLevels));
    ui->logTransformComboBox->setCurrentText(QString::fromStdString(mLogTransform));

    // Note: Mounted files are now restored via session system, not QSettings

    mGlobalRenderOptions = getRenderOptions(*ui);
    updateUi();
}

bool MainWindow::handleAlreadyMounted(const QString& filePath) {
    QFileInfo incomingInfo(filePath);
    QString incomingCanonical = incomingInfo.canonicalFilePath();
    if (incomingCanonical.isEmpty()) {
        incomingCanonical = incomingInfo.absoluteFilePath();
    }

    for (const auto& mounted : mMountedFiles) {
        QFileInfo mountedInfo(mounted.srcFile);
        QString mountedCanonical = mountedInfo.canonicalFilePath();
        if (mountedCanonical.isEmpty()) {
            mountedCanonical = mountedInfo.absoluteFilePath();
        }

        if (mountedCanonical.compare(incomingCanonical, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    auto isSelectionIgnored = [](QWidget* widget) -> bool {
        auto* current = widget;
        while (current) {
            if (current->property("selectionIgnore").toBool()) {
                return true;
            }
            if (qobject_cast<QAbstractButton*>(current)) {
                return true;
            }
            current = current->parentWidget();
        }
        return false;
    };

    auto isComboBox = [](QWidget* widget) -> bool {
        auto* current = widget;
        while (current) {
            if (qobject_cast<QComboBox*>(current)) {
                return true;
            }
            current = current->parentWidget();
        }
        return false;
    };
    auto applySelectionForMount = [this](QWidget* mountWidget, Qt::KeyboardModifiers modifiers) {
        bool ok = false;
        const auto mountId = mountWidget->property("mountId").toInt(&ok);
        if (!ok) {
            return;
        }
        const bool shiftSelect = modifiers & Qt::ShiftModifier;
        const bool toggleSelect = modifiers & Qt::ControlModifier;
        bool handledSelection = false;
        if (shiftSelect && mLastSelectedMountId >= 0) {
            auto* scrollContent = ui->dragAndDropScrollArea->widget();
            auto* scrollLayout = scrollContent ? qobject_cast<QVBoxLayout*>(scrollContent->layout()) : nullptr;
            if (scrollLayout) {
                int startIndex = -1;
                int endIndex = -1;
                for (int i = 0; i < scrollLayout->count(); ++i) {
                    auto* widget = scrollLayout->itemAt(i)->widget();
                    if (!widget) {
                        continue;
                    }
                    bool idOk = false;
                    const auto id = widget->property("mountId").toInt(&idOk);
                    if (!idOk || id < 0) {
                        continue;
                    }
                    if (id == mLastSelectedMountId) {
                        startIndex = i;
                    }
                    if (id == mountId) {
                        endIndex = i;
                    }
                }
                if (startIndex >= 0 && endIndex >= 0) {
                    if (startIndex > endIndex) {
                        std::swap(startIndex, endIndex);
                    }
                    // Clear selection but preserve the anchor for Shift+Click
                    const auto savedAnchor = mLastSelectedMountId;
                    clearSelection();
                    mLastSelectedMountId = savedAnchor;

                    for (int i = startIndex; i <= endIndex; ++i) {
                        auto* widget = scrollLayout->itemAt(i)->widget();
                        if (!widget) {
                            continue;
                        }
                        bool idOk = false;
                        const auto id = widget->property("mountId").toInt(&idOk);
                        if (!idOk || id < 0) {
                            continue;
                        }
                        mSelectedMountIds.insert(id);
                        setFileWidgetSelected(widget, true);
                    }
                    // Don't update anchor during Shift+Click - keep original anchor
                    handledSelection = true;
                }
            }
        }
        if (!handledSelection) {
            if (toggleSelect) {
                if (mSelectedMountIds.contains(mountId)) {
                    mSelectedMountIds.remove(mountId);
                    setFileWidgetSelected(mountWidget, false);
                } else {
                    mSelectedMountIds.insert(mountId);
                    setFileWidgetSelected(mountWidget, true);
                }
            } else {
                clearSelection();
                mSelectedMountIds.insert(mountId);
                setFileWidgetSelected(mountWidget, true);
            }
            mLastSelectedMountId = mountId;
        }
        updateScopeUi();
        if (mSelectedMountIds.size() == 1) {
            applySelectionSettingsToUi();
        }
    };

    if (watched == ui->dragAndDropScrollArea) {
        if (event->type() == QEvent::DragEnter) {
            auto* dragEvent = static_cast<QDragEnterEvent*>(event);

            if (dragEvent->mimeData()->hasUrls()) {
                const auto urls = dragEvent->mimeData()->urls();

                // Check if at least one file has the extension we want
                for (const auto& url : urls) {
                    auto filePath = url.toLocalFile();

                    // Replace ".txt" with your desired file extension
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive)) {
                        dragEvent->acceptProposedAction();
                        return true;
                    }
                }
            }

            return true;
        }
        else if (event->type() == QEvent::Drop) {
            auto* dropEvent = static_cast<QDropEvent*>(event);

            if (dropEvent->mimeData()->hasUrls()) {
                const auto urls = dropEvent->mimeData()->urls();

                // Collect all valid files
                QStringList filesToMount;
                for (const auto& url : urls) {
                    auto filePath = url.toLocalFile();
                    if (filePath.endsWith(".mcraw", Qt::CaseInsensitive)) {
                        filesToMount.append(filePath);
                    }
                }

                // Show progress dialog for any number of files so users see load status
                if (filesToMount.size() > 0) {
                    QProgressDialog progress("Loading files...", "Cancel", 0, filesToMount.size(), this);
                    progress.setWindowModality(Qt::WindowModal);
                    progress.setMinimumDuration(0);

                    for (int i = 0; i < filesToMount.size(); ++i) {
                        if (progress.wasCanceled()) {
                            break;
                        }

                        const QString filePath = filesToMount[i];
                        const qint64 fileBytes = estimateInputBytes(filePath);
                        if (!mountFileWithProgress(filePath, progress, i, filesToMount.size(), fileBytes)) {
                            break;
                        }
                    }

                    progress.setValue(filesToMount.size());
                }

                dropEvent->acceptProposedAction();
            }

            return true;
        }
    }

    auto* watchedLabel = qobject_cast<QLabel*>(watched);
    if (watchedLabel && event->type() == QEvent::Resize) {
        if (watchedLabel->property("fullTitle").isValid()) {
            updateElidedTitle(watchedLabel);
        }
    }

    if (event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            QWidget* pressTarget = qobject_cast<QWidget*>(watched);
            if (watched == ui->dragAndDropScrollArea->viewport()) {
                pressTarget = ui->dragAndDropScrollArea->viewport()->childAt(mouseEvent->pos());
            }
            if (pressTarget && isSelectionIgnored(pressTarget)) {
                return false;
            }
            auto* mountWidget = pressTarget ? findMountWidget(pressTarget) : nullptr;
            if (mountWidget) {
                // Don't apply selection immediately - wait for release or drag
                mSelectionPressPos = mouseEvent->pos();
                mSelectionPressWidget = mountWidget;
                mSelectionPressOnSelectableLabel = false;

                // Check if we should force toggle mode
                Qt::KeyboardModifiers effectiveModifiers = mouseEvent->modifiers();

                // If clicking on select indicator/column, force toggle mode (like Ctrl+Click)
                bool forceToggle = false;
                auto* current = pressTarget;
                while (current && current != mountWidget) {
                    if (current->property("cardToggle").toBool()) {
                        forceToggle = true;
                        break;
                    }
                    current = current->parentWidget();
                }

                // Also check combo boxes
                if (!forceToggle && pressTarget && isComboBox(pressTarget)) {
                    forceToggle = true;
                }

                if (forceToggle) {
                    effectiveModifiers |= Qt::ControlModifier;
                }

                mSelectionPressToggle = effectiveModifiers & Qt::ControlModifier;
                mSelectionPressAllowSelection = true;
                mSelectionPressSuppress = false;
                mSelectionDragActive = false;
                mSelectionPressModifiers = effectiveModifiers;
                return true;
            }
            if (watched == ui->dragAndDropScrollArea->viewport()) {
                clearSelection();
                updateScopeUi();
                return true;
            }
        }
    }

    if (event->type() == QEvent::MouseMove) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if ((mouseEvent->buttons() & Qt::LeftButton) && mSelectionPressWidget && !mSelectionPressOnSelectableLabel) {
            const int distance = (mouseEvent->pos() - mSelectionPressPos).manhattanLength();
            if (!mSelectionDragActive && distance >= QApplication::startDragDistance()) {
                // Check if the pressed file is already part of a multi-selection
                bool ok = false;
                const auto mountId = mSelectionPressWidget->property("mountId").toInt(&ok);
                const bool alreadySelected = ok && mSelectedMountIds.contains(mountId);
                const bool hasMultipleSelected = mSelectedMountIds.size() > 1;

                // If dragging from an already-selected file in a multi-selection, preserve the selection
                // Otherwise, apply the selection logic (toggle, shift-range, or replace)
                if (!alreadySelected || !hasMultipleSelected) {
                    if (ok && mSelectionPressAllowSelection && !mSelectionPressSuppress) {
                        applySelectionForMount(mSelectionPressWidget, mSelectionPressModifiers);
                    }
                }

                QStringList dragPaths;
                    if (!mSelectedMountIds.isEmpty()) {
                        for (auto selectedId : mSelectedMountIds) {
                            if (auto* widget = findFileWidgetForMountId(selectedId)) {
                                const auto path = widget->property("mountPath").toString();
                                if (!path.isEmpty()) {
                                    dragPaths.append(path);
                                }
                            }
                        }
                    } else {
                        const auto path = mSelectionPressWidget->property("mountPath").toString();
                        if (!path.isEmpty()) {
                            dragPaths.append(path);
                        }
                    }

                    if (!dragPaths.isEmpty()) {
                        auto* mimeData = new QMimeData();
                        QList<QUrl> urls;
                        for (const auto& path : dragPaths) {
                            urls.append(QUrl::fromLocalFile(path));
                        }
                        mimeData->setUrls(urls);
                        auto* drag = new QDrag(this);
                        drag->setMimeData(mimeData);
                        mSelectionDragActive = true;
                        mSelectionPressSuppress = true;
                        drag->exec(Qt::CopyAction);
                        return true;
                    }
                }
            }
        if ((mouseEvent->buttons() & Qt::LeftButton) && mSelectionPressWidget && mSelectionPressOnSelectableLabel) {
            const int distance = (mouseEvent->pos() - mSelectionPressPos).manhattanLength();
            if (distance >= QApplication::startDragDistance()) {
                mSelectionPressSuppress = true;
            }
        }
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            // Apply selection on release if no drag occurred
            if (mSelectionPressWidget && !mSelectionDragActive && mSelectionPressAllowSelection && !mSelectionPressSuppress) {
                applySelectionForMount(mSelectionPressWidget, mSelectionPressModifiers);
            }
            mSelectionPressWidget = nullptr;
            mSelectionPressOnSelectableLabel = false;
            mSelectionPressSuppress = false;
            mSelectionDragActive = false;
            mSelectionPressAllowSelection = false;
            mSelectionPressToggle = false;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::mountDestinationPath(const QFileInfo& fileInfo) const {
#ifdef __APPLE__
    QString mountRoot = mCacheRootFolder;
    if (mountRoot.isEmpty()) {
        mountRoot = QDir(QDir::homePath()).filePath("Mounts/MotionCamFuse");
    }
    return QDir(mountRoot).filePath(fileInfo.baseName());
#else
    return (mCacheRootFolder.isEmpty() ? fileInfo.path() : mCacheRootFolder) + "/" + fileInfo.baseName();
#endif
}

void MainWindow::deleteMountOutputIfRequested(const QString& mountPath) {
    if (!mDeleteOnUnmount) {
        return;
    }
    if (mountPath.isEmpty()) {
        return;
    }

    QFileInfo pathInfo(mountPath);
    if (!pathInfo.exists() || !pathInfo.isDir()) {
        return;
    }

    const QString canonical = pathInfo.canonicalFilePath().isEmpty()
        ? pathInfo.absoluteFilePath()
        : pathInfo.canonicalFilePath();

#ifdef __APPLE__
    QString mountRoot = mCacheRootFolder;
    if (mountRoot.isEmpty()) {
        mountRoot = QDir(QDir::homePath()).filePath("Mounts/MotionCamFuse");
    }
    const QString rootCanonical = QDir(mountRoot).canonicalPath().isEmpty()
        ? QDir(mountRoot).absolutePath()
        : QDir(mountRoot).canonicalPath();

    if (rootCanonical.isEmpty() || canonical == rootCanonical ||
        !canonical.startsWith(rootCanonical + QDir::separator())) {
        return;
    }

    QDir dir(canonical);
    const QStringList entries = dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    if (!entries.isEmpty()) {
        spdlog::warn("Skipping delete of non-empty mount folder {}", canonical.toStdString());
        return;
    }

    if (!dir.rmdir(canonical)) {
        spdlog::warn("Failed to remove mount folder {}", canonical.toStdString());
    }
    return;
#endif

#ifndef __APPLE__
    const QString cacheRootCanonical = mCacheRootFolder.isEmpty() ? QString() : QDir(mCacheRootFolder).canonicalPath();
    if (!cacheRootCanonical.isEmpty() && canonical == cacheRootCanonical) {
        return;
    }

    QDir dir(canonical);
    if (dir.isRoot()) {
        return;
    }

    qDebug() << "[REMOVE] Deleting mount output at" << canonical;
    dir.removeRecursively();
#endif
}

bool MainWindow::mountFileBackend(const QString& filePath, motioncam::MountId& mountId, QString& errorMessage) {
    QFileInfo fileInfo(filePath);
    auto dstPath = mountDestinationPath(fileInfo);

#ifdef __APPLE__
    cleanupStaleMacFuseMounts();
#endif

#ifdef _WIN32
    if (mCacheRootFolder.isEmpty()) {
        if (!isSupportedMountVolume(fileInfo.path(), "Source folder", errorMessage)) {
            if (errorMessage.contains("NTFS")) {
                QStorageInfo srcStorage(fileInfo.path());
                errorMessage = QString("Source is on %1. Mounting requires NTFS. Move the file to NTFS or set an NTFS DNG output folder in Preferences.")
                                   .arg(formatVolumeInfo(srcStorage));
            }
            return false;
        }
    } else if (!isSupportedMountVolume(mCacheRootFolder, "DNG output folder", errorMessage)) {
        return false;
    }
#endif

    try {
        auto settings = globalSettingsFromState();
        mountId = mFuseFilesystem->mount(
            settings,
            filePath.toStdString(),
            dstPath.toStdString());
    }
    catch(std::runtime_error& e) {
        errorMessage = QString("There was an error mounting the file. (error: %1)").arg(e.what());
        return false;
    }

    return true;
}

void MainWindow::addMountedFileUi(const QString& filePath, motioncam::MountId mountId, const QString& mountPath) {
    auto formatDuration = [](double seconds) -> QString {
        if (seconds <= 0.0) {
            return QString();
        }
        const int totalSeconds = static_cast<int>(seconds + 0.5);
        const int hours = totalSeconds / 3600;
        const int minutes = (totalSeconds % 3600) / 60;
        const int secs = totalSeconds % 60;
        if (hours > 0) {
            return QString("%1:%2:%3")
                .arg(hours, 2, 10, QChar('0'))
                .arg(minutes, 2, 10, QChar('0'))
                .arg(secs, 2, 10, QChar('0'));
        }
        return QString("%1:%2")
            .arg(minutes, 2, 10, QChar('0'))
            .arg(secs, 2, 10, QChar('0'));
    };
    // Extract just the filename from the path
    QFileInfo fileInfo(filePath);
    auto fileName = fileInfo.fileName();
    auto dstPath = mountPath;

    // Get the scroll area's content widget and its layout
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());

    // Create a widget to hold a filename label and buttons
    auto* fileWidget = new QWidget(scrollContent);
    fileWidget->setObjectName("fileCard");

    fileWidget->setMinimumHeight(170);
    fileWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    fileWidget->setProperty("filePath", filePath);
    fileWidget->setToolTip(filePath);
    fileWidget->setProperty("mountId", mountId);
    fileWidget->setProperty("mountPath", dstPath);
    fileWidget->setProperty("selected", false);
    fileWidget->setProperty("cardClickProxy", true);
    fileWidget->setCursor(Qt::PointingHandCursor);
    fileWidget->setAttribute(Qt::WA_StyledBackground, true);
    fileWidget->installEventFilter(this);
#ifdef __APPLE__
    const int titlePt = 13;
    const int infoPt = 10;
    const int sourcePt = 10;
    const int indexPt = 10;
    const int resetPt = 11;
    const int badgePt = 10;
    const int globalBadgePt = 10;
#else
    const int titlePt = 11;
    const int infoPt = 8;
    const int sourcePt = 8;
    const int indexPt = 8;
    const int resetPt = 9;
    const int badgePt = 8;
    const int globalBadgePt = 8;
#endif

    fileWidget->setStyleSheet(QString(
        "QWidget#fileCard { background-color: transparent; }"
        "QWidget#fileCard:hover { background-color: #1b2736; border: 1px solid #2d4460; border-radius: 6px; }"
        "QWidget#fileCard[selected=\"true\"] { background-color: #223246; border: 1px solid #3a5878; border-radius: 6px; }"
        "QLabel { background-color: transparent; }"
        "QLabel[labelRole=\"title\"] { font-weight: bold; font-size: %1pt; color: #e6e6e6; }"
        "QLabel[labelRole=\"info\"] { font-size: %2pt; color: #888888; }"
        "QLabel[labelRole=\"source\"] { font-size: %3pt; color: #666666; }"
        "QLabel#indexLabel { font-size: %4pt; color: #7f93ad; }"
        "QLabel#selectIndicator { border: 1px solid #3a5878; border-radius: 3px; background: transparent; }"
        "QLabel#selectIndicator[checked=\"true\"] { background-color: #f1c65a; border-color: #f1c65a; }"
        "QPushButton#localReset { color: #e6e6e6; border: 1px solid #e6e6e6; border-radius: 4px; padding: 0px; min-width: 16px; max-width: 16px; min-height: 16px; max-height: 16px; font-size: %5pt; font-weight: 700; background: transparent; }"
        "QPushButton#localReset:hover { background-color: rgba(230, 230, 230, 0.16); }"
        "QPushButton#localReset:pressed { background-color: rgba(230, 230, 230, 0.24); }"
        "QLabel#localBadge { font-size: %6pt; font-weight: 700; color: #f1c65a; border: 1px solid #f1c65a; border-radius: 4px; padding: 2px 6px; }"
        "QLabel#globalBadge { font-size: %7pt; font-weight: 600; color: #a4b1c2; border: 1px solid #46566b; border-radius: 4px; padding: 2px 6px; }"
        "QWidget#fileCard[selected=\"true\"] QLabel[labelRole=\"title\"] { color: #f1c65a; }"
        "QWidget#fileCard[selected=\"true\"] QLabel[labelRole=\"info\"] { color: #c5d6ea; }"
        "QWidget#fileCard[selected=\"true\"] QLabel[labelRole=\"source\"] { color: #9bb4cc; }"
        "QWidget#fileCard[selected=\"true\"] QLabel#selectIndicator { }")
        .arg(titlePt)
        .arg(infoPt)
        .arg(sourcePt)
        .arg(indexPt)
        .arg(resetPt)
        .arg(badgePt)
        .arg(globalBadgePt));

    // Main horizontal layout (thumbnail on left, info on right)
    auto* mainLayout = new QHBoxLayout(fileWidget);
    mainLayout->setContentsMargins(4, 10, 10, 10);
    mainLayout->setSpacing(0);

    auto* selectColumn = new QWidget(fileWidget);
    selectColumn->setObjectName("selectColumn");
    selectColumn->setFixedWidth(28);
    selectColumn->setCursor(Qt::PointingHandCursor);
    selectColumn->setProperty("cardClickProxy", true);
    selectColumn->setProperty("cardToggle", true);
    selectColumn->setStyleSheet("background-color: transparent;");
    selectColumn->setAutoFillBackground(false);
    selectColumn->setAttribute(Qt::WA_TranslucentBackground, true);
    selectColumn->installEventFilter(this);
    auto* indicatorLayout = new QVBoxLayout(selectColumn);
    indicatorLayout->setContentsMargins(0, 0, 0, 0);
    indicatorLayout->setSpacing(6);
    indicatorLayout->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    indicatorLayout->addStretch();
    auto* indexLabel = new QLabel(selectColumn);
    indexLabel->setObjectName("indexLabel");
    indexLabel->setText("--");
    indexLabel->setAlignment(Qt::AlignHCenter);
    indexLabel->setContentsMargins(0, 0, 0, 0);
    indexLabel->setFixedHeight(indexLabel->fontMetrics().height() + 2);
    indexLabel->setProperty("cardClickProxy", true);
    indexLabel->setProperty("cardToggle", true);
    indexLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    indicatorLayout->addWidget(indexLabel, 0, Qt::AlignHCenter);
    indicatorLayout->addSpacing(6);
    auto* selectIndicator = new QLabel(selectColumn);
    selectIndicator->setObjectName("selectIndicator");
    selectIndicator->setFixedSize(14, 14);
    selectIndicator->setAttribute(Qt::WA_StyledBackground, true);
    selectIndicator->setAlignment(Qt::AlignCenter);
    selectIndicator->setProperty("cardClickProxy", true);
    selectIndicator->setProperty("cardToggle", true);
    selectIndicator->setCursor(Qt::PointingHandCursor);
    selectIndicator->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    indicatorLayout->addWidget(selectIndicator, 0, Qt::AlignHCenter);
    indicatorLayout->addStretch();
    mainLayout->addWidget(selectColumn);
    mainLayout->addSpacing(6);

    // Create thumbnail label
    auto* thumbnailLabel = new QLabel(fileWidget);
    thumbnailLabel->setObjectName("thumbnailLabel");
    thumbnailLabel->setFixedSize(180, 135);
    thumbnailLabel->setScaledContents(true);
    thumbnailLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333;");
    thumbnailLabel->setAlignment(Qt::AlignCenter);
    thumbnailLabel->setText("Loading...");
    thumbnailLabel->setProperty("selectionIgnore", true);
    mainLayout->addWidget(thumbnailLabel);
    mainLayout->addSpacing(8);

    // Create vertical layout for file info and buttons
    auto* fileLayout = new QVBoxLayout();
    fileLayout->setSpacing(3);

    QString durationText;
    auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
    if (fileInfoOpt.has_value()) {
        const auto info = fileInfoOpt.value();
        const double fps = info.avgFps > 0.0f ? info.avgFps : (info.fps > 0.0f ? info.fps : info.medFps);
        if (fps > 0.0 && info.totalFrames > 0) {
            durationText = formatDuration(static_cast<double>(info.totalFrames) / fps);
        }
    }

    // Create and add the filename label
    auto* titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(0, 0, 0, 0);
    titleLayout->setSpacing(8);

    auto displayName = fileInfo.baseName();
    if (!durationText.isEmpty()) {
        displayName = QString("%1 - %2").arg(displayName, durationText);
    }

    auto* fileLabel = new QLabel(displayName, fileWidget);
    fileLabel->setToolTip(filePath); // Show full path on hover
    fileLabel->setProperty("labelRole", "title");
    fileLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    fileLabel->setProperty("cardClickProxy", true);
    fileLabel->setCursor(Qt::PointingHandCursor);
    fileLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    fileLabel->installEventFilter(this);
    fileLabel->setMinimumWidth(0);
    fileLabel->setMinimumHeight(fileLabel->fontMetrics().height() + 4);
    fileLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    fileLabel->setProperty("fullTitle", displayName);
    updateElidedTitle(fileLabel);
    titleLayout->addWidget(fileLabel);

    auto* localReset = new QPushButton("X", fileWidget);
    localReset->setObjectName("localReset");
    localReset->setToolTip("Reset local overrides");
    localReset->setCursor(Qt::PointingHandCursor);
    localReset->setFocusPolicy(Qt::NoFocus);
    localReset->setProperty("selectionIgnore", true);
    localReset->setVisible(mLocalSettings.contains(mountId));
    connect(localReset, &QPushButton::clicked, this, [this, mountId]() {
        if (!mLocalSettings.contains(mountId)) {
            return;
        }
        mLocalSettings.remove(mountId);
        applySettingsToMount(mountId, globalSettingsFromState());
        updateThumbnailForMount(mountId);
        updateLocalBadgeForMount(mountId);
        if (mSelectedMountIds.contains(mountId)) {
            applySelectionSettingsToUi();
        }
        updateScopeUi();
    });

    auto* localBadge = new QLabel("LOCAL", fileWidget);
    localBadge->setObjectName("localBadge");
    localBadge->setVisible(mLocalSettings.contains(mountId));

    auto* globalBadge = new QLabel("GLOBAL", fileWidget);
    globalBadge->setObjectName("globalBadge");
    globalBadge->setVisible(!mLocalSettings.contains(mountId));
    titleLayout->setStretch(0, 1);
    fileLayout->addLayout(titleLayout);
    titleLayout->addStretch();
    titleLayout->addWidget(localBadge, 0, Qt::AlignVCenter);
    titleLayout->addWidget(localReset, 0, Qt::AlignVCenter);
    titleLayout->addWidget(globalBadge, 0, Qt::AlignVCenter);

    // Get file information from the FUSE filesystem
    if (fileInfoOpt.has_value()) {
        auto info = fileInfoOpt.value();

        // Create info labels with FPS, Total Frames/Dropped, and Resolution
        auto infoLine1 = QString("Median / Average / Target FPS: %1 / %2 -> %3")
                                .arg(QString::number(info.medFps, 'f', 2))
                                .arg(QString::number(info.avgFps, 'f', 2))
                                .arg(QString::number(info.fps, 'f', 2));

        auto infoLine2 = QString("Framecount: %1 | Dropped: -%2 | Duplicated: +%3 | Resolution: %4x%5")
                                .arg(info.totalFrames)
                                .arg(info.droppedFrames)
                                .arg(info.duplicatedFrames)
                                .arg(info.width)
                                .arg(info.height);

    auto* infoLabel1 = new QLabel(infoLine1, fileWidget);
    infoLabel1->setProperty("labelRole", "info");
    infoLabel1->setTextInteractionFlags(Qt::NoTextInteraction);
    infoLabel1->setProperty("cardClickProxy", true);
    infoLabel1->setProperty("infoLabel", true);
    infoLabel1->setProperty("infoLine", 1);
        infoLabel1->setProperty("mountId", QVariant(mountId));
        infoLabel1->setCursor(Qt::ArrowCursor);
        infoLabel1->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        infoLabel1->installEventFilter(this);
        infoLabel1->setMinimumWidth(0);
        infoLabel1->setMinimumHeight(infoLabel1->fontMetrics().height() + 2);
        infoLabel1->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        fileLayout->addWidget(infoLabel1);

    auto* infoLabel2 = new QLabel(infoLine2, fileWidget);
    infoLabel2->setProperty("labelRole", "info");
    infoLabel2->setTextInteractionFlags(Qt::NoTextInteraction);
    infoLabel2->setProperty("cardClickProxy", true);
    infoLabel2->setProperty("infoLabel", true);
    infoLabel2->setProperty("infoLine", 2);
        infoLabel2->setProperty("mountId", QVariant(mountId));
        infoLabel2->setCursor(Qt::ArrowCursor);
        infoLabel2->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        infoLabel2->installEventFilter(this);
        infoLabel2->setMinimumWidth(0);
        infoLabel2->setMinimumHeight(infoLabel2->fontMetrics().height() + 2);
        infoLabel2->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        fileLayout->addWidget(infoLabel2);
    }

    // Create and add the source folder label
    auto* sourceLabel = new QLabel(QString("Source: %1").arg(fileInfo.path()), fileWidget);
    sourceLabel->setProperty("labelRole", "source");
    sourceLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    sourceLabel->setProperty("cardClickProxy", true);
    sourceLabel->setCursor(Qt::ArrowCursor);
    sourceLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    sourceLabel->installEventFilter(this);
    sourceLabel->setMinimumWidth(0);
    sourceLabel->setMinimumHeight(sourceLabel->fontMetrics().height() + 2);
    sourceLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    fileLayout->addWidget(sourceLabel);

    // Add spacer to maintain button position
    fileLayout->addSpacing(12);

    // Create horizontal layout for buttons
    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(8);
    
    // Define consistent button size
    const int buttonWidth = 92;
    const int buttonHeight = 28;
    const char* actionButtonStyle =
        "QPushButton { background-color: #253447; border: 1px solid #3a5878; border-radius: 4px; color: #e6e6e6; }"
        "QPushButton:hover { background-color: #2f425a; }"
        "QPushButton:pressed { background-color: #1c2736; }";

    // Create and add the open button
    auto* openButton = new QPushButton("Open", fileWidget);
    openButton->setProperty("selectionIgnore", true);
    openButton->setFixedSize(buttonWidth, buttonHeight);
    openButton->setIcon(QIcon(":/assets/folder_btn.png"));
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setStyleSheet(actionButtonStyle);
    buttonLayout->addWidget(openButton);

    // Create and add the play button
    auto* playButton = new QPushButton("Play", fileWidget);
    playButton->setProperty("selectionIgnore", true);
    playButton->setFixedSize(buttonWidth, buttonHeight);
    playButton->setIcon(QIcon(":/assets/play_btn.png"));
    playButton->setCursor(Qt::PointingHandCursor);
    playButton->setStyleSheet(actionButtonStyle);
    buttonLayout->addWidget(playButton);

    // Create and add the remove button
    auto* removeButton = new QPushButton("Unmount", fileWidget);
    removeButton->setProperty("selectionIgnore", true);
    removeButton->setFixedSize(buttonWidth, buttonHeight);
    removeButton->setIcon(QIcon(":/assets/remove_btn.png"));
    removeButton->setCursor(Qt::PointingHandCursor);
    removeButton->setStyleSheet(actionButtonStyle);
    buttonLayout->addWidget(removeButton);

    // Add stretch to push buttons to the left
    buttonLayout->addStretch();

    // Add button layout to file info layout
    fileLayout->addLayout(buttonLayout);
    fileLayout->addSpacing(10);

    // Add file info layout to main horizontal layout
    mainLayout->addLayout(fileLayout);

    // Generate and load actual frame thumbnail
    QString thumbPath = QDir::temp().filePath(QString("mcraw_thumb_%1.jpg").arg(mountId));

#ifdef __APPLE__
    thumbnailLabel->setText("Generating...");
    thumbnailLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; color: #888888;");

    QPointer<QLabel> thumbLabelPtr = thumbnailLabel;
    const int mountIdCopy = mountId;
    const QString thumbPathCopy = thumbPath;
    mThumbnailPool.start(new ThumbnailTask([this, thumbLabelPtr, mountIdCopy, thumbPathCopy]() {
        const bool ok = mFuseFilesystem->generateThumbnail(
            mountIdCopy,
            thumbPathCopy.toStdString(),
            320,
            240);
        QMetaObject::invokeMethod(this, [thumbLabelPtr, thumbPathCopy, ok]() {
            if (!thumbLabelPtr) {
                return;
            }
            if (ok) {
                QPixmap thumbnail(thumbPathCopy);
                if (!thumbnail.isNull()) {
                    thumbLabelPtr->setPixmap(thumbnail);
                    thumbLabelPtr->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333;");
                } else {
                    thumbLabelPtr->setText("Failed to load");
                    thumbLabelPtr->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; color: #888888;");
                }
            } else {
                thumbLabelPtr->setText("No preview");
                thumbLabelPtr->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; color: #888888;");
            }
        }, Qt::QueuedConnection);
    }));
#else
    if (mFuseFilesystem->generateThumbnail(mountId, thumbPath.toStdString(), 320, 240)) {
        QPixmap thumbnail(thumbPath);
        if (!thumbnail.isNull()) {
            thumbnailLabel->setPixmap(thumbnail);
            thumbnailLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333;");
        } else {
            thumbnailLabel->setText("Failed to load");
            thumbnailLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; color: #888888;");
        }
    } else {
        thumbnailLabel->setText("No preview");
        thumbnailLabel->setStyleSheet("background-color: #1a1a1a; border: 1px solid #333333; color: #888888;");
    }
#endif

    for (auto* child : fileWidget->findChildren<QWidget*>()) {
        if (child->property("selectionIgnore").toBool()) {
            continue;
        }
        if (qobject_cast<QAbstractButton*>(child)) {
            child->setProperty("selectionIgnore", true);
            continue;
        }
        child->setProperty("cardClickProxy", true);
        child->installEventFilter(this);
    }

    // Add separator if there are already mounted files
    if (!mMountedFiles.empty()) {
        auto* separator = new QFrame(scrollContent);

        separator->setFrameShape(QFrame::HLine);
        separator->setFrameShadow(QFrame::Plain);
        separator->setLineWidth(1);
        separator->setStyleSheet("QFrame { color: #e0e0e0; margin: 16px 0px; }");
        scrollLayout->insertWidget(0, separator);
    }

    // Add the file widget to the scroll area
    scrollLayout->insertWidget(0, fileWidget);
    QTimer::singleShot(0, fileWidget, [this, fileLabel]() {
        updateElidedTitle(fileLabel);
    });
    updateCardIndices();

    // Hide the drag-drop label since we now have files
    ui->dragAndDropLabel->hide();

    // Connect buttons
    connect(openButton, &QPushButton::clicked, this, [this, fileWidget] {
        openMountedDirectory(fileWidget);
    });

    connect(playButton, &QPushButton::clicked, this, [this, fileWidget] {
        playMountedFolder(fileWidget);
    });

    connect(removeButton, &QPushButton::clicked, this, [this, fileWidget] {
        removeFile(fileWidget);
    });

    mMountedFiles.append(
        motioncam::MountedFile(mountId, filePath, mountPath));
}

bool MainWindow::mountFileWithProgress(const QString& filePath, QProgressDialog& progress, int index, int total, qint64 fileBytes) {
    QFileInfo loadingInfo(filePath);
    progress.setRange(0, 100);
    progress.setValue(0);

    if (handleAlreadyMounted(filePath)) {
        return true;
    }

    std::atomic<bool> done(false);
    std::atomic<uint64_t> bytesRead(0);
    motioncam::MountId mountId = 0;
    QString errorMessage;

    QElapsedTimer timer;
    timer.start();
    double smoothedMbps = 0.0;

    std::thread worker([this, &filePath, &mountId, &errorMessage, &done, &bytesRead]() {
        // Decoder in this snapshot does not expose setReadCounter; keep the counter but skip hook.
        QString localError;
        motioncam::MountId localMountId = 0;
        if (mountFileBackend(filePath, localMountId, localError)) {
            mountId = localMountId;
        } else {
            errorMessage = localError;
        }
        done.store(true);
    });

    while (!done.load()) {
        QString speedText = "measuring...";
        const qint64 elapsedMs = timer.elapsed();
        const uint64_t readBytes = bytesRead.load();
        int progressValue = 0;
        if (fileBytes > 0 && readBytes > 0) {
            const double ratio = static_cast<double>(readBytes) / static_cast<double>(fileBytes);
            progressValue = std::min(99, static_cast<int>(ratio * 100.0));
        }
        if (elapsedMs > 0 && readBytes > 0) {
            const double mb = static_cast<double>(readBytes) / (1024.0 * 1024.0);
            const double seconds = static_cast<double>(elapsedMs) / 1000.0;     
            const double mbps = mb / seconds;
            const double alpha = 0.2;
            if (smoothedMbps <= 0.0) {
                smoothedMbps = mbps;
            } else {
                smoothedMbps = alpha * mbps + (1.0 - alpha) * smoothedMbps;
            }
            speedText = formatSpeed(smoothedMbps);
        }

        progress.setValue(progressValue);
        progress.setLabelText(QString("Loading %1 of %2:\n%3\nRead speed: %4")  
            .arg(index + 1)
            .arg(total)
            .arg(loadingInfo.fileName())
            .arg(speedText));

        QApplication::processEvents();
        QThread::msleep(100);
    }

    if (worker.joinable()) {
        worker.join();
    }

    if (!errorMessage.isEmpty()) {
        QMessageBox::critical(this, "Error", errorMessage);
        return false;
    }

    const QString mountPath = mountDestinationPath(loadingInfo);
    progress.setValue(100);
    addMountedFileUi(filePath, mountId, mountPath);
    return true;
}

void MainWindow::mountFile(const QString& filePath) {
    motioncam::MountId mountId = 0;
    QString errorMessage;
    if (handleAlreadyMounted(filePath)) {
        return;
    }
    if (!mountFileBackend(filePath, mountId, errorMessage)) {
        QMessageBox::critical(this, "Error", errorMessage);
        return;
    }

    const QFileInfo fileInfo(filePath);
    const QString mountPath = mountDestinationPath(fileInfo);
    addMountedFileUi(filePath, mountId, mountPath);
}

void MainWindow::playFile(const QString& path) {
    bool success = false;

#ifdef _WIN32
    if (mPlayerPath.isEmpty() || !QFile::exists(mPlayerPath)) {
        QMessageBox::warning(this, "Error",
            QString("Player not found at: %1\n\nPlease set the player path in Cache & Player settings.").arg(mPlayerPath));
        return;
    }

    success = QProcess::startDetached(QDir::cleanPath(mPlayerPath), QStringList() << path);
#elif __APPLE__
    QString resolvedPath = path;
    if (resolvedPath.startsWith("~/")) {
        resolvedPath = QDir::home().filePath(resolvedPath.mid(2));
    }
    QFileInfo sourceInfo(resolvedPath);
    if (!sourceInfo.exists()) {
        QMessageBox::warning(this, "Error",
            QString("Source file not found: %1").arg(resolvedPath));
        return;
    }
    resolvedPath = sourceInfo.canonicalFilePath().isEmpty()
        ? sourceInfo.absoluteFilePath()
        : sourceInfo.canonicalFilePath();

    if (!mPlayerPath.isEmpty() && !QFile::exists(mPlayerPath)) {
        QMessageBox::warning(this, "Error",
            QString("Player not found at: %1\n\nPlease set the player app path in Cache & Player settings.").arg(mPlayerPath));
        return;
    }

    if (!mPlayerPath.isEmpty()) {
        // Avoid LaunchServices document-type errors; pass file only via argv.
        success = QProcess::startDetached(
            "/usr/bin/open",
            QStringList() << "-a" << mPlayerPath << "--args" << resolvedPath);
    } else {
        success = QProcess::startDetached("/usr/bin/open", QStringList() << resolvedPath);
    }
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to launch player with file: %1").arg(path));
}

void MainWindow::playMountedFolder(QWidget* fileWidget) {
    auto mountPath = fileWidget->property("mountPath").toString();
    if (mountPath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Mount path not found");
        return;
    }

#ifdef _WIN32
    if (mPlayerPath.isEmpty() || !QFile::exists(mPlayerPath)) {
        QMessageBox::warning(this, "Error",
            QString("Player not found at: %1\n\nPlease set the player path in Settings > Preferences.").arg(mPlayerPath));
        return;
    }

    // Get the source MCRAW file path to pass to MotionCamPlayer
    QString srcFilePath = fileWidget->property("filePath").toString();
    if (srcFilePath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Source file path not found");
        return;
    }

    bool success = QProcess::startDetached(QDir::cleanPath(mPlayerPath), QStringList() << srcFilePath);

    if (!success) {
        QMessageBox::warning(this, "Error", QString("Failed to launch player with: %1").arg(srcFilePath));
    }
#elif __APPLE__
    // For macOS, launch the player with the source MCRAW file.
    QString srcFilePath = fileWidget->property("filePath").toString();
    if (srcFilePath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Source file path not found");
        return;
    }
    playFile(srcFilePath);
#endif
}

void MainWindow::openMountedDirectory(QWidget* fileWidget) {
    auto mountPath = fileWidget->property("mountPath").toString();
    if (mountPath.isEmpty()) {
        QMessageBox::warning(this, "Error", "Mount path not found");
        return;
    }

    bool success = false;

#ifdef _WIN32
    success = QProcess::startDetached("explorer", QStringList() << QDir::toNativeSeparators(mountPath));
#elif __APPLE__
    success = QProcess::startDetached("/usr/bin/open", QStringList() << mountPath);
#endif

    if (!success)
        QMessageBox::warning(this, "Error", QString("Failed to open directory: %1").arg(mountPath));
}

void MainWindow::removeFile(QWidget* fileWidget) {
    qDebug() << "[REMOVE] START - fileWidget:" << fileWidget;

    if (!fileWidget) {
        qDebug() << "[REMOVE] ERROR: fileWidget is null";
        return;
    }

    // Get mount ID and other info BEFORE deleting the widget
    bool ok = false;
    auto mountId = fileWidget->property("mountId").toInt(&ok);
    QString mountPath = fileWidget->property("mountPath").toString();
    qDebug() << "[REMOVE] Mount ID:" << mountId << "ok:" << ok << "Files count:" << mMountedFiles.size();

    // Unmount the file first (before UI cleanup)
    if(ok) {
        auto it = std::find_if(
            mMountedFiles.begin(), mMountedFiles.end(),
            [mountId](const motioncam::MountedFile& f) { return f.mountId == mountId; });
        if (mountPath.isEmpty() && it != mMountedFiles.end()) {
            mountPath = it->mountPath;
        }

        qDebug() << "[REMOVE] Calling unmount for ID:" << mountId;
        mFuseFilesystem->unmount(mountId);
        qDebug() << "[REMOVE] Unmount completed";

        deleteMountOutputIfRequested(mountPath);

        // Delete the thumbnail file
        QString thumbPath = QDir::temp().filePath(QString("mcraw_thumb_%1.jpg").arg(mountId));
        qDebug() << "[REMOVE] Deleting thumbnail:" << thumbPath;
        QFile::remove(thumbPath);
        qDebug() << "[REMOVE] Thumbnail deleted";

        // Remove from mounted files list
        qDebug() << "[REMOVE] Removing from mMountedFiles list";
        if(it != mMountedFiles.end())
            mMountedFiles.erase(it);
        qDebug() << "[REMOVE] Removed from list. New count:" << mMountedFiles.size();

        mLocalSettings.remove(mountId);
        mSelectedMountIds.remove(mountId);
        updateScopeUi();
    }

    // Show drag-drop label when all files are removed
    if (mMountedFiles.empty()) {
        qDebug() << "[REMOVE] All files removed - showing drag-drop label";
        QPointer<QLabel> label = mDragAndDropLabel;
        QTimer::singleShot(0, this, [label]() {
            if (label) {
                label->show();
                label->raise();
            }
        });
    }

    // Now clean up the UI
    qDebug() << "[REMOVE] Starting UI cleanup";
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        qDebug() << "[REMOVE] ERROR: scrollContent is null";
        return;
    }

    auto* scrollLayout = qobject_cast<QVBoxLayout*>(scrollContent->layout());
    if (!scrollLayout) {
        qDebug() << "[REMOVE] ERROR: scrollLayout is null";
        return;
    }

    // Find and remove the separator above this file widget if it exists
    int fileWidgetIndex = scrollLayout->indexOf(fileWidget);
    qDebug() << "[REMOVE] Widget index:" << fileWidgetIndex;

    if (fileWidgetIndex > 0) {
        qDebug() << "[REMOVE] Checking for separator above";
        auto* itemAbove = scrollLayout->itemAt(fileWidgetIndex - 1);
        if (itemAbove && itemAbove->widget()) {
            auto* widgetAbove = itemAbove->widget();
            // Check if it's a separator (QFrame with HLine shape)
            auto* frame = qobject_cast<QFrame*>(widgetAbove);
            if (frame && frame->frameShape() == QFrame::HLine) {
                qDebug() << "[REMOVE] Found separator, removing";
                scrollLayout->removeWidget(frame);
                frame->deleteLater();
                qDebug() << "[REMOVE] Separator removed";
            }
        }
    }

    // Remove widget from layout and schedule for deletion
    qDebug() << "[REMOVE] Removing widget from layout";
    scrollLayout->removeWidget(fileWidget);
    qDebug() << "[REMOVE] Calling deleteLater on widget";
    fileWidget->deleteLater();
    qDebug() << "[REMOVE] deleteLater called";

    updateCardIndices();
    qDebug() << "[REMOVE] END";
}

void MainWindow::updateUi() {
    auto isCheckedOrMixed = [](QCheckBox* checkBox) {
        return checkBox->checkState() != Qt::CheckState::Unchecked;
    };

    auto isMixed = [](QCheckBox* checkBox) {
        return checkBox->checkState() == Qt::CheckState::PartiallyChecked;
    };

    // Draft quality only enabled when draft mode is on
    if (isCheckedOrMixed(ui->draftModeCheckBox)) {
        ui->draftQuality->setEnabled(true);
        ui->quadBayerComboBox->setEnabled(false);
    } else {
        ui->draftQuality->setEnabled(false);
        ui->quadBayerComboBox->setEnabled(false);
    }

    ui->cropTargetComboBox->setEnabled(isCheckedOrMixed(ui->cropEnableCheckBox));

    ui->quadBayerCheckBox->setEnabled(false);
    ui->quadBayerComboBox->setEnabled(false);
    ui->logTransformCheckBox->setEnabled(false);
    ui->logTransformComboBox->setEnabled(false);

    // Scale raw only enabled when vignette correction is on
    if (isCheckedOrMixed(ui->vignetteCorrectionCheckBox)) {
        ui->scaleRawCheckBox->setEnabled(true);
        if (ui->scaleRawCheckBox->checkState() == Qt::CheckState::Checked) {
            ui->debugVignetteCheckBox->setEnabled(false);
            if (!isMixed(ui->scaleRawCheckBox)) {
                ui->debugVignetteCheckBox->setChecked(false);
            }
        } else {
            ui->debugVignetteCheckBox->setEnabled(true);
        }
        ui->vignetteOnlyColorCheckBox->setEnabled(true);
    } else {
        ui->scaleRawCheckBox->setEnabled(false);
        ui->debugVignetteCheckBox->setEnabled(false);
        ui->vignetteOnlyColorCheckBox->setEnabled(false);
        if (!isLocalModeActive()) {
            ui->scaleRawCheckBox->setChecked(false);
            ui->debugVignetteCheckBox->setChecked(false);
            ui->vignetteOnlyColorCheckBox->setChecked(false);
        }
    }
}

MainWindow::RenderSettings MainWindow::captureSettingsFromUi() const {
    RenderSettings settings;
    settings.renderOptions = getRenderOptions(*ui);
    settings.draftQuality = draftQualityFromUi();
    settings.cfrTarget = ui->cfrTarget->currentText().toStdString();
    settings.cropTarget = ui->cropTargetComboBox->currentText().toStdString();
    settings.cameraModel = mCameraModel;
    settings.levels = ui->levelsComboBox->currentText().toStdString();
    settings.exposureCompensation = ui->exposureCompensationCombobox->currentText().toStdString();
    return settings;
}

MainWindow::RenderSettings MainWindow::globalSettingsFromState() const {
    RenderSettings settings;
    settings.renderOptions = mGlobalRenderOptions;
    settings.draftQuality = mDraftQuality;
    settings.cfrTarget = mCFRTarget;
    settings.cropTarget = mCropTarget;
    settings.cameraModel = mCameraModel;
    settings.levels = mLevels;
    settings.exposureCompensation = mExposureCompensation;
    return settings;
}

MainWindow::RenderSettings MainWindow::effectiveSettingsForMount(motioncam::MountId mountId) const {
    if (mLocalSettings.contains(mountId)) {
        return mLocalSettings.value(mountId);
    }
    return globalSettingsFromState();
}

void MainWindow::applySettingsToUi(const RenderSettings& settings) {
    QSignalBlocker blockDraft(ui->draftQuality);
    QSignalBlocker blockCfr(ui->cfrTarget);
    QSignalBlocker blockCrop(ui->cropTargetComboBox);
    QSignalBlocker blockLevels(ui->levelsComboBox);
    QSignalBlocker blockLog(ui->logTransformComboBox);
    QSignalBlocker blockExposure(ui->exposureCompensationCombobox);
    QSignalBlocker blockQuad(ui->quadBayerComboBox);

    applyRenderOptionsToUi(*ui, settings.renderOptions);
    ui->draftQuality->setCurrentIndex(draftQualityIndex(settings.draftQuality));
    ui->cfrTarget->setCurrentText(QString::fromStdString(settings.cfrTarget));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(settings.cropTarget));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(settings.levels));
    ui->logTransformComboBox->setCurrentText("");
    ui->exposureCompensationCombobox->setCurrentText(QString::fromStdString(settings.exposureCompensation));
}

void MainWindow::applySettingsToMount(motioncam::MountId mountId, const RenderSettings& settings) {
    auto settingsCopy = settings;
    settingsCopy.cameraModel = mCameraModel;
    mFuseFilesystem->updateOptions(mountId, settingsCopy);
}

void MainWindow::applySettingsToMounts(const QSet<motioncam::MountId>& mountIds, const RenderSettings& settings) {
    for (auto mountId : mountIds) {
        applySettingsToMount(mountId, settings);
        updateThumbnailForMount(mountId);
    }
}

void MainWindow::applyEffectiveSettingsToAll() {
    for (const auto& mountedFile : mMountedFiles) {
        const auto settings = effectiveSettingsForMount(mountedFile.mountId);
        applySettingsToMount(mountedFile.mountId, settings);
        updateThumbnailForMount(mountedFile.mountId);
    }
}

void MainWindow::updateLocalBadgeForMount(motioncam::MountId mountId) {
    auto* fileWidget = findFileWidgetForMountId(mountId);
    if (!fileWidget) {
        return;
    }

    const bool hasLocal = mLocalSettings.contains(mountId);
    if (auto* reset = fileWidget->findChild<QPushButton*>("localReset")) {
        reset->setVisible(hasLocal);
    }
    auto* badge = fileWidget->findChild<QLabel*>("localBadge");
    if (badge) {
        badge->setVisible(hasLocal);
    }
    auto* globalBadge = fileWidget->findChild<QLabel*>("globalBadge");
    if (globalBadge) {
        globalBadge->setVisible(!hasLocal);
    }
}

void MainWindow::updateThumbnailForMount(motioncam::MountId mountId) {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return;
    }

    const auto widgets = scrollContent->findChildren<QWidget*>();
    for (auto* widget : widgets) {
        bool ok = false;
        auto widgetMountId = widget->property("mountId").toInt(&ok);
        if (!ok || widgetMountId != mountId) {
            continue;
        }

    auto* label = widget->findChild<QLabel*>("thumbnailLabel");
    if (!label) {
        return;
    }
    QString thumbPath = QDir::temp().filePath(QString("mcraw_thumb_%1.jpg").arg(mountId));
    if (mFuseFilesystem->generateThumbnail(mountId, thumbPath.toStdString(), 320, 240)) {
        QPixmap thumbnail(thumbPath);
        if (!thumbnail.isNull()) {
            label->setPixmap(thumbnail);
        }
    }
    }
}

void MainWindow::updateSelectionUi() {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return;
    }

    auto fileWidgets = scrollContent->findChildren<QWidget*>();
    for (auto* widget : fileWidgets) {
        bool ok = false;
        auto mountId = widget->property("mountId").toInt(&ok);
        if (!ok || mountId < 0) {
            continue;
        }
        setFileWidgetSelected(widget, mSelectedMountIds.contains(mountId));
    }
}

void MainWindow::updateCardIndices() {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    auto* scrollLayout = scrollContent ? qobject_cast<QVBoxLayout*>(scrollContent->layout()) : nullptr;
    if (!scrollLayout) {
        return;
    }

    int index = 1;
    for (int i = 0; i < scrollLayout->count(); ++i) {
        auto* widget = scrollLayout->itemAt(i)->widget();
        if (!widget) {
            continue;
        }
        bool ok = false;
        const auto mountId = widget->property("mountId").toInt(&ok);
        if (!ok || mountId < 0) {
            continue;
        }
        if (auto* label = widget->findChild<QLabel*>("indexLabel")) {
            label->setText(QString("%1.").arg(index, 2, 10, QChar('0')));
        }
        ++index;
    }
}

void MainWindow::applySelectionSettingsToUi() {
    if (mSelectedMountIds.isEmpty()) {
        return;
    }

    const auto mountIds = mSelectedMountIds.values();
    const RenderSettings firstSettings = effectiveSettingsForMount(mountIds.front());

    bool draftMixed = false;
    bool cfrMixed = false;
    bool cropMixed = false;
    bool levelsMixed = false;
    bool exposureMixed = false;
    bool quadMixed = false;

    bool draftModeMixed = false;
    bool vignetteMixed = false;
    bool vignetteColorMixed = false;
    bool scaleRawMixed = false;
    bool debugVignetteMixed = false;
    bool normalizeExposureMixed = false;
    bool cfrConversionMixed = false;
    bool cropEnableMixed = false;
    bool quadBayerMixed = false;

    const auto firstOptions = firstSettings.renderOptions;
    const bool firstDraftMode = firstOptions & motioncam::RENDER_OPT_DRAFT;
    const bool firstVignette = firstOptions & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION;
    const bool firstVignetteColor = firstOptions & motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR;
    const bool firstScaleRaw = firstOptions & motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP;
    const bool firstDebugVignette = firstOptions & motioncam::RENDER_OPT_DEBUG_SHADING_MAP;
    const bool firstNormalizeExposure = firstOptions & motioncam::RENDER_OPT_NORMALIZE_EXPOSURE;
    const bool firstCfr = firstOptions & motioncam::RENDER_OPT_FRAMERATE_CONVERSION;
    const bool firstCrop = firstOptions & motioncam::RENDER_OPT_CROPPING;
    const bool firstLog = false;
    const bool firstQuad = firstOptions & motioncam::RENDER_OPT_INTERPRET_AS_QUAD_BAYER;

    for (int i = 1; i < mountIds.size(); ++i) {
        const auto settings = effectiveSettingsForMount(mountIds[i]);
        const auto options = settings.renderOptions;

        draftMixed |= settings.draftQuality != firstSettings.draftQuality;
        cfrMixed |= settings.cfrTarget != firstSettings.cfrTarget;
        cropMixed |= settings.cropTarget != firstSettings.cropTarget;
        levelsMixed |= settings.levels != firstSettings.levels;
        exposureMixed |= settings.exposureCompensation != firstSettings.exposureCompensation;
        quadMixed |= settings.quadBayerOption != firstSettings.quadBayerOption;

        draftModeMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_DRAFT) != firstDraftMode);
        vignetteMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION) != firstVignette);
        vignetteColorMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR) != firstVignetteColor);
        scaleRawMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP) != firstScaleRaw);
        debugVignetteMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_DEBUG_SHADING_MAP) != firstDebugVignette);
        normalizeExposureMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_NORMALIZE_EXPOSURE) != firstNormalizeExposure);
        cfrConversionMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_FRAMERATE_CONVERSION) != firstCfr);
        cropEnableMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_CROPPING) != firstCrop);
        quadBayerMixed |= (static_cast<bool>(options & motioncam::RENDER_OPT_INTERPRET_AS_QUAD_BAYER) != firstQuad);
    }

    setCheckBoxMixedState(ui->draftModeCheckBox, draftModeMixed, firstDraftMode);
    setCheckBoxMixedState(ui->vignetteCorrectionCheckBox, vignetteMixed, firstVignette);
    setCheckBoxMixedState(ui->vignetteOnlyColorCheckBox, vignetteColorMixed, firstVignetteColor);
    setCheckBoxMixedState(ui->scaleRawCheckBox, scaleRawMixed, firstScaleRaw);
    setCheckBoxMixedState(ui->debugVignetteCheckBox, debugVignetteMixed, firstDebugVignette);
    setCheckBoxMixedState(ui->normalizeExposureCheckBox, normalizeExposureMixed, firstNormalizeExposure);
    setCheckBoxMixedState(ui->cfrConversionCheckBox, cfrConversionMixed, firstCfr);
    setCheckBoxMixedState(ui->cropEnableCheckBox, cropEnableMixed, firstCrop);
    setCheckBoxMixedState(ui->quadBayerCheckBox, quadBayerMixed, firstQuad);

    setComboMixedState(ui->draftQuality, draftMixed, QString::number(firstSettings.draftQuality));
    if (!draftMixed) {
        ui->draftQuality->setCurrentIndex(draftQualityIndex(firstSettings.draftQuality));
    }
    setComboMixedState(ui->cfrTarget, cfrMixed, QString::fromStdString(firstSettings.cfrTarget));
    setComboMixedState(ui->cropTargetComboBox, cropMixed, QString::fromStdString(firstSettings.cropTarget));
    setComboMixedState(ui->levelsComboBox, levelsMixed, QString::fromStdString(firstSettings.levels));
    setComboMixedState(ui->exposureCompensationCombobox, exposureMixed, QString::fromStdString(firstSettings.exposureCompensation));
    setComboMixedState(ui->quadBayerComboBox, quadMixed, QString::fromStdString(firstSettings.quadBayerOption));

    updateUi();
}

void MainWindow::setFileWidgetSelected(QWidget* fileWidget, bool selected) {
    if (!fileWidget) {
        return;
    }

    fileWidget->setProperty("selected", selected);
    if (auto* indicator = fileWidget->findChild<QLabel*>("selectIndicator")) {
        indicator->setText("");
        if (selected) {
            indicator->setStyleSheet("background-color: #f1c65a; border: 1px solid #f1c65a;");
        } else {
            indicator->setStyleSheet("background-color: transparent; border: 1px solid #3a5878;");
        }
        indicator->style()->unpolish(indicator);
        indicator->style()->polish(indicator);
        indicator->update();
    }
    const auto titleLabels = fileWidget->findChildren<QLabel*>();
    for (auto* label : titleLabels) {
        if (label->property("labelRole").toString() == "title") {
            label->setStyleSheet(selected ? "color: #f1c65a;" : "color: #e6e6e6;");
            label->style()->unpolish(label);
            label->style()->polish(label);
            label->update();
            break;
        }
    }
    fileWidget->style()->unpolish(fileWidget);
    fileWidget->style()->polish(fileWidget);
    fileWidget->update();
}

QWidget* MainWindow::findFileWidgetForMountId(motioncam::MountId mountId) const {
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return nullptr;
    }

    auto widgets = scrollContent->findChildren<QWidget*>();
    for (auto* widget : widgets) {
        bool ok = false;
        auto widgetMountId = widget->property("mountId").toInt(&ok);
        if (ok && widgetMountId == mountId) {
            return widget;
        }
    }

    return nullptr;
}

QWidget* MainWindow::findMountWidget(QObject* obj) const {
    auto* widget = qobject_cast<QWidget*>(obj);
    while (widget) {
        if (widget->property("mountId").isValid()) {
            return widget;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

void MainWindow::updateScopeUi() {
    const int selectedCount = mSelectedMountIds.size();
    const bool localActive = isLocalModeActive();
    int localOverrideCount = 0;
    for (auto mountId : mSelectedMountIds) {
        if (mLocalSettings.contains(mountId)) {
            ++localOverrideCount;
        }
    }

    // Build the combined title text
    QString titleText;
    if (localActive) {
        if (selectedCount > 0) {
            titleText = QString("%1 selected").arg(selectedCount);
        } else {
            titleText = "Settings (Selected)";
        }
    } else {
        titleText = "Settings (Global)";
    }

    const QString titleStyle = localActive
        ? "color: #f1c65a; font-size: 18pt; font-weight: 700;"
        : "color: #e6e6e6; font-size: 18pt; font-weight: 700;";
    ui->settingsTitle->setText(titleText);
    ui->settingsTitle->setStyleSheet(titleStyle);

    // Hide the separate scope and selection labels (keeping them for potential fallback)
    ui->settingsScopeLabel->setVisible(false);
    ui->selectedFilesLabel->setVisible(false);

    ui->applySelectedButton->setEnabled(mSettingsDirty && selectedCount > 0);
    ui->applyAllButton->setEnabled(mSettingsDirty && !mMountedFiles.isEmpty());
    updateOverrideMarkers();
}

void MainWindow::updateOverrideMarkers() {
    auto setMarker = [](QWidget* widget, bool enabled) {
        if (!widget) {
            return;
        }
        widget->setProperty("localOverride", enabled);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    };

    if (mSelectedMountIds.isEmpty()) {
        setMarker(ui->draftModeCheckBox, false);
        setMarker(ui->vignetteCorrectionCheckBox, false);
        setMarker(ui->vignetteOnlyColorCheckBox, false);
        setMarker(ui->scaleRawCheckBox, false);
        setMarker(ui->debugVignetteCheckBox, false);
        setMarker(ui->normalizeExposureCheckBox, false);
        setMarker(ui->cfrConversionCheckBox, false);
        setMarker(ui->cropEnableCheckBox, false);
        setMarker(ui->logTransformCheckBox, false);
        setMarker(ui->quadBayerCheckBox, false);

        setMarker(ui->draftQuality, false);
        setMarker(ui->cfrTarget, false);
        setMarker(ui->cropTargetComboBox, false);
        setMarker(ui->levelsComboBox, false);
        setMarker(ui->logTransformComboBox, false);
        setMarker(ui->exposureCompensationCombobox, false);
        setMarker(ui->quadBayerComboBox, false);
        return;
    }

    const RenderSettings globalSettings = globalSettingsFromState();
    const auto globalOptions = globalSettings.renderOptions;
    auto hasFlag = [](motioncam::FileRenderOptions options, motioncam::FileRenderOptions flag) {
        return (options & flag) != 0;
    };

    bool diffDraftMode = false;
    bool diffVignette = false;
    bool diffVignetteColor = false;
    bool diffScaleRaw = false;
    bool diffDebugVignette = false;
    bool diffNormalizeExposure = false;
    bool diffCfrConversion = false;
    bool diffCropEnable = false;
    bool diffQuadBayer = false;

    bool diffDraftQuality = false;
    bool diffCfrTarget = false;
    bool diffCropTarget = false;
    bool diffLevels = false;
    bool diffExposure = false;
    bool diffQuadBayerOption = false;

    for (auto mountId : mSelectedMountIds) {
        if (!mLocalSettings.contains(mountId)) {
            continue;
        }

        const auto localSettings = mLocalSettings.value(mountId);
        const auto localOptions = localSettings.renderOptions;

        diffDraftMode |= hasFlag(localOptions, motioncam::RENDER_OPT_DRAFT) != hasFlag(globalOptions, motioncam::RENDER_OPT_DRAFT);
        diffVignette |= hasFlag(localOptions, motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION) != hasFlag(globalOptions, motioncam::RENDER_OPT_APPLY_VIGNETTE_CORRECTION);
        diffVignetteColor |= hasFlag(localOptions, motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR) != hasFlag(globalOptions, motioncam::RENDER_OPT_VIGNETTE_ONLY_COLOR);
        diffScaleRaw |= hasFlag(localOptions, motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP) != hasFlag(globalOptions, motioncam::RENDER_OPT_NORMALIZE_SHADING_MAP);
        diffDebugVignette |= hasFlag(localOptions, motioncam::RENDER_OPT_DEBUG_SHADING_MAP) != hasFlag(globalOptions, motioncam::RENDER_OPT_DEBUG_SHADING_MAP);
        diffNormalizeExposure |= hasFlag(localOptions, motioncam::RENDER_OPT_NORMALIZE_EXPOSURE) != hasFlag(globalOptions, motioncam::RENDER_OPT_NORMALIZE_EXPOSURE);
        diffCfrConversion |= hasFlag(localOptions, motioncam::RENDER_OPT_FRAMERATE_CONVERSION) != hasFlag(globalOptions, motioncam::RENDER_OPT_FRAMERATE_CONVERSION);
        diffCropEnable |= hasFlag(localOptions, motioncam::RENDER_OPT_CROPPING) != hasFlag(globalOptions, motioncam::RENDER_OPT_CROPPING);
        diffQuadBayer |= hasFlag(localOptions, motioncam::RENDER_OPT_INTERPRET_AS_QUAD_BAYER) != hasFlag(globalOptions, motioncam::RENDER_OPT_INTERPRET_AS_QUAD_BAYER);

        diffDraftQuality |= localSettings.draftQuality != globalSettings.draftQuality;
        diffCfrTarget |= localSettings.cfrTarget != globalSettings.cfrTarget;
        diffCropTarget |= localSettings.cropTarget != globalSettings.cropTarget;
        diffLevels |= localSettings.levels != globalSettings.levels;
        diffExposure |= localSettings.exposureCompensation != globalSettings.exposureCompensation;
        diffQuadBayerOption |= localSettings.quadBayerOption != globalSettings.quadBayerOption;
    }

    setMarker(ui->draftModeCheckBox, diffDraftMode);
    setMarker(ui->vignetteCorrectionCheckBox, diffVignette);
    setMarker(ui->vignetteOnlyColorCheckBox, diffVignetteColor);
    setMarker(ui->scaleRawCheckBox, diffScaleRaw);
    setMarker(ui->debugVignetteCheckBox, diffDebugVignette);
    setMarker(ui->normalizeExposureCheckBox, diffNormalizeExposure);
    setMarker(ui->cfrConversionCheckBox, diffCfrConversion);
    setMarker(ui->cropEnableCheckBox, diffCropEnable);
    setMarker(ui->quadBayerCheckBox, diffQuadBayer);

    setMarker(ui->draftQuality, diffDraftQuality);
    setMarker(ui->cfrTarget, diffCfrTarget);
    setMarker(ui->cropTargetComboBox, diffCropTarget);
    setMarker(ui->levelsComboBox, diffLevels);
    setMarker(ui->exposureCompensationCombobox, diffExposure);
    setMarker(ui->quadBayerComboBox, diffQuadBayerOption);
}

bool MainWindow::isLocalModeActive() const {
    return !mSelectedMountIds.isEmpty();
}

void MainWindow::updateElidedTitle(QLabel* label) {
    if (!label) {
        return;
    }
    const auto fullText = label->property("fullTitle").toString();
    if (fullText.isEmpty()) {
        return;
    }
    const int availableWidth = std::max(0, label->width());
    if (availableWidth <= 0) {
        return;
    }
    const QFontMetrics metrics(label->font());
    label->setText(metrics.elidedText(fullText, Qt::ElideRight, availableWidth));
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->matches(QKeySequence::SelectAll)) {
        auto* scrollContent = ui->dragAndDropScrollArea->widget();
        auto* scrollLayout = scrollContent ? qobject_cast<QVBoxLayout*>(scrollContent->layout()) : nullptr;
        if (!scrollLayout) {
            return;
        }

        mSelectedMountIds.clear();
        for (int i = 0; i < scrollLayout->count(); ++i) {
            auto* widget = scrollLayout->itemAt(i)->widget();
            if (!widget) {
                continue;
            }
            bool ok = false;
            const auto mountId = widget->property("mountId").toInt(&ok);
            if (!ok || mountId < 0) {
                continue;
            }
            mSelectedMountIds.insert(mountId);
            setFileWidgetSelected(widget, true);
            if (mLastSelectedMountId < 0) {
                mLastSelectedMountId = mountId;
            }
        }

        if (!mSelectedMountIds.isEmpty()) {
            updateScopeUi();
            applySelectionSettingsToUi();
        } else {
            updateScopeUi();
        }
        event->accept();
        return;
    }

    QMainWindow::keyPressEvent(event);
}

void MainWindow::clearSelection() {
    if (mSelectedMountIds.isEmpty()) {
        return;
    }

    for (auto mountId : mSelectedMountIds) {
        if (auto* widget = findFileWidgetForMountId(mountId)) {
            setFileWidgetSelected(widget, false);
        }
    }
    mSelectedMountIds.clear();
    mLastSelectedMountId = -1;
    updateScopeUi();
}

int MainWindow::draftQualityFromUi() const {
    switch (ui->draftQuality->currentIndex()) {
    case 0:
        return 2;
    case 1:
        return 4;
    case 2:
        return 8;
    default:
        return 2;
    }
}

int MainWindow::draftQualityIndex(int draftQuality) const {
    switch (draftQuality) {
    case 2:
        return 0;
    case 4:
        return 1;
    case 8:
        return 2;
    default:
        return 0;
    }
}

void MainWindow::setComboMixedState(QComboBox* combo, bool mixed, const QString& value) {
    if (!combo) {
        return;
    }

    QSignalBlocker blocker(combo);
    auto* model = qobject_cast<QStandardItemModel*>(combo->model());
    int mixedIndex = combo->findData("MIXED_STATE", Qt::UserRole);
    if (mixed) {
        if (mixedIndex < 0) {
            combo->insertItem(0, "Mixed");
            combo->setItemData(0, "MIXED_STATE", Qt::UserRole);
            mixedIndex = 0;
        }
        if (model && mixedIndex >= 0) {
            if (auto* item = model->item(mixedIndex)) {
                item->setEnabled(false);
            }
        }
        combo->setCurrentIndex(mixedIndex);
        return;
    }

    if (mixedIndex >= 0) {
        combo->removeItem(mixedIndex);
    }

    if (!value.isEmpty()) {
        combo->setCurrentText(value);
    }
}

void MainWindow::setCheckBoxMixedState(QCheckBox* checkBox, bool mixed, bool checked) {
    if (!checkBox) {
        return;
    }

    QSignalBlocker blocker(checkBox);
    if (mixed) {
        checkBox->setTristate(true);
        checkBox->setCheckState(Qt::CheckState::PartiallyChecked);
    } else {
        checkBox->setTristate(false);
        checkBox->setCheckState(checked ? Qt::CheckState::Checked : Qt::CheckState::Unchecked);
    }
}

void MainWindow::syncGlobalsFromUi() {
    const auto settings = captureSettingsFromUi();
    mGlobalRenderOptions = settings.renderOptions;
    mDraftQuality = settings.draftQuality;
    mCFRTarget = settings.cfrTarget;
    mCropTarget = settings.cropTarget;
    mLevels = settings.levels;
    mLogTransform = settings.logTransform;
    mExposureCompensation = settings.exposureCompensation;
    mQuadBayerOption = settings.quadBayerOption;
}

void MainWindow::updateFpsLabels() {
    // Get the scroll area's content widget
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (!scrollContent) {
        return;
    }

    // Force recalculation of fps values by calling updateOptions for all mounted files
    for (const auto& mountedFile : mMountedFiles) {
        auto settings = effectiveSettingsForMount(mountedFile.mountId);
        settings.cameraModel = mCameraModel;
        mFuseFilesystem->updateOptions(mountedFile.mountId, settings);
    }
    
    // Find all fps labels in the scroll area
    auto fpsLabels = scrollContent->findChildren<QLabel*>();
    
    for (auto* label : fpsLabels) {
        // Check if this is an fps label by looking for the isFpsLabel property
        if (label->property("infoLabel").toBool()) {
            bool ok = false;
            auto mountId = label->property("mountId").toInt(&ok);
            
            if (ok && mountId >= 0) {
                // Get the updated fps value
                auto fileInfoOpt = mFuseFilesystem->getFileInfo(mountId);
                if (fileInfoOpt.has_value()) {
                    auto info = fileInfoOpt.value();
                    const int line = label->property("infoLine").toInt();
                    if (line == 2) {
                        const auto infoText = QString("Framecount: %1 | Dropped: -%2 | Duplicated: +%3 | Resolution: %4x%5")
                                              .arg(info.totalFrames)
                                              .arg(info.droppedFrames)
                                              .arg(info.duplicatedFrames)
                                              .arg(info.width)
                                              .arg(info.height);
                        label->setText(infoText);
                    } else {
                        const auto infoText = QString("Median / Average / Target FPS: %1 / %2 -> %3")
                                              .arg(QString::number(info.medFps, 'f', 2))
                                              .arg(QString::number(info.avgFps, 'f', 2))
                                              .arg(QString::number(info.fps, 'f', 2));
                        label->setText(infoText);
                    }
                }
            }
        }
    }
}

void MainWindow::onRenderSettingsChanged(const Qt::CheckState &checkState) {    
    if (!isLocalModeActive()) {
        mGlobalRenderOptions = getRenderOptions(*ui);
    }
    updateUi();
    markSettingsDirty(false);
}

void MainWindow::onResetNormalizeExposure() {
    ui->normalizeExposureCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->exposureCompensationCombobox->setCurrentText("0ev");
    if (!isLocalModeActive()) {
        mGlobalRenderOptions = getRenderOptions(*ui);
    }
    updateUi();
    markSettingsDirty(false);
}

void MainWindow::onResetCfr() {
    ui->cfrConversionCheckBox->setCheckState(Qt::CheckState::Checked);
    ui->cfrTarget->setCurrentText("Prefer Drop Frame");
    if (!isLocalModeActive()) {
        mGlobalRenderOptions = getRenderOptions(*ui);
    }
    updateUi();
    markSettingsDirty(false);
}

void MainWindow::onDraftModeQualityChanged(int index) {
    if (!isLocalModeActive()) {
        if(index == 0)
            mDraftQuality = 2;
        else if(index == 1)
            mDraftQuality = 4;
        else if(index == 2)
            mDraftQuality = 8;
    }

    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onCFRTargetChanged(std::string input) {
    if (!isLocalModeActive()) {
        mCFRTarget = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onCropTargetChanged(std::string input) {
    if (!isLocalModeActive()) {
        mCropTarget = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onLevelsChanged(std::string input) {
    if (!isLocalModeActive()) {
        mLevels = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onLogTransformChanged(std::string input) {
    if (!isLocalModeActive()) {
        mLogTransform = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onExposureCompensationChanged(std::string input) {
    if (!isLocalModeActive()) {
        mExposureCompensation = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onQuadBayerChanged(std::string input) {
    if (!isLocalModeActive()) {
        mQuadBayerOption = input;
    }
    onRenderSettingsChanged(Qt::CheckState::Checked);
}

void MainWindow::onCacheCleanup() {
    if (mFuseFilesystem) {
        mFuseFilesystem->cleanupCacheExpired();
    }
}

void MainWindow::applyCacheManagementSettings() {
    if (!mFuseFilesystem) {
        return;
    }

    mFuseFilesystem->setCachePolicy(mCachePolicy);
    if (mCachePolicy == motioncam::CachePolicy::Quota) {
        mFuseFilesystem->setCacheQuotaBytes(mCacheQuotaBytes);
    } else {
        mFuseFilesystem->setCacheQuotaBytes(0);
    }

    if (mCacheCleanupTimer) {
        if (mCachePolicy != motioncam::CachePolicy::Off && mCacheCleanupIntervalSeconds > 0) {
            mCacheCleanupTimer->start(mCacheCleanupIntervalSeconds * 1000);
        } else {
            mCacheCleanupTimer->stop();
        }
    }
}

void MainWindow::onSetDefaultSettings(bool checked) {
    const std::string defaultCfrTarget = "Prefer Drop Frame";
    const std::string defaultExposure = "0ev";
    const std::string defaultLevels = "Dynamic";
    const std::string defaultQuadBayer = "Wrong CFA Metadata";

    ui->draftModeCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteCorrectionCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->scaleRawCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->debugVignetteCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->vignetteOnlyColorCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->normalizeExposureCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->cfrConversionCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->cropEnableCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->quadBayerCheckBox->setCheckState(Qt::CheckState::Unchecked);
    ui->highQualityFirstFrameCheckBox->setCheckState(Qt::CheckState::Checked);

    ui->draftQuality->setCurrentIndex(0);
    ui->cfrTarget->setCurrentText(QString::fromStdString(defaultCfrTarget));
    ui->exposureCompensationCombobox->setCurrentText(QString::fromStdString(defaultExposure));
    ui->levelsComboBox->setCurrentText(QString::fromStdString(defaultLevels));
    ui->cropTargetComboBox->setCurrentText(QString::fromStdString(mCropTarget));
    ui->quadBayerComboBox->setCurrentText(QString::fromStdString(defaultQuadBayer));

    if (!isLocalModeActive()) {
        syncGlobalsFromUi();
    }

    updateUi();
}
void MainWindow::markSettingsDirty(bool autoApply) {
    // Nothing mounted: just clear UI affordances and bail.
    if (mMountedFiles.isEmpty()) {
        mSettingsDirty = false;
        ui->applySelectedButton->setEnabled(false);
        ui->applyAllButton->setEnabled(false);
        ui->applySelectedButton->setText("Apply to Selected");
        ui->applyAllButton->setText("Apply to All");
        stopApplyPulse();
        return;
    }

    if (!mSettingsDirty) {
        mSettingsDirty = true;
    }

    if (autoApply) {
        ui->applySelectedButton->setEnabled(false);
        ui->applyAllButton->setEnabled(false);
        ui->applySelectedButton->setText("Apply to Selected");
        ui->applyAllButton->setText("Apply to All");
        stopApplyPulse();
        queueAutoApplyAll();
        return;
    }

    ui->applySelectedButton->setEnabled(!mSelectedMountIds.isEmpty());
    ui->applyAllButton->setEnabled(!mMountedFiles.isEmpty());
    ui->applySelectedButton->setText("Apply to Selected *");
    ui->applyAllButton->setText("Apply to All *");
    startApplyPulse();
}

void MainWindow::queueAutoApplyAll() {
    if (mMountedFiles.isEmpty()) {
        return;
    }
    mAutoApplyPending = true;
    mAutoApplyTimer.start(150);
}

void MainWindow::startApplyPulse() {
    ui->applySelectedButton->setStyleSheet(
        "QPushButton { background-color: #d1a53a; color: #111111; }"
        "QPushButton:hover { background-color: #e1b64a; }");
    ui->applyAllButton->setStyleSheet(
        "QPushButton { background-color: #d1a53a; color: #111111; }"
        "QPushButton:hover { background-color: #e1b64a; }");
}

void MainWindow::stopApplyPulse() {
    ui->applySelectedButton->setStyleSheet(mApplySelectedButtonBaseStyle);
    ui->applyAllButton->setStyleSheet(mApplyAllButtonBaseStyle);
}

void MainWindow::applyRenderSettings() {
    applyEffectiveSettingsToAll();
    QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
}

void MainWindow::onApplySelected() {
    applyChanges(false);
}

void MainWindow::onApplyAll() {
    applyChanges(true);
}

void MainWindow::applyChanges(bool applyAll) {
    if (!mSettingsDirty || mMountedFiles.isEmpty()) {
        return;
    }
    if (!applyAll && mSelectedMountIds.isEmpty()) {
        return;
    }

    // Disable button and show loading state
    stopApplyPulse();
    ui->applySelectedButton->setEnabled(false);
    ui->applyAllButton->setEnabled(false);
    ui->applySelectedButton->setText("Applying...");
    ui->applyAllButton->setText("Applying...");
    ui->refreshProgressBar->setVisible(true);
    ui->refreshStatusLabel->setText("Refreshing DNGs...");

    // Apply settings
    const auto settings = captureSettingsFromUi();
    if (!applyAll) {
        applySettingsToMounts(mSelectedMountIds, settings);
        for (auto mountId : mSelectedMountIds) {
            mLocalSettings.insert(mountId, settings);
            updateLocalBadgeForMount(mountId);
        }
    } else {
        syncGlobalsFromUi();
        QSet<motioncam::MountId> mountIds;
        for (const auto& file : mMountedFiles) {
            mountIds.insert(file.mountId);
        }
        applySettingsToMounts(mountIds, settings);
        mLocalSettings.clear();
        for (const auto& file : mMountedFiles) {
            updateLocalBadgeForMount(file.mountId);
        }
    }

    QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);

    // Show completion after a delay (to allow files to refresh)
    QTimer::singleShot(2000, this, [this]() {
        mSettingsDirty = false;
        ui->applySelectedButton->setText("Apply to Selected");
        ui->applyAllButton->setText("Apply to All");
        stopApplyPulse();
        ui->refreshProgressBar->setVisible(false);
        ui->refreshStatusLabel->setText("Ready");
        updateScopeUi();

        // Clear status after 2 seconds
        QTimer::singleShot(2000, this, [this]() {
            ui->refreshStatusLabel->setText("");
        });
    });
}
void MainWindow::saveSessionToFile(const QString& filePath) {
    QJsonObject sessionObj;
    sessionObj["version"] = 1;
    sessionObj["appName"] = "MotionCam FS";

    // Save mounted files
    QJsonArray filesArray;
    for (const auto& file : mMountedFiles) {
        filesArray.append(file.srcFile);
    }
    sessionObj["files"] = filesArray;

    // Save settings
    QJsonObject settingsObj;
    settingsObj["cacheFolder"] = mCacheRootFolder;
    settingsObj["draftQuality"] = mDraftQuality;
    settingsObj["cfrTarget"] = QString::fromStdString(mCFRTarget);
    settingsObj["cropTarget"] = QString::fromStdString(mCropTarget);
    settingsObj["cameraModel"] = QString::fromStdString(mCameraModel);
    settingsObj["levels"] = QString::fromStdString(mLevels);
    settingsObj["logTransform"] = "";
    settingsObj["exposureCompensation"] = QString::fromStdString(mExposureCompensation);
    settingsObj["quadBayerOption"] = QString::fromStdString(mQuadBayerOption);

    // Save render options
    settingsObj["renderOptions"] = static_cast<int>(mGlobalRenderOptions);

    sessionObj["settings"] = settingsObj;

    // Save per-file local overrides
    QJsonArray localSettingsArray;
    for (auto it = mLocalSettings.constBegin(); it != mLocalSettings.constEnd(); ++it) {
        QString filePath;
        for (const auto& file : mMountedFiles) {
            if (file.mountId == it.key()) {
                filePath = file.srcFile;
                break;
            }
        }

        if (filePath.isEmpty()) {
            continue;
        }

        const auto& local = it.value();
        QJsonObject localSettingsObj;
        localSettingsObj["draftQuality"] = local.draftQuality;
        localSettingsObj["cfrTarget"] = QString::fromStdString(local.cfrTarget);
        localSettingsObj["cropTarget"] = QString::fromStdString(local.cropTarget);
        localSettingsObj["levels"] = QString::fromStdString(local.levels);
        localSettingsObj["logTransform"] = "";
        localSettingsObj["exposureCompensation"] = QString::fromStdString(local.exposureCompensation);
        localSettingsObj["quadBayerOption"] = QString::fromStdString(local.quadBayerOption);
        localSettingsObj["renderOptions"] = static_cast<int>(local.renderOptions);

        QJsonObject entry;
        entry["file"] = filePath;
        entry["settings"] = localSettingsObj;
        localSettingsArray.append(entry);
    }
    sessionObj["localSettings"] = localSettingsArray;

    // Write to file
    QJsonDocument doc(sessionObj);
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
        mCurrentSessionFile = filePath;
        addRecentSession(filePath);
    } else {
        QMessageBox::warning(this, "Save Session", "Failed to save session file: " + file.errorString());
    }
}

void MainWindow::loadSessionFromFile(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Load Session", "Failed to open session file: " + file.errorString());
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::warning(this, "Load Session", "Invalid session file format");
        return;
    }

    QJsonObject sessionObj = doc.object();

    QHash<QString, RenderSettings> localSettingsByPath;
    if (sessionObj.contains("localSettings")) {
        QJsonArray localSettingsArray = sessionObj["localSettings"].toArray();
        for (const auto& entryValue : localSettingsArray) {
            QJsonObject entryObj = entryValue.toObject();
            const QString filePath = entryObj["file"].toString();
            QJsonObject settingsObj = entryObj["settings"].toObject();

            if (filePath.isEmpty() || settingsObj.isEmpty()) {
                continue;
            }

            RenderSettings settings;
            settings.draftQuality = settingsObj["draftQuality"].toInt(2);
            settings.cfrTarget = settingsObj["cfrTarget"].toString().toStdString();
            settings.cropTarget = settingsObj["cropTarget"].toString().toStdString();
            settings.levels = settingsObj["levels"].toString().toStdString();
            settings.logTransform = "";
            settings.exposureCompensation = settingsObj["exposureCompensation"].toString().toStdString();
            settings.quadBayerOption = settingsObj["quadBayerOption"].toString().toStdString();
            settings.renderOptions = static_cast<motioncam::FileRenderOptions>(
                settingsObj["renderOptions"].toInt(static_cast<int>(motioncam::RENDER_OPT_NONE)));

            localSettingsByPath.insert(filePath, settings);
        }
    }

    clearCurrentSession(true);

    // Load settings
    if (sessionObj.contains("settings")) {
        QJsonObject settingsObj = sessionObj["settings"].toObject();

        mCacheRootFolder = settingsObj["cacheFolder"].toString();
        mDraftQuality = settingsObj["draftQuality"].toInt(2);
        mCFRTarget = settingsObj["cfrTarget"].toString().toStdString();
        mCropTarget = settingsObj["cropTarget"].toString().toStdString();
        mCameraModel = settingsObj["cameraModel"].toString().toStdString();
        mLevels = settingsObj["levels"].toString().toStdString();
        mLogTransform = "";
        mExposureCompensation = settingsObj["exposureCompensation"].toString().toStdString();
        mQuadBayerOption = settingsObj["quadBayerOption"].toString().toStdString();
        mMatrixOverrideEnabled = false;
        mMatrixProfile.clear();
        mMatrixFilePath.clear();

        if (settingsObj.contains("renderOptions")) {
            const auto renderOptions = static_cast<motioncam::FileRenderOptions>(
                settingsObj["renderOptions"].toInt(static_cast<int>(motioncam::RENDER_OPT_NONE)));
            mGlobalRenderOptions = renderOptions;
            applyRenderOptionsToUi(*ui, renderOptions);
        } else {
            mGlobalRenderOptions = getRenderOptions(*ui);
        }

        applySettingsToUi(globalSettingsFromState());

        // Don't call restoreSettings() here - it was already called in constructor
        // and we just manually loaded the settings from the session file
    }

    // Load files with progress dialog
    if (sessionObj.contains("files")) {
        QJsonArray filesArray = sessionObj["files"].toArray();

        if (filesArray.size() > 0) {
            QProgressDialog progress("Loading session files...", "Cancel", 0, filesArray.size(), this);
            progress.setWindowModality(Qt::WindowModal);
            progress.setMinimumDuration(0);

            for (int i = 0; i < filesArray.size(); ++i) {
                if (progress.wasCanceled()) {
                    break;
                }

                QString filePath = filesArray[i].toString();
                const qint64 fileBytes = estimateInputBytes(filePath);
                if (!mountFileWithProgress(filePath, progress, i, filesArray.size(), fileBytes)) {
                    break;
                }
            }

            progress.setValue(filesArray.size());
        }
    }

    if (!localSettingsByPath.isEmpty()) {
        for (const auto& file : mMountedFiles) {
            auto it = localSettingsByPath.constFind(file.srcFile);
            if (it == localSettingsByPath.constEnd()) {
                continue;
            }

            mLocalSettings.insert(file.mountId, it.value());
            applySettingsToMount(file.mountId, it.value());
            updateThumbnailForMount(file.mountId);
            updateLocalBadgeForMount(file.mountId);
        }
        QTimer::singleShot(100, this, &MainWindow::updateFpsLabels);
    }

    mCurrentSessionFile = filePath;
    addRecentSession(filePath);
    updateUi();
}

void MainWindow::onNewSession() {
    clearCurrentSession(true);
    mCurrentSessionFile.clear();

    const QString appDataDir = fuseAppDataRoot();
    const QString sessionFile = QDir(appDataDir).filePath("last_session.json");
    if (QFile::exists(sessionFile)) {
        QFile::remove(sessionFile);
    }

    updateUi();
}

void MainWindow::clearCurrentSession(bool showProgress) {
    const int totalFiles = mMountedFiles.size();
    QProgressDialog* clearProgress = nullptr;
    if (showProgress && totalFiles > 0) {
        clearProgress = new QProgressDialog("Clearing current session...", QString(), 0, totalFiles, this);
        clearProgress->setWindowModality(Qt::WindowModal);
        clearProgress->setMinimumDuration(0);
        clearProgress->setCancelButton(nullptr);
    }

    int i = 0;
    while (!mMountedFiles.isEmpty()) {
        if (clearProgress) {
            clearProgress->setValue(i);
            QApplication::processEvents();
        }

        auto& file = mMountedFiles.first();
        const QString mountPath = file.mountPath;
        try {
            mFuseFilesystem->unmount(file.mountId);
        } catch (const std::exception& e) {
            // Ignore unmount errors, file might not be mounted
        }
        deleteMountOutputIfRequested(mountPath);
        mMountedFiles.removeFirst();
        ++i;
    }

    if (clearProgress) {
        clearProgress->setValue(i);
    }

    if (totalFiles > 0) {
        // Give the filesystem time to clean up
        QThread::msleep(100);
    }

    // Update UI to show empty state
    auto* scrollContent = ui->dragAndDropScrollArea->widget();
    if (scrollContent) {
        QLayout* layout = scrollContent->layout();
        if (layout) {
            for (int i = layout->count() - 1; i >= 0; --i) {
                QLayoutItem* item = layout->itemAt(i);
                if (!item) {
                    continue;
                }

                if (QWidget* widget = item->widget()) {
                    if (widget == mDragAndDropLabel) {
                        continue;
                    }

                    if (widget->property("mountId").isValid() || qobject_cast<QFrame*>(widget)) {
                        layout->removeWidget(widget);
                        widget->deleteLater();
                    }
                }
            }
        }
    }

    if (mDragAndDropLabel) {
        mDragAndDropLabel->show();
        mDragAndDropLabel->raise();
    }

    mLocalSettings.clear();
    mSelectedMountIds.clear();
    updateScopeUi();

    QApplication::processEvents();

    delete clearProgress;
}

void MainWindow::autoSaveSession() {
    if (mMountedFiles.isEmpty()) {
        return; // Don't save empty sessions
    }

    const QString appDataDir = fuseAppDataRoot();
    QDir().mkpath(appDataDir);
    const QString sessionFile = QDir(appDataDir).filePath("last_session.json");
    const QString previousSessionFile = mCurrentSessionFile;
    saveSessionToFile(sessionFile);
    mCurrentSessionFile = previousSessionFile;
}

void MainWindow::onSaveSession() {
    if (!mCurrentSessionFile.isEmpty()) {
        saveSessionToFile(mCurrentSessionFile);
        QMessageBox::information(this, "Save Session", "Session saved successfully!");
        return;
    }

    const QString filePath = defaultSessionFilePath();
    saveSessionToFile(filePath);
    QMessageBox::information(this, "Save Session", "Session saved successfully!\n\n" + filePath);
}

void MainWindow::onLoadSession() {
    QString filePath = QFileDialog::getOpenFileName(this,
        "Load Session", sessionDirectory(),
        "MotionCam Session (*.json);;All Files (*)");

    if (!filePath.isEmpty()) {
        loadSessionFromFile(filePath);
    }
}

void MainWindow::onSaveSessionAs() {
    QString filePath = QFileDialog::getSaveFileName(this,
        "Save Session As", defaultSessionFilePath(),
        "MotionCam Session (*.json);;All Files (*)");

    if (!filePath.isEmpty()) {
        if (!filePath.endsWith(".json", Qt::CaseInsensitive)) {
            filePath += ".json";
        }
        saveSessionToFile(filePath);
        QMessageBox::information(this, "Save Session", "Session saved successfully!");
    }
}

void MainWindow::initSessionMenus() {
    mRecentSessionsMenu = new QMenu("Recent Sessions", this);
    mClearRecentSessionsAction = new QAction("Clear Recent Sessions", this);
    connect(mClearRecentSessionsAction, &QAction::triggered, this, &MainWindow::onClearRecentSessions);

    ui->menuFile->insertMenu(ui->actionOpenSettings, mRecentSessionsMenu);
    loadRecentSessions();
    updateRecentSessionsMenu();
}

void MainWindow::updateRecentSessionsMenu() {
    mRecentSessionsMenu->clear();
    if (mRecentSessions.isEmpty()) {
        auto* emptyAction = mRecentSessionsMenu->addAction("No recent sessions");
        emptyAction->setEnabled(false);
    } else {
        for (const auto& sessionPath : mRecentSessions) {
            QFileInfo info(sessionPath);
            const QString displayName = info.completeBaseName();
            auto* action = mRecentSessionsMenu->addAction(displayName);
            action->setToolTip(sessionPath);
            action->setData(sessionPath);
            connect(action, &QAction::triggered, this, [this, sessionPath]() {
                if (!QFile::exists(sessionPath)) {
                    mRecentSessions.removeAll(sessionPath);
                    saveRecentSessions();
                    updateRecentSessionsMenu();
                    QMessageBox::warning(this, "Load Session", "Session file not found:\n\n" + sessionPath);
                    return;
                }
                loadSessionFromFile(sessionPath);
            });
        }
    }

    mRecentSessionsMenu->addSeparator();
    mRecentSessionsMenu->addAction(mClearRecentSessionsAction);
}

void MainWindow::loadRecentSessions() {
    QSettings settings(PACKAGE_NAME, APP_NAME);
    mRecentSessions = settings.value("recentSessions").toStringList();
    mRecentSessions.removeAll(QString());
    for (int i = mRecentSessions.size() - 1; i >= 0; --i) {
        if (!QFile::exists(mRecentSessions[i])) {
            mRecentSessions.removeAt(i);
        }
    }
    const int maxRecent = 10;
    if (mRecentSessions.size() > maxRecent) {
        mRecentSessions = mRecentSessions.mid(0, maxRecent);
    }
}

void MainWindow::saveRecentSessions() const {
    QSettings settings(PACKAGE_NAME, APP_NAME);
    settings.setValue("recentSessions", mRecentSessions);
}

void MainWindow::addRecentSession(const QString& filePath) {
    if (isAutoSessionFile(filePath)) {
        return;
    }

    const QString normalized = QFileInfo(filePath).absoluteFilePath();
    mRecentSessions.removeAll(normalized);
    mRecentSessions.prepend(normalized);
    const int maxRecent = 10;
    if (mRecentSessions.size() > maxRecent) {
        mRecentSessions = mRecentSessions.mid(0, maxRecent);
    }
    saveRecentSessions();
    updateRecentSessionsMenu();
}

QString MainWindow::sessionDirectory() const {
    const QString appDataDir = fuseAppDataRoot();
    const QString sessionsDir = QDir(appDataDir).filePath("sessions");
    QDir().mkpath(sessionsDir);
    return sessionsDir;
}

QString MainWindow::defaultSessionFilePath() const {
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd_HHmm");
    QString baseName = QString("Session_%1").arg(timestamp);
    QString candidate = QDir(sessionDirectory()).filePath(baseName + ".json");
    int suffix = 1;
    while (QFile::exists(candidate)) {
        candidate = QDir(sessionDirectory()).filePath(QString("%1_%2.json").arg(baseName).arg(suffix++));
    }
    return candidate;
}

bool MainWindow::isAutoSessionFile(const QString& filePath) const {
    return QFileInfo(filePath).fileName().compare("last_session.json", Qt::CaseInsensitive) == 0;
}

void MainWindow::onClearRecentSessions() {
    mRecentSessions.clear();
    saveRecentSessions();
    updateRecentSessionsMenu();
}

void MainWindow::onOpenSettings() {
    SettingsDialog dialog(this);
    dialog.setCacheFolder(mCacheRootFolder);
    dialog.setPlayerPath(mPlayerPath);
    dialog.setCameraModel(QString::fromStdString(mCameraModel));
    dialog.setDeleteOnUnmount(mDeleteOnUnmount);
    if (mCachePolicy == motioncam::CachePolicy::Quota) {
        dialog.setCachePolicyMode("quota");
    } else if (mCachePolicy == motioncam::CachePolicy::Off) {
        dialog.setCachePolicyMode("off");
    } else {
        dialog.setCachePolicyMode("quota");
    }
    dialog.setCacheQuotaGb(static_cast<double>(mCacheQuotaBytes) / (1024.0 * 1024.0 * 1024.0));
    dialog.setCacheCleanupIntervalSeconds(mCacheCleanupIntervalSeconds);
    if (!mCacheRootFolder.isEmpty()) {
        QString warn;
        if (!isSupportedMountVolume(mCacheRootFolder, "DNG output folder", warn)) {
            dialog.setCacheFolderWarning(warn);
        }
    }

    if (dialog.exec() == QDialog::Accepted) {
        bool changed = false;
        QString newCacheFolder = dialog.getCacheFolder();
        if (newCacheFolder != mCacheRootFolder) {
            mCacheRootFolder = newCacheFolder;
            changed = true;
        }

        QString newPlayerPath = dialog.getPlayerPath();
        if (newPlayerPath != mPlayerPath) {
            mPlayerPath = newPlayerPath;
            changed = true;
        }

        const bool newDeleteOnUnmount = dialog.getDeleteOnUnmount();
        if (newDeleteOnUnmount != mDeleteOnUnmount) {
            mDeleteOnUnmount = newDeleteOnUnmount;
            changed = true;
        }

        const QString newPolicyMode = dialog.getCachePolicyMode();
        motioncam::CachePolicy newPolicy = motioncam::CachePolicy::Quota;
        if (newPolicyMode == "quota") {
            newPolicy = motioncam::CachePolicy::Quota;
        } else if (newPolicyMode == "off") {
            newPolicy = motioncam::CachePolicy::Off;
        }
        if (newPolicy != mCachePolicy) {
            mCachePolicy = newPolicy;
            changed = true;
        }

        const int newCleanupIntervalSeconds = dialog.getCacheCleanupIntervalSeconds();
        if (newCleanupIntervalSeconds != mCacheCleanupIntervalSeconds) {
            mCacheCleanupIntervalSeconds = newCleanupIntervalSeconds;
            changed = true;
        }

        const double newQuotaGb = dialog.getCacheQuotaGb();
        const std::uint64_t newQuotaBytes = static_cast<std::uint64_t>(newQuotaGb * 1024.0 * 1024.0 * 1024.0);
        if (newQuotaBytes != mCacheQuotaBytes) {
            mCacheQuotaBytes = newQuotaBytes;
            changed = true;
        }

        QString newCameraModel = dialog.getCameraModel();
        bool cameraModelChanged = false;
        if (newCameraModel.toStdString() != mCameraModel) {
            mCameraModel = newCameraModel.toStdString();
            cameraModelChanged = true;
            changed = true;
        }

        if (changed) {
            saveSettings();
            updateUi();
            applyCacheManagementSettings();

            // If camera model changed, apply render settings to update mounted files and regenerate thumbnails
            if (cameraModelChanged) {
                markSettingsDirty(true);
                applyRenderSettings();
            }

            markSettingsDirty(true);
        }
    }
}
