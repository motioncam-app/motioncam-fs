#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "IFuseFileSystem.h"

#include <QMainWindow>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QPoint>
#include <QTimer>
#include <QThreadPool>
#include <cstdint>
#include <utility>

namespace motioncam {
    struct MountedFile {
        MountedFile(MountId mountId, QString srcFile, QString mountPath) :
            mountId(mountId), srcFile(std::move(srcFile)), mountPath(std::move(mountPath))
        {}

        // Copy constructor
        MountedFile(const MountedFile& other) :
            mountId(other.mountId), srcFile(other.srcFile), mountPath(other.mountPath)
        {}

        const MountId mountId;
        const QString srcFile;
        const QString mountPath;

        // Copy assignment operator
        MountedFile& operator=(const MountedFile& other) {
            if (this != &other) {
                // Use const_cast to modify const members
                const_cast<MountId&>(mountId) = other.mountId;
                const_cast<QString&>(srcFile) = other.srcFile;
                const_cast<QString&>(mountPath) = other.mountPath;
            }
            return *this;
        }

        // Move assignment operator
        MountedFile& operator=(MountedFile&& other) noexcept {
            if (this != &other) {
                // Use const_cast to modify const members
                const_cast<MountId&>(mountId) = std::move(other.mountId);
                const_cast<QString&>(srcFile) = std::move(other.srcFile);
                const_cast<QString&>(mountPath) = std::move(other.mountPath);
            }
            return *this;
        }
    };
}

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QLabel;
class QAction;
class QMenu;
class QProgressDialog;
class QComboBox;
class QCheckBox;
class QTimer;
class QFileInfo;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void mountFile(const QString& filePath);
    void loadSessionFromFile(const QString& filePath);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void onRenderSettingsChanged(const Qt::CheckState &state);
    void onDraftModeQualityChanged(int index);
    void onCFRTargetChanged(std::string input);
    void onLevelsChanged(std::string input);
    void onCropTargetChanged(std::string input);
    void onLogTransformChanged(std::string input);
    void onExposureCompensationChanged(std::string input);
    void onQuadBayerChanged(std::string input);
    void onSetDefaultSettings(bool checked);
    void onApplySelected();
    void onApplyAll();
    void onCacheCleanup();

    void playFile(const QString& path);
    void playMountedFolder(QWidget* fileWidget);
    void openMountedDirectory(QWidget* fileWidget);
    void removeFile(QWidget* fileWidget);

    void onNewSession();
    void onSaveSession();
    void onLoadSession();
    void onSaveSessionAs();
    void onOpenSettings();
    void onClearRecentSessions();
    void onResetNormalizeExposure();
    void onResetCfr();

