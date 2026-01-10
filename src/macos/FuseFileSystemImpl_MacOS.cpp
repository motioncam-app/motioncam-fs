#include "macos/FuseFileSystemImpl_MacOS.h"
#include "VirtualFileSystemImpl_MCRAW.h"
#include "CameraFrameMetadata.h"
#include "LRUCache.h"
#include "Utils.h"
#include <motioncam/Decoder.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <iostream>
#include <pwd.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#include <BS_thread_pool.hpp>
#if __has_include(<fuse_t/fuse_t.h>)
#include <fuse_t/fuse_t.h>
#elif __has_include(<fuse/fuse.h>)
#include <fuse/fuse.h>
#elif __has_include(<fuse3/fuse.h>)
#include <fuse3/fuse.h>
#else
#error "FUSE headers not found. Install macFUSE or provide FUSE headers."
#endif
#include <QDir>

// Logging
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace fs = boost::filesystem;

namespace motioncam {

constexpr auto CACHE_SIZE = 1024 * 1024 * 1024; // 1 GB cache size
constexpr auto IO_THREADS = 4;

namespace {

std::string getLogDirectory() {
    std::string logPath;

    const char* home = getenv("HOME");
    if (!home) {
        // Fallback to getpwuid if HOME is not set
        struct passwd* pw = getpwuid(getuid());
        home = pw->pw_dir;
    }

    logPath = std::string(home) + "/Library/Logs/MotionCam Tools";

    // Create directory if it doesn't exist
    std::filesystem::create_directories(logPath);

    return logPath;
}

void setupLogging() {
    try {
        std::string logDir = getLogDirectory();
        std::string logFile = logDir + "/fuse.txt";

        std::vector<spdlog::sink_ptr> sinks;

        // Console sink
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // Rotating file sink: max 5MB per file, keep 3 files
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logFile, 1024 * 1024 * 5, 3));

        auto logger = std::make_shared<spdlog::logger>("multi_sink", sinks.begin(), sinks.end());
        spdlog::set_default_logger(logger);

#ifdef NDEBUG
        spdlog::set_level(spdlog::level::info);
#else
        spdlog::set_level(spdlog::level::debug);
#endif

        spdlog::flush_on(spdlog::level::info);
    }
    catch (const spdlog::spdlog_ex& ex) {
        std::cerr << "Log initialization failed: " << ex.what() << std::endl;
    }
}

} // namespace

//

struct FuseContext {
    VirtualFileSystemImpl_MCRAW* fs;
    std::atomic_int nextFileHandle;
};

class Session {
public:
    Session(const std::string& srcFile, const std::string& dstPath, VirtualFileSystemImpl_MCRAW* fs);
    ~Session();

    void updateOptions(const RenderSettings& settings);
    FileInfo getFileInfo() const;

private:
    void init(VirtualFileSystemImpl_MCRAW* fs);

    void fuseMain(struct fuse_chan* ch, struct fuse* fuse);

    static void* fuseInit(struct fuse_conn_info* conn);
    static void fuseDestroy(void* privateData);
    static int fuseRelease(const char* path, struct fuse_file_info* fi);
    static int fuseGetattr(const char* path, struct stat* stbuf);
    static int fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi);
    static int fuseOpen(const char* path, struct fuse_file_info* fi);
    static int fuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi);

private:
    std::string mSrcFile;
    std::string mDstPath;
    std::unique_ptr<std::thread> mThread;
    VirtualFileSystemImpl_MCRAW* mFs;
    struct fuse_chan* mFuseCh;
    struct fuse* mFuse;

public:
    VirtualFileSystemImpl_MCRAW* getFileSystem() const { return mFs; }
};


Session::Session(const std::string& srcFile, const std::string& dstPath, VirtualFileSystemImpl_MCRAW* fs) :
    mSrcFile(srcFile),
    mDstPath(dstPath),
    mFs(fs),
    mFuseCh(nullptr),
    mFuse(nullptr)
{
    init(fs);
}

