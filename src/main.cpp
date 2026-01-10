#include "mainwindow.h"
#include "SingleApplication.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QProcess>
#include <QTimer>
#include <QFileInfo>
#include <QDirIterator>
#include <QStandardPaths>
#include <QFile>
#include <QSplashScreen>
#include <QPixmap>
#include <QDateTime>
#include <QProcess>
#include <QPushButton>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include "CrashDebug.h"
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <exception>
#endif

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

#ifdef __APPLE__
namespace {
int gCrashFd = -1;

void writeCrashLine(const char* msg) {
    if (gCrashFd >= 0) {
        ::write(gCrashFd, msg, std::strlen(msg));
    }
}

void crashHandler(int sig) {
    writeCrashLine("\n==== MotionCamFuse crash ====\n");
    char buf[64];
    const int len = std::snprintf(buf, sizeof(buf), "Signal %d\n", sig);
    if (len > 0 && gCrashFd >= 0) {
        ::write(gCrashFd, buf, static_cast<size_t>(len));
    }
    motioncam::debug::dumpCrashContext(gCrashFd >= 0 ? gCrashFd : STDERR_FILENO);
    void* stack[64];
    const int frames = ::backtrace(stack, static_cast<int>(sizeof(stack) / sizeof(stack[0])));
    const int fd = gCrashFd >= 0 ? gCrashFd : STDERR_FILENO;
    ::backtrace_symbols_fd(stack, frames, fd);
    if (gCrashFd >= 0) {
        ::fsync(gCrashFd);
    }
    _Exit(128 + sig);
}

void installCrashHandler() {
    const QString logDir = QDir(QDir::homePath()).filePath("Library/Logs/MotionCam Tools");
    QDir().mkpath(logDir);
    const QString logPath = QDir(logDir).filePath("crash.txt");
    gCrashFd = ::open(logPath.toUtf8().constData(), O_CREAT | O_WRONLY | O_APPEND, 0644);

    struct sigaction sa {};
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_RESETHAND;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);

    std::set_terminate([]() {
        writeCrashLine("\n==== MotionCamFuse terminate ====\n");
        void* stack[64];
        const int frames = ::backtrace(stack, static_cast<int>(sizeof(stack) / sizeof(stack[0])));
        const int fd = gCrashFd >= 0 ? gCrashFd : STDERR_FILENO;
        ::backtrace_symbols_fd(stack, frames, fd);
        if (gCrashFd >= 0) {
            ::fsync(gCrashFd);
        }
        _Exit(1);
    });
}
} // namespace
#endif

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(context);
    const QByteArray localMsg = msg.toLocal8Bit();

    switch (type) {
    case QtDebugMsg:
        spdlog::debug("Qt: {}", localMsg.constData());
        break;
    case QtInfoMsg:
        spdlog::info("Qt: {}", localMsg.constData());
        break;
    case QtWarningMsg:
        spdlog::warn("Qt: {}", localMsg.constData());
        break;
    case QtCriticalMsg:
        spdlog::error("Qt: {}", localMsg.constData());
        break;
    case QtFatalMsg:
        spdlog::critical("Qt: {}", localMsg.constData());
        abort();
    }
}