private:
    using RenderSettings = motioncam::RenderSettings;

    bool mountFileBackend(const QString& filePath, motioncam::MountId& mountId, QString& errorMessage);
    void addMountedFileUi(const QString& filePath, motioncam::MountId mountId, const QString& mountPath);
    bool mountFileWithProgress(const QString& filePath, QProgressDialog& progress, int index, int total, qint64 fileBytes);
    QString mountDestinationPath(const QFileInfo& fileInfo) const;
    void saveSettings();
    void restoreSettings();
    void updateUi();
    void updateFpsLabels();
    bool handleAlreadyMounted(const QString& filePath);
    void markSettingsDirty(bool autoApply = false);
    void applyRenderSettings();
    void startApplyPulse();
    void stopApplyPulse();

    RenderSettings captureSettingsFromUi() const;
    RenderSettings globalSettingsFromState() const;
    RenderSettings effectiveSettingsForMount(motioncam::MountId mountId) const;
    void applySettingsToUi(const RenderSettings& settings);
    void applySettingsToMount(motioncam::MountId mountId, const RenderSettings& settings);
    void applySettingsToMounts(const QSet<motioncam::MountId>& mountIds, const RenderSettings& settings);
    void applyEffectiveSettingsToAll();
    void applyChanges(bool applyAll);
    void applyCacheManagementSettings();
    void updateOverrideMarkers();
    void updateLocalBadgeForMount(motioncam::MountId mountId);
    void updateThumbnailForMount(motioncam::MountId mountId);
    void updateSelectionUi();
    void applySelectionSettingsToUi();
    void setFileWidgetSelected(QWidget* fileWidget, bool selected);
    QWidget* findFileWidgetForMountId(motioncam::MountId mountId) const;
    QWidget* findMountWidget(QObject* obj) const;
    void updateCardIndices();
    void deleteMountOutputIfRequested(const QString& mountPath);
    void updateElidedTitle(QLabel* label);
    void updateScopeUi();
    bool isLocalModeActive() const;
    void clearSelection();
    int draftQualityFromUi() const;
    int draftQualityIndex(int draftQuality) const;
    void setComboMixedState(QComboBox* combo, bool mixed, const QString& value);
    void setCheckBoxMixedState(QCheckBox* checkBox, bool mixed, bool checked);
    void syncGlobalsFromUi();
    void refreshMatrixProfiles() {}
    QStringList loadMatrixProfilesFromFile(const QString& path) const { Q_UNUSED(path); return {}; }
    QString defaultMatrixFilePath() const { return QString(); }

    void clearCurrentSession(bool showProgress);
    void saveSessionToFile(const QString& filePath);
    void autoSaveSession();
    void initSessionMenus();
    void updateRecentSessionsMenu();
    void loadRecentSessions();
    void saveRecentSessions() const;
    void addRecentSession(const QString& filePath);
    QString sessionDirectory() const;
    QString defaultSessionFilePath() const;
    bool isAutoSessionFile(const QString& filePath) const;
    void queueAutoApplyAll();
#ifdef __APPLE__
    void cleanupStaleMacFuseMounts();
    void forceUnmountAllMacFuseMounts();
#endif

private:
    Ui::MainWindow *ui;
    std::unique_ptr<motioncam::IFuseFileSystem> mFuseFilesystem;
    QList<motioncam::MountedFile> mMountedFiles;
    QHash<motioncam::MountId, RenderSettings> mLocalSettings;
    QSet<motioncam::MountId> mSelectedMountIds;
    QPoint mSelectionPressPos;
    QWidget* mSelectionPressWidget;
    bool mSelectionPressOnSelectableLabel;
    bool mSelectionPressToggle;
    bool mSelectionPressAllowSelection;
    bool mSelectionPressSuppress;
    bool mSelectionDragActive;
    Qt::KeyboardModifiers mSelectionPressModifiers;
    motioncam::MountId mLastSelectedMountId;
    motioncam::FileRenderOptions mGlobalRenderOptions;
    QString mCacheRootFolder;
    int mDraftQuality;
    std::string mCFRTarget;
    std::string mCropTarget;
    std::string mCameraModel;
    std::string mLevels;
    std::string mLogTransform;
    std::string mExposureCompensation;
    std::string mQuadBayerOption;
    bool mMatrixOverrideEnabled;
    std::string mMatrixProfile;
    QString mMatrixFilePath;
    QStringList mMatrixProfiles;
    bool mSettingsDirty;
    QTimer mAutoApplyTimer;
    bool mAutoApplyPending;
    QString mCurrentSessionFile;
    QString mPlayerPath;
    QPointer<QLabel> mDragAndDropLabel;
    QString mApplySelectedButtonBaseStyle;
    QString mApplyAllButtonBaseStyle;
    QMenu* mRecentSessionsMenu;
    QAction* mClearRecentSessionsAction;
    QTimer* mCacheCleanupTimer;
    QStringList mRecentSessions;
    motioncam::CachePolicy mCachePolicy;
    int mCacheCleanupIntervalSeconds;
    std::uint64_t mCacheQuotaBytes;
    bool mDeleteOnUnmount;
#ifdef __APPLE__
    QThreadPool mThumbnailPool;
#endif
};

#endif // MAINWINDOW_H