Session::~Session() {
    auto mountPoint = mDstPath;
    auto channel = mFuseCh;

    if(channel) {
        std::thread([mountPoint, channel]() {
            spdlog::debug("Unmounting {}", mountPoint);

            fuse_unmount(mountPoint.c_str(), channel);

            spdlog::debug("Umounted {}", mountPoint);
        }).detach();
    }

    mFuseCh = nullptr;
    mFuse = nullptr;

    if(mThread && mThread->joinable())
        mThread->join();

    QDir dst;

    if(!dst.rmdir(mDstPath.c_str()))
        spdlog::warn("Failed to remove {}", mDstPath);

    spdlog::debug("Exiting session for {}", mSrcFile);
}

void Session::init(VirtualFileSystemImpl_MCRAW* fs) {
    // FUSE operations structure
    struct fuse_operations ops = {};

    ops.init = fuseInit;
    ops.destroy = fuseDestroy;
    ops.release = fuseRelease;
    ops.getattr = fuseGetattr;
    ops.readdir = fuseReaddir;
    ops.open = fuseOpen;
    ops.read = fuseRead;

    auto addFuseArgs = [](struct fuse_args& args, bool minimal) {
        // argv[0] is required by macFUSE for argument parsing.
        fuse_opt_add_arg(&args, "MotionCam Fuse");
        // Read only
        fuse_opt_add_arg(&args, "-r");
        if (minimal) {
            return;
        }
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "nobrowse");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "rwsize=262144");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "nonamedattr");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "nomtime");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "noappledouble");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "noapplexattr");
    };

    auto createSession = [&](bool minimal, std::string& errorOut) -> bool {
        struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
        addFuseArgs(args, minimal);

        auto* context = new FuseContext();
        context->fs = fs;
        context->nextFileHandle = 0;

        struct fuse_chan* ch = fuse_mount(mDstPath.c_str(), &args);
        if (ch == nullptr) {
            const int err = errno;
            errorOut = std::string("Failed to mount FUSE (") + std::strerror(err) + ")";
            fuse_opt_free_args(&args);
            delete context;
            return false;
        }

        struct fuse* fuse = fuse_new(ch, &args, &ops, sizeof(ops), context);
        fuse_opt_free_args(&args);

        if (fuse == nullptr) {
            const int err = errno;
            errorOut = err == 0
                ? "Failed to create FUSE session (errno=0). Check macFUSE approval or mount options."
                : std::string("Failed to create FUSE session (") + std::strerror(err) + ")";
            fuse_unmount(mDstPath.c_str(), ch);
            delete context;
            return false;
        }

        mFuseCh = ch;
        mFuse = fuse;
        mThread = std::make_unique<std::thread>(&Session::fuseMain, this, ch, fuse);
        return true;
    };

    std::string errorMessage;
    if (!createSession(false, errorMessage)) {
        spdlog::warn("FUSE session creation failed with full options: {}", errorMessage);
        if (!createSession(true, errorMessage)) {
            throw std::runtime_error(
                "Failed to create FUSE session at " + mDstPath + " (error: " + errorMessage + ")");
        }
    }

}

void Session::updateOptions(const RenderSettings& settings) {
    mFs->updateOptions(settings);

    fuse_invalidate_path(mFuse, mDstPath.c_str());
}

FileInfo Session::getFileInfo() const {
    return mFs->getFileInfo();
}

void Session::fuseMain(struct fuse_chan* ch, struct fuse* fuse) {
    int res = fuse_loop_mt(fuse);

    fuse_destroy(fuse);

    spdlog::info("Fuse has exited with code {}", res);
}

FuseContext* fuseGetContext() {
    auto context = fuse_get_context();

    return reinterpret_cast<FuseContext*>(context->private_data);
}

void* Session::fuseInit(struct fuse_conn_info* conn) {
    auto context = fuse_get_context();

    return context->private_data;
}

void Session::fuseDestroy(void* privateData) {
    spdlog::debug("fuseDestroy() entering");

    auto* context = reinterpret_cast<FuseContext*>(privateData);

    if(context->fs)
        delete context->fs;

    context->fs = nullptr;
    context->nextFileHandle = INT_MIN;

    delete context;

    spdlog::debug("fuseDestroy() exiting");
}