#ifdef _WIN32
bool ensureProjectedFsAvailable()
{
    HMODULE lib = LoadLibraryW(L"projectedfslib.dll");
    if (lib) {
        FreeLibrary(lib);
        return true;
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList enableBatCandidates{
        QDir(appDir).filePath("Enable_ProjFS.bat"),
        QDir(appDir).filePath("../Enable_ProjFS.bat"),
        QDir(appDir).filePath("../../Enable_ProjFS.bat"),
    };
    QString enableBat;
    for (const auto& candidate : enableBatCandidates) {
        const QString cleaned = QDir::cleanPath(candidate);
        if (QFile::exists(cleaned)) {
            enableBat = cleaned;
            break;
        }
    }
    if (enableBat.isEmpty()) {
        enableBat = QDir::cleanPath(enableBatCandidates.back());
    }

    QDialog dialog;
    dialog.setWindowTitle("ProjectedFS Required");
    dialog.setModal(true);
    dialog.setMinimumWidth(520);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto* headerRow = new QHBoxLayout();
    auto* logoLabel = new QLabel(&dialog);
    QIcon appIcon(":/assets/app_icon.ico");
    QPixmap logo = appIcon.pixmap(48, 48);
    if (!logo.isNull()) {
        logoLabel->setPixmap(logo);
    }
    auto* title = new QLabel("Welcome to MotionCam Fuse", &dialog);
    title->setStyleSheet("font-size: 18px; font-weight: 600; color: #f0f0f0;");
    headerRow->addStretch();
    headerRow->addWidget(logoLabel);
    headerRow->addSpacing(10);
    headerRow->addWidget(title);
    headerRow->addStretch();
    layout->addLayout(headerRow);
    layout->addSpacing(10);

    auto* alertBox = new QLabel(&dialog);
    alertBox->setText("Projected File System is disabled or missing.");
    alertBox->setAlignment(Qt::AlignHCenter);
    alertBox->setStyleSheet(
        "font-size: 14px; color: #d1a53a; font-weight: 700;"
        "background-color: transparent; border: 2px solid #d1a53a; border-radius: 6px;"
        "padding: 8px 10px;");
    alertBox->setFixedWidth(380);
    auto* alertRow = new QHBoxLayout();
    alertRow->addStretch();
    alertRow->addWidget(alertBox);
    alertRow->addStretch();
    layout->addLayout(alertRow);

    auto* body = new QLabel(&dialog);
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    body->setText(
        "<div style='font-size: 13px; color: #d4d4d4;'>"
        "<p style='margin: 0 0 8px 0;'>"
        "MotionCam Fuse needs ProjectedFS to mount files."
        "</p>"
        "<p style='margin: 0;'>"
        "<b>Step 1:</b> Enable ProjectedFS (admin required)<br/>"
        "<b>Step 2:</b> Restart your PC"
        "</p>"
        "</div>");
    layout->addWidget(body);

    auto* enableButton = new QPushButton("Enable ProjectedFS (Admin)", &dialog);
    enableButton->setDefault(true);
    enableButton->setAutoDefault(true);
    enableButton->setMinimumHeight(46);
    enableButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    enableButton->setStyleSheet(
        "QPushButton { background-color: #d1a53a; color: #111111; font-weight: 700; border-radius: 6px; }"
        "QPushButton:hover { background-color: #e1b64a; }");
    layout->addWidget(enableButton);

    auto* auxRow = new QHBoxLayout();
    auxRow->setSpacing(8);
    auxRow->setContentsMargins(0, 4, 0, 0);
    auto* openButton = new QPushButton("Open Folder", &dialog);
    auto* restartButton = new QPushButton("Restart PC", &dialog);
    auto* quitButton = new QPushButton("Quit", &dialog);
    const int auxHeight = 30;
    openButton->setMinimumHeight(auxHeight);
    restartButton->setMinimumHeight(auxHeight);
    quitButton->setMinimumHeight(auxHeight);
    openButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    restartButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quitButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auxRow->addWidget(openButton);
    auxRow->addWidget(restartButton);
    auxRow->addWidget(quitButton);
    layout->addLayout(auxRow);

    if (!QFile::exists(enableBat)) {
        enableButton->setEnabled(false);
    }

    enum class Action { None, Enable, Open, Restart, Quit };
    Action action = Action::None;
    QObject::connect(enableButton, &QPushButton::clicked, &dialog, [&]() {
        action = Action::Enable;
        dialog.accept();
    });
    QObject::connect(openButton, &QPushButton::clicked, &dialog, [&]() {
        const QString scriptDir = QFileInfo(enableBat).absolutePath();
        QProcess::startDetached("explorer", { QDir::toNativeSeparators(scriptDir) });
    });
    QObject::connect(restartButton, &QPushButton::clicked, &dialog, [&]() {
        action = Action::Restart;
        dialog.accept();
    });
    QObject::connect(quitButton, &QPushButton::clicked, &dialog, [&]() {
        action = Action::Quit;
        dialog.reject();
    });

    dialog.exec();

    if (action == Action::Enable) {
        if (!QFile::exists(enableBat)) {
            QMessageBox::critical(nullptr, "ProjectedFS Setup",
                                  "Enable_ProjFS.bat was not found.\n\n"
                                  "Please make sure it is included in the app folder.");
            return 1;
        }
        const QString enablePath = QDir::toNativeSeparators(enableBat);
        const QString psCommand =
            QString("Start-Process -FilePath '%1' -Verb RunAs").arg(enablePath);
        const QStringList psArgs{
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            psCommand,
        };
        if (!QProcess::startDetached("powershell.exe", psArgs)) {
            QMessageBox::critical(nullptr, "ProjectedFS Setup",
                                  "Failed to launch Enable_ProjFS.bat.\n\n"
                                  "Please run it manually as Administrator.");
            return 1;
        }
        QMessageBox::information(nullptr, "ProjectedFS Setup",
                                 "A setup window has opened.\n\n"
                                 "When it finishes, restart your PC.");
    } else if (action == Action::Restart) {
        if (QMessageBox::question(nullptr, "Restart PC",
                                  "Restart now? Make sure all work is saved.",
                                  QMessageBox::Yes | QMessageBox::No,
                                  QMessageBox::No) == QMessageBox::Yes) {
            QProcess::startDetached("shutdown", { "/r", "/t", "0" });
        }
    }

    return false;
}
#endif

int main(int argc, char *argv[])
{
    SingleApplication app(argc, argv);

#ifdef __APPLE__
    installCrashHandler();
#endif

    // Set application properties
    app.setApplicationName("MotionCam Fuse");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("MotionCam");
#ifdef __APPLE__
    app.setWindowIcon(QIcon(":/assets/app_icon_mac.png"));
#else
    app.setWindowIcon(QIcon(":/assets/app_icon.png"));
#endif

#ifdef __APPLE__
    QFont appFont = app.font();
    if (appFont.pointSizeF() > 0) {
        appFont.setPointSizeF(appFont.pointSizeF() + 2.0);
        app.setFont(appFont);
    }
#endif

    // Load theme
    QFile themeFile(":qdarkstyle/dark/darkstyle.qss");

    if (themeFile.exists())   {
        themeFile.open(QFile::ReadOnly | QFile::Text);

        QTextStream ts(&themeFile);
        app.setStyleSheet(ts.readAll());
    }

    // Parse command line arguments
    QCommandLineParser parser;

    parser.setApplicationDescription("MotionCam Fuse");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add file option
    QCommandLineOption fileOption(QStringList() << "f" << "file",
                                  "Mount file on startup",
                                  "filename");

    parser.addOption(fileOption);
    parser.process(app);

    // Get file parameter if provided
    QString fileToMount;

    if (parser.isSet(fileOption)) {
        fileToMount = parser.value(fileOption);
    }

    // Check if another instance is running
    if (!app.listen()) {
        if (!fileToMount.isEmpty()) {
            QString message = QString("MOUNT_FILE:%1").arg(fileToMount);
            if (app.sendMessage(message)) {
                return 0; // Successfully sent message to existing instance
            }
        }

        QMessageBox::information(nullptr,
                                 "Application Already Running",
                                 "Another instance of the application is already running.");
        return 1;
    }

#ifdef _WIN32
    if (!ensureProjectedFsAvailable()) {
        return 1;
    }
#endif

    // Create main window (initializes spdlog on Windows)
    MainWindow window;
    qInstallMessageHandler(messageHandler);

    // Show window first so any dialogs appear in front of the app
    window.show();

#ifdef __APPLE__
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window,
        [&window](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) {
                return;
            }
            if (window.isVisible() && !window.isMinimized()) {
                return;
            }
            window.show();
            if (window.isMinimized()) {
                window.showNormal();
            }
            window.raise();
            window.activateWindow();
        });