int Session::fuseGetattr(const char* path, struct stat* stbuf) {
    spdlog::debug("fuse_get_attr(path: {})", path);

    memset(stbuf, 0, sizeof(struct stat));

    auto* context = fuseGetContext();
    std::string pathStr(path);

    // Root directory
    if (pathStr == "/" || pathStr == "//") {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;

        return 0;
    }
    else {
        auto entry = context->fs->findEntry(pathStr);

        if(!entry.has_value())
            return -ENOENT;

        if(entry->type == EntryType::DIRECTORY_ENTRY) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            stbuf->st_mtime = stbuf->st_ctime = time(NULL);
            stbuf->st_size = 4096;
        }
        else if(entry->type == EntryType::FILE_ENTRY) {
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;
            stbuf->st_size = entry->size;

            stbuf->st_mtime = stbuf->st_ctime = time(NULL);
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
        }

        return 0;
    }

    return -ENOENT;
}

int Session::fuseReaddir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    spdlog::debug("fuse_read_dir(path: {})", path);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    if(pathStr == "//" || pathStr == "/") {
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);

        auto files = context->fs->listFiles("/");
        for(auto& entry : files) {
            filler(buf, entry.getFullPath().c_str(), nullptr, 0);
        }

        return 0;
    }

    return -ENOENT;
}

int Session::fuseOpen(const char* path, struct fuse_file_info* fi) {
    spdlog::debug("fuse_open(path: {})", path);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    auto entry = context->fs->findEntry(pathStr);

    if(!entry.has_value())
        return -ENOENT;

    // Only allow read access
    if ((fi->flags & 3) != O_RDONLY)
        return -EACCES;


    // Set file handle
    fi->fh = ++context->nextFileHandle;

    return 0;
}

int Session::fuseRead(const char* path, char* buf, size_t size, off_t offset, struct fuse_file_info* fi) {
    spdlog::debug("fuse_read(path: {}, size: {}, offset: {})", path, size, offset);

    auto* context = fuseGetContext();
    std::string pathStr(path);

    auto entry = context->fs->findEntry(pathStr);

    if(!entry.has_value())
        return -ENOENT;

    return context->fs->readFile(
        entry.value(),
        offset,
        size,
        buf,
        [](auto a, auto b) {},
        false
        );
}

int Session::fuseRelease(const char* path, struct fuse_file_info* fi) {
    return 0;
}

//

FuseFileSystemImpl_MacOs::FuseFileSystemImpl_MacOs() :
    mNextMountId(0),
    mIoThreadPool(std::make_unique<BS::thread_pool>(IO_THREADS)),
    mProcessingThreadPool(std::make_unique<BS::thread_pool>()),
    mCache(std::make_unique<LRUCache>(CACHE_SIZE))
{
    setupLogging();
}

FuseFileSystemImpl_MacOs::~FuseFileSystemImpl_MacOs() {
    mMountedFiles.clear();

    // Wait for tasks to complete before we destroy ourselves
    mIoThreadPool->wait();

    mProcessingThreadPool->wait();

    spdlog::info("Destroying FuseFileSystemImpl_MacOs()");
}

void FuseFileSystemImpl_MacOs::setCachePolicy(CachePolicy policy) {
    mCachePolicy = policy;
}

void FuseFileSystemImpl_MacOs::setCacheQuotaBytes(std::uint64_t bytes) {        
    mCacheQuotaBytes = bytes;
}

void FuseFileSystemImpl_MacOs::cleanupCacheExpired() {
    if (mCache) {
        mCache->cleanupExpired();
    }
}

MountId FuseFileSystemImpl_MacOs::mount(
    const RenderSettings& settings,
    const std::string& srcFile,
    const std::string& dstPath) {
    fs::path srcPath(srcFile);
    std::string extension = srcPath.extension().string();

    spdlog::debug("Mounting file {} to {}", srcFile, dstPath);

    QDir dst(dstPath.c_str());

    if(!dst.exists()) {
        spdlog::info("Creating path {}", dstPath);

        if(!dst.mkpath(dstPath.c_str())) {
            spdlog::error("Could not create path {}", dstPath);

            throw std::runtime_error("Failed to create " + dstPath);
        }
    }
    else {
        QFileInfo dstInfo(QString::fromStdString(dstPath));
        if (dstInfo.exists() && !dstInfo.isDir()) {
            throw std::runtime_error("Mount path is not a directory: " + dstPath);
        }
        const QStringList entries = dst.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        if (!entries.isEmpty()) {
            throw std::runtime_error("Mount path is not empty. Choose an empty folder: " + dstPath);
        }
    }

    if(boost::iequals(extension, ".mcraw")) {
        auto mountId = mNextMountId++;

        void* stack_addr = nullptr;
        size_t stack_size = 0;

        try {
            // Extract base name from destination path
            fs::path dstPathObj(dstPath);
            std::string baseName = dstPathObj.filename().string();
            
            auto* fs =
                new VirtualFileSystemImpl_MCRAW(
                    *mIoThreadPool,
                    *mProcessingThreadPool,
                    *mCache,
                    settings.renderOptions,
                    settings.draftQuality,
                    settings.cfrTarget,
                    settings.cropTarget,
                    srcFile,
                    baseName,
                    settings.cameraModel,
                    settings.levels,
                    std::string{},
                    settings.exposureCompensation,
                    std::string{},
                    settings.matrixOverrideEnabled,
                    settings.matrixProfile,
                    settings.matrixFilePath
                );

            auto session = std::make_unique<Session>(srcFile, dstPath, fs);

            if(!session) {
                spdlog::error("Failed to mount {} to {}", srcFile, dstPath);

                throw std::runtime_error("Failed to session");
            }

            mMountedFiles[mountId] = std::move(session);
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to mount {} to {} (error: {})", srcFile, dstPath, e.what());

            throw std::runtime_error(e.what());
        }

        return mountId;
    }

    spdlog::error("Failed to mount {} to {}, invalid file format", srcFile, dstPath);

    throw std::runtime_error("Invalid format");
}

void FuseFileSystemImpl_MacOs::unmount(MountId mountId) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        auto session = std::move(it->second);
        mMountedFiles.erase(it);
        std::thread([session = std::move(session)]() mutable {
            session.reset();
        }).detach();
    }
}

void FuseFileSystemImpl_MacOs::updateOptions(
    MountId mountId,
    const RenderSettings& settings) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        it->second->updateOptions(settings);
    }
}

std::optional<FileInfo> FuseFileSystemImpl_MacOs::getFileInfo(MountId mountId) {
    auto it = mMountedFiles.find(mountId);
    if(it != mMountedFiles.end()) {
        return it->second->getFileInfo();
    }
    return std::nullopt;
}

bool FuseFileSystemImpl_MacOs::generateThumbnail(MountId mountId, const std::string& outputPath, int width, int height) {
    auto it = mMountedFiles.find(mountId);
    if(it == mMountedFiles.end()) {
        spdlog::error("generateThumbnail(): Invalid mount ID {}", mountId);
        return false;
    }

    try {
        auto* session = dynamic_cast<Session*>(it->second.get());
        auto* fs = session->getFileSystem();
        const std::string& srcPath = fs->getSourcePath();

        Decoder decoder(srcPath);
        auto frames = decoder.getFrames();
        if(frames.empty()) {
            spdlog::error("generateThumbnail(): No frames in {}", srcPath);
            return false;
        }

        std::sort(frames.begin(), frames.end());
        const auto timestamp = frames[0];

        std::vector<uint8_t> data;
        nlohmann::json metadata;
        decoder.loadFrame(timestamp, data, metadata);

        auto frameMetadata = CameraFrameMetadata::parse(metadata);
        auto cameraConfiguration = CameraConfiguration::parse(decoder.getContainerMetadata());

        const std::string& levels = fs->getLevels();
        const std::string& exposureComp = fs->getExposureCompensation();

        return utils::generateJpegThumbnail(
            data,
            frameMetadata,
            cameraConfiguration,
            outputPath,
            width,
            height,
            levels,
            exposureComp);
    }
    catch(const std::exception& e) {
        spdlog::error("generateThumbnail(): Exception: {}", e.what());
        return false;
    }
}

} // namespace motioncam