#endif

    // Ask to resume previous session after showing the window
    const QString appData = qEnvironmentVariable("APPDATA");
    const QString baseDir = appData.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QDir(appData).filePath("MotionCam Tools/Fuse");
    const QString sessionFile = QDir(baseDir).filePath("last_session.json");
    if (QFile::exists(sessionFile) && fileToMount.isEmpty()) {
        QTimer::singleShot(0, &window, [&window, sessionFile]() {
            QMessageBox prompt(&window);
            prompt.setIcon(QMessageBox::Question);
            prompt.setWindowTitle("Resume Session");
            prompt.setText("Resume the last session or start a new one?");
            QPushButton* resumeButton = prompt.addButton("Resume", QMessageBox::AcceptRole);
            QPushButton* newButton = prompt.addButton("New Session", QMessageBox::RejectRole);
            prompt.setDefaultButton(resumeButton);
            prompt.exec();

            if (prompt.clickedButton() == resumeButton) {
                window.loadSessionFromFile(sessionFile);
            }
        });
    }

    // Handle messages from other instances
    QObject::connect(&app, &SingleApplication::messageReceived, &window,
        [&window](const QString &message) {
             if (message.startsWith("MOUNT_FILE:")) {
                 QString filePath = message.mid(11); // Remove "MOUNT_FILE:" prefix

                 window.mountFile(filePath);
                 window.show();
                 window.raise();
                 window.activateWindow();
             }
         });

    // Mount file if provided on startup
    if (!fileToMount.isEmpty()) {
        QTimer::singleShot(100, &window, [&window, fileToMount]() {
            window.mountFile(fileToMount);
        });
    }

    return app.exec();
}
