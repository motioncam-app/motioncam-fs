#include "VirtualFileSystemImpl_MCRAW.h"
#include "CameraFrameMetadata.h"
#include "CameraMetadata.h"
#include "Utils.h"
#include "AudioWriter.h"
#include "LRUCache.h"

#include <motioncam/Decoder.hpp>

#include <boost/filesystem.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>

#include <BS_thread_pool.hpp>
#include <spdlog/spdlog.h>
#include <audiofile/AudioFile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <limits>
#include <sstream>
#include <tuple>

namespace motioncam {

namespace {

#ifdef _WIN32
    constexpr std::string_view DESKTOP_INI = R"([.ShellClassInfo]
ConfirmFileOp=0

[ViewState]
Mode=4
Vid={137E7700-3573-11CF-AE69-08002B2E1262}
FolderType=Generic

[{5984FFE0-28D4-11CF-AE66-08002B2E1262}]
Mode=4
LogicalViewMode=1
IconSize=16

[LocalizedFileNames]
)";

#endif

    std::string extractFilenameWithoutExtension(const std::string& fullPath) {
        boost::filesystem::path p(fullPath);
        return p.stem().string();
    }

    struct FrameRateInfo {
        float medianFrameRate;
        float averageFrameRate;
    };

    using MatrixOverrideProfile = VirtualFileSystemImpl_MCRAW::MatrixOverrideProfile;

    bool parseMatrixArray(const nlohmann::json& value, std::array<float, 9>& outMatrix) {
        if (!value.is_array() || value.size() != 9) {
            return false;
        }
        for (size_t i = 0; i < 9; ++i) {
            outMatrix[i] = value[i].get<float>();
        }
        return true;
    }

    std::optional<MatrixOverrideProfile> loadMatrixOverrideProfile(const std::string& filePath, const std::string& profileName) {
        if (filePath.empty() || profileName.empty()) {
            return std::nullopt;
        }

        std::ifstream stream(filePath);
        if (!stream.is_open()) {
            spdlog::warn("Matrix override enabled, but file not found: {}", filePath);
            return std::nullopt;
        }

        nlohmann::json json;
        try {
            stream >> json;
        } catch (const std::exception& e) {
            spdlog::warn("Failed to parse matrix file {} ({})", filePath, e.what());
            return std::nullopt;
        }

        auto parseProfileObject = [](const nlohmann::json& obj) -> MatrixOverrideProfile {
            MatrixOverrideProfile profile{};
            profile.hasColor1 = parseMatrixArray(obj.value("colorMatrix1", nlohmann::json::array()), profile.colorMatrix1);
            profile.hasColor2 = parseMatrixArray(obj.value("colorMatrix2", nlohmann::json::array()), profile.colorMatrix2);
            profile.hasForward1 = parseMatrixArray(obj.value("forwardMatrix1", nlohmann::json::array()), profile.forwardMatrix1);
            profile.hasForward2 = parseMatrixArray(obj.value("forwardMatrix2", nlohmann::json::array()), profile.forwardMatrix2);
            profile.hasCalibration1 = parseMatrixArray(obj.value("calibrationMatrix1", nlohmann::json::array()), profile.calibrationMatrix1);
            profile.hasCalibration2 = parseMatrixArray(obj.value("calibrationMatrix2", nlohmann::json::array()), profile.calibrationMatrix2);
            return profile;
        };

        std::string loweredProfile = profileName;
        std::transform(loweredProfile.begin(), loweredProfile.end(), loweredProfile.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        // New format: { "profiles": [ { "id": "...", ... } ] }
        if (json.contains("profiles") && json["profiles"].is_array()) {
            for (const auto& item : json["profiles"]) {
                if (!item.is_object()) {
                    continue;
                }
                const std::string id = item.value("id", "");
                std::string loweredId = id;
                std::transform(loweredId.begin(), loweredId.end(), loweredId.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (!id.empty() && loweredId == loweredProfile) {
                    auto profile = parseProfileObject(item);
                    if (!profile.hasColor1 && !profile.hasColor2) {
                        spdlog::warn("Matrix profile '{}' has no valid color matrices", profileName);
                        return std::nullopt;
                    }
                    return profile;
                }
            }
            spdlog::warn("Matrix profile '{}' not found in {}", profileName, filePath);
            return std::nullopt;
        }

        // Legacy format: nested brand/model
        auto matricesIt = json.find("cinema_camera_color_matrices");
        if (matricesIt != json.end() && matricesIt->is_object()) {
            const auto& matricesObj = *matricesIt;
            const auto slashPos = loweredProfile.find('/');
            if (slashPos == std::string::npos) {
                spdlog::warn("Matrix profile '{}' is invalid. Expected brand/model", profileName);
                return std::nullopt;
            }
            const std::string brand = loweredProfile.substr(0, slashPos);
            const std::string model = loweredProfile.substr(slashPos + 1);
            auto brandIt = matricesObj.find(brand);
            if (brandIt != matricesObj.end() && brandIt->is_object()) {
                auto modelIt = brandIt->find(model);
                if (modelIt != brandIt->end() && modelIt->is_object()) {
                    auto profile = parseProfileObject(*modelIt);
                    if (!profile.hasColor1 && !profile.hasColor2) {
                        spdlog::warn("Matrix profile '{}' has no valid color matrices", profileName);
                        return std::nullopt;
                    }
                    return profile;
                }
            }
            spdlog::warn("Matrix profile '{}/{}' not found in {}", brand, model, filePath);
        } else {
            spdlog::warn("Matrix file {} missing expected keys", filePath);
        }
        return std::nullopt;
    }

    FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
        // Need at least 2 frames to calculate frame rate
        if (frames.size() < 2) {
            return {0.0f, 0.0f};
        }

        // Use running average to prevent overflow
        double avgDuration = 0.0;
        int validFrames = 0;
        std::vector<double> durations;  // Store all valid durations for median calculation

        for (size_t i = 1; i < frames.size(); ++i) {
            double duration = static_cast<double>(frames[i] - frames[i-1]);

            if (duration > 0) {
                // Update running average
                // new_avg = old_avg + (new_value - old_avg) / (count + 1)
                avgDuration = avgDuration + (duration - avgDuration) / (validFrames + 1);
                durations.push_back(duration);  // Store duration for median calculation
                validFrames++;
            }
        }

        // Calculate median duration
        double medianDuration = 0.0;
        if (!durations.empty()) {
            std::sort(durations.begin(), durations.end());
            size_t mid = durations.size() / 2;
            if (durations.size() % 2 == 0) {
                // Even number of elements - average of two middle values
                medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
            } else {
                // Odd number of elements - middle value
                medianDuration = durations[mid];
            }
        }

        if (validFrames == 0) {
            return {0.0f, 0.0f};
        }

        return {
            static_cast<float>(1000000000.0 / medianDuration),
            static_cast<float>(1000000000.0 / avgDuration)
        };
    }

    int64_t getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
        if (frameRate <= 0) {
            return -1; // Invalid frame rate
        }

        int64_t timeDifference = timestamp - referenceTimestamp;
        if (timeDifference < 0) {
            return -1;
        }

        // Calculate microseconds per frame
        double nanosecondsPerFrame = 1000000000.0 / frameRate;

        // Calculate expected frame number
        return static_cast<int64_t>(std::round(timeDifference / nanosecondsPerFrame));
    }

    std::string constructFrameFilename(
        const std::string& baseName, int frameNumber, int padding = 6, const std::string& extension = "")
    {
        std::ostringstream oss;

        // Add the base name
        oss << baseName;

        // Add the zero-padded frame number
        oss << std::setfill('0') << std::setw(padding) << frameNumber;

        // Add the extension if provided
        if (!extension.empty()) {
            // Check if extension already has a dot prefix
            if (extension[0] != '.') {
                oss << '.';
            }
            oss << extension;
        }

        return oss.str();
    }

    void syncAudio(Timestamp videoTimestamp, std::vector<AudioChunk>& audioChunks, int sampleRate, int numChannels) {
        if(audioChunks.empty())
            return;
            
        // Calculate drift between the video and audio
        auto audioVideoDriftMs = (audioChunks[0].timestamp - videoTimestamp) * 1e-6f;
        if(std::abs(audioVideoDriftMs) > 1000) {
            spdlog::warn("Audio drift too large, not syncing audio");
            return;
        }

        AudioSampleFormat format = audioChunks[0].format;

        if(audioVideoDriftMs > 0) {
            // Calculate how many audio frames to remove
            int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
            int samplesToRemove = audioFramesToRemove * numChannels;

            // Remove samples from the beginning of audio chunks
            int samplesRemoved = 0;
            auto it = audioChunks.begin();

            while(it != audioChunks.end() && samplesRemoved < samplesToRemove) {
                int remainingSamplesToRemove = samplesToRemove - samplesRemoved;
                size_t chunkSize = it->sampleCount();

                if(chunkSize <= remainingSamplesToRemove) {
                    // Remove entire chunk
                    samplesRemoved += chunkSize;
                    it = audioChunks.erase(it);
                }
                else {
                    // Trim partial chunk from the beginning
                    if(format == AudioSampleFormat::Float32) {
                        it->float32Data.erase(it->float32Data.begin(), it->float32Data.begin() + remainingSamplesToRemove);
                    } else {
                        it->int16Data.erase(it->int16Data.begin(), it->int16Data.begin() + remainingSamplesToRemove);
                    }

                    // Update timestamp for the trimmed chunk
                    it->timestamp += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                    break;
                }
            }
        }
        else {
            // Otherwise video starts before audio, add silence
            auto silenceDuration = -audioVideoDriftMs; // Make positive

            int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
            int silenceSamples = silenceFrames * numChannels;

            // Create silence chunk at the beginning
            AudioChunk silenceChunk;
            silenceChunk.timestamp = videoTimestamp;
            silenceChunk.format = format;
            
            if(format == AudioSampleFormat::Float32) {
                silenceChunk.float32Data.resize(silenceSamples, 0.0f);
            } else {
                silenceChunk.int16Data.resize(silenceSamples, 0);
            }

            // Insert silence at the beginning
            audioChunks.insert(audioChunks.begin(), std::move(silenceChunk));

            // Update timestamps of existing chunks
            for(auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
                it->timestamp += silenceDuration;
            }
        }
    }

    std::string getBaselineCachePath() {
#ifdef _WIN32
        const char* appData = std::getenv("APPDATA");
        if (appData && *appData) {
            boost::filesystem::path dir = boost::filesystem::path(appData) / "MotionCam Tools" / "Fuse";
            boost::filesystem::create_directories(dir);
            return (dir / "baseline_cache.json").string();
        }
#endif
        boost::filesystem::path dir = boost::filesystem::temp_directory_path() / "MotionCamFuse";
        boost::filesystem::create_directories(dir);
        return (dir / "baseline_cache.json").string();
    }

    bool getFileSignature(const std::string& path, uint64_t& sizeOut, std::time_t& mtimeOut) {
        try {
            boost::filesystem::path p(path);
            if (!boost::filesystem::exists(p)) {
                return false;
            }
            sizeOut = static_cast<uint64_t>(boost::filesystem::file_size(p));
            mtimeOut = boost::filesystem::last_write_time(p);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    bool loadBaselineCache(nlohmann::json& out) {
        const std::string cachePath = getBaselineCachePath();
        std::ifstream in(cachePath);
        if (!in.is_open()) {
            return false;
        }
        try {
            in >> out;
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    void saveBaselineCache(const nlohmann::json& data) {
        const std::string cachePath = getBaselineCachePath();
        std::ofstream out(cachePath, std::ios::trunc);
        if (!out.is_open()) {
            return;
        }
        out << data.dump(2);
    }

    bool getCachedBaselineExposure(
        const std::string& path,
        uint64_t fileSize,
        std::time_t mtime,
        double& baselineOut) {
        nlohmann::json cache;
        if (!loadBaselineCache(cache)) {
            return false;
        }

        if (!cache.contains("entries") || !cache["entries"].is_object()) {
            return false;
        }

        auto& entries = cache["entries"];
        if (!entries.contains(path)) {
            return false;
        }

        auto entry = entries[path];
        if (!entry.contains("size") || !entry.contains("mtime") || !entry.contains("baseline")) {
            return false;
        }

        const uint64_t cachedSize = entry["size"].get<uint64_t>();
        const std::time_t cachedMtime = entry["mtime"].get<std::time_t>();
        if (cachedSize != fileSize || cachedMtime != mtime) {
            return false;
        }

        baselineOut = entry["baseline"].get<double>();
        return std::isfinite(baselineOut) && baselineOut > 0.0;
    }

    void storeCachedBaselineExposure(
        const std::string& path,
        uint64_t fileSize,
        std::time_t mtime,
        double baseline) {
        nlohmann::json cache;
        loadBaselineCache(cache);

        cache["version"] = 1;
        if (!cache.contains("entries") || !cache["entries"].is_object()) {
            cache["entries"] = nlohmann::json::object();
        }

        cache["entries"][path] = {
            {"size", fileSize},
            {"mtime", mtime},
            {"baseline", baseline}
        };

        saveBaselineCache(cache);
    }

    int getScaleFromOptions(FileRenderOptions options, int draftScale) {
        if(options & RENDER_OPT_DRAFT)
            return draftScale;

        return 1;
    }
}

void VirtualFileSystemImpl_MCRAW::applyMatrixOverride(CameraConfiguration& cameraConfig) const {
    (void)cameraConfig;
}

VirtualFileSystemImpl_MCRAW::VirtualFileSystemImpl_MCRAW(
        BS::thread_pool& ioThreadPool,
        BS::thread_pool& processingThreadPool,
        LRUCache& lruCache,
        FileRenderOptions options,
        int draftScale,
        const std::string& cfrTarget,
        const std::string& cropTarget,
        const std::string& file,
        const std::string& baseName,
        const std::string& cameraModel,
        const std::string& levels,
        const std::string& logTransform,
        const std::string& exposureCompensation,
        const std::string& quadBayerOption,
        bool matrixOverrideEnabled,
        const std::string& matrixProfile,
        const std::string& matrixFilePath) :
        mCache(lruCache),
        mIoThreadPool(ioThreadPool),
        mProcessingThreadPool(processingThreadPool),
        mSrcPath(file),
        mBaseName(baseName),
        mTypicalDngSize(0),
        mFps(0),
        mMedFps(0),
        mAvgFps(0),
        mTotalFrames(0),
        mDroppedFrames(0),
        mDuplicatedFrames(0),
        mWidth(0),
        mHeight(0),
        mDraftScale(draftScale),
        mCFRTarget(cfrTarget),
        mCropTarget(cropTarget),
        mCameraModel(cameraModel),
        mLevels(levels),
        mLogTransform(logTransform),
        mExposureCompensation(exposureCompensation),
        mQuadBayerOption(quadBayerOption),
        mOptions(options),
        mUseMatrixOverride(false),
        mMatrixProfile(""),
        mMatrixFilePath(""),
        mPrefetchWindow(6) {

#if defined(__APPLE__) || defined(__linux__)
    // FUSE-backed platforms use the legacy behavior: always cache generated
    // frames and avoid speculative read-ahead from concurrent kernel reads.
    mCache.setStreamingBypass(false);
    mPrefetchWindow = 0;
#else
    mCache.setStreamingBypass(true);
#endif

    const auto ctorStart = std::chrono::steady_clock::now();
    auto msSince = [](const std::chrono::steady_clock::time_point& start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    };

    Decoder decoder(mSrcPath);
    spdlog::info("Mount timing: Decoder init {} ms ({})", msSince(ctorStart), mSrcPath);
    auto frames = decoder.getFrames();
    spdlog::info("Mount timing: Frame list {} ms (count={})", msSince(ctorStart), frames.size());
    std::sort(frames.begin(), frames.end());
    if(frames.empty())
        return;
    uint64_t fileSize = 0;
    std::time_t mtime = 0;
    bool haveSignature = getFileSignature(mSrcPath, fileSize, mtime);

    const bool fastMount = (options & RENDER_OPT_FAST_MOUNT);
    double cachedBaseline = 0.0;
    bool cacheHit = haveSignature && getCachedBaselineExposure(mSrcPath, fileSize, mtime, cachedBaseline);
    if (cacheHit) {
        mBaselineExpValue = cachedBaseline;
        spdlog::info("Mount timing: Baseline exposure cache hit");
    } else if (fastMount) {
        nlohmann::json metadata;
        decoder.loadFrameMetadata(frames.front(), metadata);
        const auto& cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
        mBaselineExpValue = cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime;
        spdlog::info("Mount timing: Fast mount baseline from first frame");
    } else {
        mBaselineExpValue = std::numeric_limits<double>::max();
        for(const auto& frame : frames) {
            nlohmann::json metadata;
            decoder.loadFrameMetadata(frame, metadata);
            const auto& cameraFrameMetadata = CameraFrameMetadata::limitedParse(metadata);
            mBaselineExpValue = std::min(mBaselineExpValue, cameraFrameMetadata.iso * cameraFrameMetadata.exposureTime);
        }
        spdlog::info("Mount timing: Baseline exposure scan {} ms", msSince(ctorStart));
        if (haveSignature && std::isfinite(mBaselineExpValue) && mBaselineExpValue > 0.0) {
            storeCachedBaselineExposure(mSrcPath, fileSize, mtime, mBaselineExpValue);
            spdlog::info("Mount timing: Baseline exposure cache stored");
        }
    }
    this->init(options);
    spdlog::info("Mount timing: init {} ms (total {})", msSince(ctorStart), msSince(ctorStart));
}

VirtualFileSystemImpl_MCRAW::~VirtualFileSystemImpl_MCRAW() {
    spdlog::info("Destroying VirtualFileSystemImpl_MCRAW({})", mSrcPath);
}

void VirtualFileSystemImpl_MCRAW::init(FileRenderOptions options) {
    const auto initStart = std::chrono::steady_clock::now();
    auto msSince = [](const std::chrono::steady_clock::time_point& start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    };
    Decoder decoder(mSrcPath);
    spdlog::info("Init timing: Decoder init {} ms", msSince(initStart));
    auto frames = decoder.getFrames();
    spdlog::info("Init timing: Frame list {} ms (count={})", msSince(initStart), frames.size());
    std::sort(frames.begin(), frames.end());

    if(frames.empty())
        return;

    spdlog::debug("VirtualFileSystemImpl_MCRAW::init(options={})", optionsToString(options));

    mMatrixOverrideProfile.reset();

    // Clear everything
    mFiles.clear();
    mFileIndex.clear();
    mPathIndex.clear();
    mTimestampIndex.clear();
    {
        std::lock_guard<std::mutex> lock(mPrefetchMutex);
        mPrefetchFutures.clear();
        mPrefetchOrder.clear();
#if defined(__APPLE__) || defined(__linux__)
        mPrefetchOrderIndex.clear();
#endif
    }

    auto frameRateInfo = calculateFrameRate(frames);
    mMedFps = frameRateInfo.medianFrameRate;
    mAvgFps = frameRateInfo.averageFrameRate;
    spdlog::info("Init timing: FPS stats {} ms", msSince(initStart));

    bool applyCFRConversion = options & RENDER_OPT_FRAMERATE_CONVERSION;
    
    if (applyCFRConversion && !mCFRTarget.empty()) {
        if (mCFRTarget == "Prefer Integer") {
            if (mMedFps <=  23.0 || mMedFps >= 1000.0) 
                mFps = mMedFps;
            else if (mMedFps < 24.5)
                mFps = 24.0f;
            else if (mMedFps < 26.0)
                mFps = 25.0f;
            else if (mMedFps < 33.0)
                mFps = 30.0f;
            else if (mMedFps < 49.0)
                mFps = 48.0f;
            else if (mMedFps < 52.0)
                mFps = 50.0f;
            else if (mMedFps > 56.0  && mMedFps < 63.0)
                mFps = 60.0f;
            else if (mMedFps > 112.0 && mMedFps < 125.0)
                mFps = 120.0f;
            else if (mMedFps > 224.0 && mMedFps < 250.0)
                mFps = 240.0f;
            else if (mMedFps > 448.0 && mMedFps < 500.0)
                mFps = 480.0f;
            else if (mMedFps > 896.0 && mMedFps < 1000.0)
                mFps = 960.0f;
            else if (mMedFps >= 63.0)
                mFps = 120.0f;
            else   
                mFps = 60.0f;
        }
        else if (mCFRTarget == "Prefer Drop Frame") {
            if (mMedFps <=  23.0 || mMedFps >= 1000.0) 
                mFps = mMedFps;
            else if (mMedFps < 24.5)
                mFps = 23.976f;
            else if (mMedFps < 26.0)
                mFps = 25.0f;
            else if (mMedFps < 33.0)
                mFps = 29.97f;
            else if (mMedFps < 49.0)
                mFps = 47.952f;
            else if (mMedFps < 52.0)
                mFps = 50.0f;
            else if (mMedFps > 56.0  && mMedFps < 63.0)
                mFps = 59.94f;
            else if (mMedFps > 112.0 && mMedFps < 125.0)
                mFps = 119.88f;
            else if (mMedFps > 224.0 && mMedFps < 250.0)
                mFps = 240.0f;
            else if (mMedFps > 448.0 && mMedFps < 500.0)
                mFps = 480.0f;
            else if (mMedFps > 896.0 && mMedFps < 1000.0)
                mFps = 960.0f;
            else if (mMedFps >= 63.0)
                mFps = 119.88f;
            else    
                mFps = 59.94f;
        }
        else if (mCFRTarget == "Median (Slowmotion)") {
            // Use median frame rate for non real time playback
            mFps = mMedFps;
        }
        else if (mCFRTarget == "Average (Testing)") {
            // legacy framerate target determination
            mFps = mAvgFps;
        }
        else {
            // Custom framerate - try to parse as float
            try {
                mFps = std::stof(mCFRTarget);
            } catch (const std::exception& e) {
                spdlog::warn("Invalid CFR target '{}', using median frame rate", mCFRTarget);
                mFps = mMedFps;
            }
        }
    } else {
        // CFR disabled: just use the measured average rate
        mFps = mAvgFps;
    }

    // Calculate typical DNG size that we can use for all files
    std::vector<uint8_t> data;
    nlohmann::json metadata;

    decoder.loadFrame(frames[0], data, metadata);

    auto cameraConfig = CameraConfiguration::parse(decoder.getContainerMetadata());
    auto cameraFrameMetadata = CameraFrameMetadata::parse(metadata);
    applyMatrixOverride(cameraConfig);

    // Store frame information
    mWidth = cameraFrameMetadata.width;
    mHeight = cameraFrameMetadata.height;
    mTotalFrames = static_cast<int>(frames.size());
    mDroppedFrames = 0; // Will be calculated during frame processing
    mDuplicatedFrames = 0;	

    auto dngData = utils::generateDng(
        data,
        cameraFrameMetadata,
        cameraConfig,
        mFps,
        0,
        options,
        getScaleFromOptions(options, mDraftScale),
        mBaselineExpValue,
        mCropTarget,
        mCameraModel,
        mLevels,
        mLogTransform,
        mExposureCompensation,
        mQuadBayerOption
    );

    mTypicalDngSize = dngData->size();

    // Calculate first frame size at full quality if high quality first frame is enabled
    if ((options & RENDER_OPT_HIGH_QUALITY_FIRST_FRAME) && (options & RENDER_OPT_DRAFT)) {
        auto firstFrameDngData = utils::generateDng(
            data,
            cameraFrameMetadata,
            cameraConfig,
            mFps,
            0,
            options,
            1,  // Force scale=1 for full quality
            mBaselineExpValue,
            mCropTarget,
            mCameraModel,
            mLevels,
            mLogTransform,
            mExposureCompensation,
            mQuadBayerOption
        );
        mFirstFrameDngSize = firstFrameDngData->size();
    } else {
        mFirstFrameDngSize = mTypicalDngSize;
    }

    // Generate file entries
    int lastPts = 0;

    mFiles.reserve(frames.size()*2);

// Disable icon previews in Windows/MacOS
#ifdef _WIN32
    Entry desktopIni;

    desktopIni.type = FILE_ENTRY;
    desktopIni.size = DESKTOP_INI.size();
    desktopIni.name = "desktop.ini";

    mFiles.emplace_back(desktopIni);
#endif

    // Generate and add audio (TODO: We're loading all the audio into memory)
    Entry audioEntry;

    std::vector<AudioChunk> audioChunks;
    decoder.loadAudio(audioChunks);

    if(!audioChunks.empty()) {
        auto fpsFraction = utils::toFraction(mFps);
        AudioSampleFormat audioFormat = audioChunks[0].format;
        int bitDepth = (audioFormat == AudioSampleFormat::Float32) ? 32 : 16;
        AudioWriter audioWriter(mAudioFile, decoder.numAudioChannels(), decoder.audioSampleRateHz(), fpsFraction.first, fpsFraction.second, bitDepth);

        // Sync the audio to the video
        syncAudio(
            frames[0],
            audioChunks,
            decoder.audioSampleRateHz(),
            decoder.numAudioChannels());

        for(auto& x : audioChunks) {
            int numFrames = x.sampleCount() / decoder.numAudioChannels();
            if(audioFormat == AudioSampleFormat::Float32) {
                audioWriter.write(x.float32Data, numFrames);
            } else {
                audioWriter.write(x.int16Data, numFrames);
            }
        }
    }

    if(!mAudioFile.empty()) {
        audioEntry.type = EntryType::FILE_ENTRY;
        audioEntry.size = mAudioFile.size();
        audioEntry.name = "audio.wav";

        mFiles.emplace_back(audioEntry);
        const auto audioPath = audioEntry.getFullPath().string();
        mFileIndex[audioPath] = audioEntry;
        mPathIndex[audioPath] = mFiles.size() - 1;
    }

    // Add video frames
    for(auto& x : frames) {
        if(applyCFRConversion) {
            int pts = getFrameNumberFromTimestamp(x, frames[0], mFps);

            // Count dropped frames before this frame
            mDuplicatedFrames += (std::max)(0, pts - lastPts - 1);

            if (lastPts > 0 && lastPts == pts)
                mDroppedFrames += 1;

            // Duplicate frames to account for dropped frames
            while(lastPts < pts) {
                Entry entry;

                // Add main entry
                entry.type = EntryType::FILE_ENTRY;
                // Use larger size for first frame if high quality first frame is enabled
                entry.size = (lastPts == 0) ? mFirstFrameDngSize : mTypicalDngSize;
                entry.name = constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");
                entry.userData = x;

                const auto fullPath = entry.getFullPath().string();
                mFileIndex[fullPath] = entry;
                mPathIndex[fullPath] = mFiles.size();
                mTimestampIndex[x] = mFiles.size();
                mFiles.emplace_back(entry);
                ++lastPts;
            }
        } else {
            Entry entry;

            // Add main entry
            entry.type = EntryType::FILE_ENTRY;
            // Use larger size for first frame if high quality first frame is enabled
            entry.size = (lastPts == 0) ? mFirstFrameDngSize : mTypicalDngSize;
            entry.name = constructFrameFilename(mBaseName + std::string("-"), lastPts, 6, "dng");
            entry.userData = x;

            const auto fullPath = entry.getFullPath().string();
            mFiles.emplace_back(entry);
            mFileIndex[fullPath] = entry;
            mPathIndex[fullPath] = mFiles.size() - 1;
            mTimestampIndex[x] = mFiles.size() - 1;
            ++lastPts;
        }
    }
}

std::vector<Entry> VirtualFileSystemImpl_MCRAW::listFiles(const std::string& filter) const {
    // TODO: Use filter
    return mFiles;
}

std::optional<Entry> VirtualFileSystemImpl_MCRAW::findEntry(const std::string& fullPath) const {
    auto it = mFileIndex.find(boost::filesystem::path(fullPath).relative_path().string());
    if (it != mFileIndex.end())
        return it->second;

    return {};
}

size_t VirtualFileSystemImpl_MCRAW::getEntryIndex(const Entry& entry) const {
    auto it = mPathIndex.find(entry.getFullPath().string());
    if (it != mPathIndex.end()) {
        return it->second;
    }
    return std::numeric_limits<size_t>::max();
}

std::shared_future<VirtualFileSystemImpl_MCRAW::FrameData> VirtualFileSystemImpl_MCRAW::scheduleDecode(
    const Entry& entry,
    std::chrono::high_resolution_clock::time_point requestStartTime) {

    auto frameDataFuture = mIoThreadPool.submit_task([this, entry, srcPath = mSrcPath, options = mOptions, requestStartTime]() -> FrameData {
        auto ioTaskStart = std::chrono::high_resolution_clock::now();
        auto queueDelayMs = std::chrono::duration_cast<std::chrono::milliseconds>(ioTaskStart - requestStartTime).count();
        spdlog::warn("[QUEUE] IO task start: file={} queueDelay={}ms", entry.name, queueDelayMs);

        thread_local std::map<std::string, std::unique_ptr<Decoder>> decoders;

        auto timestamp = std::get<Timestamp>(entry.userData);

        spdlog::debug("Reading frame {} with options {}", timestamp, optionsToString(options));

        if(decoders.find(srcPath) == decoders.end()) {
            decoders[srcPath] = std::make_unique<Decoder>(srcPath);
        }

        auto& decoder = decoders[srcPath];
        auto data = std::make_shared<std::vector<uint8_t>>();

        nlohmann::json metadata;
        auto allFrames = decoder->getFrames();

        auto it = std::find(allFrames.begin(), allFrames.end(), timestamp);
        if(it == allFrames.end()) {
            spdlog::error("Frame {} not found", timestamp);
            throw std::runtime_error("Failed to find frame");
        }

        decoder->loadFrame(timestamp, *data, metadata);

        size_t frameIndex = std::distance(allFrames.begin(), it);

        auto cameraConfig = CameraConfiguration::parse(decoder->getContainerMetadata());
        applyMatrixOverride(cameraConfig);
        auto frameMeta = CameraFrameMetadata::parse(metadata);

        return std::make_tuple(
            frameIndex, std::move(cameraConfig), std::move(frameMeta), std::move(data));
    });

    return frameDataFuture.share();
}

std::shared_future<VirtualFileSystemImpl_MCRAW::FrameData> VirtualFileSystemImpl_MCRAW::getOrSchedulePrefetch(
    const Entry& entry,
    std::chrono::high_resolution_clock::time_point requestStartTime) {
    auto timestamp = std::get<Timestamp>(entry.userData);
    {
        std::lock_guard<std::mutex> lock(mPrefetchMutex);
        auto it = mPrefetchFutures.find(timestamp);
        if (it != mPrefetchFutures.end()) {
            return it->second;
        }
    }

    auto sharedFuture = scheduleDecode(entry, requestStartTime);
    {
        std::lock_guard<std::mutex> lock(mPrefetchMutex);
        auto [it, inserted] = mPrefetchFutures.emplace(timestamp, sharedFuture);
        if (inserted) {
            mPrefetchOrder.push_back(timestamp);
#if !defined(__APPLE__) && !defined(__linux__)
            mPrefetchOrderIndex[timestamp] = std::prev(mPrefetchOrder.end());
#endif
            trimPrefetchLocked();
        } else {
            sharedFuture = it->second;
        }
    }
    return sharedFuture;
}

void VirtualFileSystemImpl_MCRAW::trimPrefetchLocked() {
    while (mPrefetchOrder.size() > mPrefetchWindow + 2) {
        auto oldest = mPrefetchOrder.front();
        mPrefetchOrder.pop_front();
#if !defined(__APPLE__) && !defined(__linux__)
        mPrefetchOrderIndex.erase(oldest);
#endif
        mPrefetchFutures.erase(oldest);
    }
}

void VirtualFileSystemImpl_MCRAW::scheduleReadahead(size_t currentIndex) {
    if (currentIndex == std::numeric_limits<size_t>::max()) {
        return;
    }

    const auto total = mFiles.size();
    const auto now = std::chrono::high_resolution_clock::now();
    const auto window = mPrefetchWindow;

    for (size_t offset = 1; offset <= window; ++offset) {
        auto nextIndex = currentIndex + offset;
        if (nextIndex >= total) {
            break;
        }

        const auto& nextEntry = mFiles[nextIndex];
        const auto timestamp = std::get<Timestamp>(nextEntry.userData);
        {
            std::lock_guard<std::mutex> lock(mPrefetchMutex);
            if (mPrefetchFutures.find(timestamp) != mPrefetchFutures.end()) {
                continue;
            }
        }

        auto fut = scheduleDecode(nextEntry, now);
        {
            std::lock_guard<std::mutex> lock(mPrefetchMutex);
            if (mPrefetchFutures.emplace(timestamp, fut).second) {
                mPrefetchOrder.push_back(timestamp);
#if !defined(__APPLE__) && !defined(__linux__)
                mPrefetchOrderIndex[timestamp] = std::prev(mPrefetchOrder.end());
#endif
                trimPrefetchLocked();
            }
        }
    }
}

void VirtualFileSystemImpl_MCRAW::consumePrefetch(Timestamp timestamp) {
    std::lock_guard<std::mutex> lock(mPrefetchMutex);
    mPrefetchFutures.erase(timestamp);
#if defined(__APPLE__) || defined(__linux__)
    auto it = std::find(mPrefetchOrder.begin(), mPrefetchOrder.end(), timestamp);
    if (it != mPrefetchOrder.end()) {
        mPrefetchOrder.erase(it);
    }
#else
    auto it = mPrefetchOrderIndex.find(timestamp);
    if (it != mPrefetchOrderIndex.end()) {
        mPrefetchOrder.erase(it->second);
        mPrefetchOrderIndex.erase(it);
    }
#endif
}

size_t VirtualFileSystemImpl_MCRAW::generateFrame(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async)
{
    const auto timestamp = std::get<Timestamp>(entry.userData);

    // Try to get from cache first
    auto cacheEntry = mCache.get(entry);
    if(cacheEntry && pos < cacheEntry->size()) {
        spdlog::debug("[CACHE] HIT: {} (size={} bytes)", entry.name, cacheEntry->size());

        // Calculate length to copy
        const size_t actualLen = (std::min)(len, cacheEntry->size() - pos);

        // Copy the data from cache
        std::memcpy(dst, cacheEntry->data() + pos, actualLen);

        // Push entry to front
        mCache.put(entry, cacheEntry);

        return actualLen;
    }

    spdlog::debug("[CACHE] MISS: {} - will generate", entry.name);

    spdlog::warn(
        "[THREADPOOL] IO running={} queued={} | Processing running={} queued={}",
        mIoThreadPool.get_tasks_running(),
        mIoThreadPool.get_tasks_queued(),
        mProcessingThreadPool.get_tasks_running(),
        mProcessingThreadPool.get_tasks_queued());

    auto requestStartTime = std::chrono::high_resolution_clock::now();
    auto sharableFuture = getOrSchedulePrefetch(entry, requestStartTime);
    scheduleReadahead(getEntryIndex(entry));

    const auto fps = mFps;
    const auto draftScale = mDraftScale;
    const auto baselineExpValue = mBaselineExpValue;
    const auto options = mOptions;

    auto generateTask = [this, &cache = mCache, entry, sharableFuture, fps, draftScale, baselineExpValue, options, pos, len, dst, result, requestStartTime, timestamp]() {
        auto taskStart = std::chrono::high_resolution_clock::now();
        long long decodeWaitMs = 0;
        long long dngMs = 0;
        long long copyMs = 0;

        auto queueDelayMs = std::chrono::duration_cast<std::chrono::milliseconds>(taskStart - requestStartTime).count();
        spdlog::warn("[QUEUE] Processing task start: file={} queueDelay={}ms", entry.name, queueDelayMs);

        size_t readBytes = 0;
        int errorCode = -1;

        try {
            auto decodeWaitStart = std::chrono::high_resolution_clock::now();
            auto decodedFrame = sharableFuture.get();
            decodeWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - decodeWaitStart).count();
            consumePrefetch(timestamp);

            auto [frameIndex, containerMetadata, frameMetadata, frameData] = std::move(decodedFrame);

            spdlog::debug("Generating {}", entry.name);

            // Force high quality for first frame if option is enabled AND draft mode is on
            int effectiveScale = getScaleFromOptions(options, draftScale);

            // Check if this is the first output frame (frame 000000) by checking the filename
            bool isFirstOutputFrame = entry.name.find("000000.dng") != std::string::npos;

            if ((options & RENDER_OPT_HIGH_QUALITY_FIRST_FRAME) &&
                (options & RENDER_OPT_DRAFT) &&
                isFirstOutputFrame) {
                // Override scale to 1 for full quality on first frame
                effectiveScale = 1;
            }

            auto dngStart = std::chrono::high_resolution_clock::now();
            auto dngData = utils::generateDng(
                *frameData,
                frameMetadata,
                containerMetadata,
                fps,
                frameIndex,
                options,
                effectiveScale,
                baselineExpValue,
                mCropTarget,
                mCameraModel,
                mLevels,
                mLogTransform,
                mExposureCompensation,
                mQuadBayerOption);
            dngMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - dngStart).count();

            if(dngData && pos < dngData->size()) {
                // Calculate length to copy
                auto copyStart = std::chrono::high_resolution_clock::now();
                const size_t actualLen = std::min(len, dngData->size() - pos);

                std::memcpy(dst, dngData->data() + pos, actualLen);

                readBytes = actualLen;
                errorCode = 0;
                copyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now() - copyStart).count();
            }

            // Add to cache
            if (cache.isStreamingBypassActive()) {
                spdlog::debug("[CACHE] SKIP STORE (streaming): {}", entry.name);
#if defined(__APPLE__) || defined(__linux__)
                // Release any in-progress marker to avoid repeated timeouts on
                // FUSE-backed platforms.
                cache.markLoadFailed(entry);
#endif
            } else {
                cache.put(entry, dngData);
                spdlog::debug("[CACHE] STORED: {} (size={} bytes)", entry.name, dngData->size());
            }
        }
        catch(std::runtime_error& e) {
            spdlog::error("Failed to generate DNG (error: {})", e.what());
            cache.markLoadFailed(entry);
        }

        auto taskMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - taskStart).count();

        spdlog::warn(
            "[PIPELINE] Frame {}: decodeWait={}ms dngGen={}ms copy={}ms taskTotal={}ms",
            entry.name,
            decodeWaitMs,
            dngMs,
            copyMs,
            taskMs);

        result(readBytes, errorCode);

        return readBytes;
    };


    auto processFuture = mProcessingThreadPool.submit_task(generateTask);
    if(!async)
        return processFuture.get();

    return 0;
}

size_t VirtualFileSystemImpl_MCRAW::generateAudio(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async)
{
    size_t readBytes = 0;

    if(pos < mAudioFile.size()) {
        // Calculate length to copy
        const size_t actualLen = (std::min)(len, mAudioFile.size() - pos);

        std::memcpy(dst, mAudioFile.data() + pos, actualLen);

        readBytes = actualLen;
    }

    // Always read synchronously for now
    return readBytes;
}

void VirtualFileSystemImpl_MCRAW::recordDngAccess(const Entry& entry) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mAccessMutex);
    mLastAccessTimes[entry] = now;
}

int VirtualFileSystemImpl_MCRAW::readFile(
    const Entry& entry,
    const size_t pos,
    const size_t len,
    void* dst,
    std::function<void(size_t, int)> result,
    bool async) {

    #ifdef _WIN32
        if(entry.name == "desktop.ini") {
            const size_t actualLen = (std::min)(len, DESKTOP_INI.size() - pos);
            std::memcpy(dst, DESKTOP_INI.data() + pos, actualLen);

            return actualLen;
        }
    #endif

    // Requestion audio?
    if(boost::ends_with(entry.name, "wav")) {
        return generateAudio(entry, pos, len, dst, result, async);
    }
    else if(boost::ends_with(entry.name, "dng")) {
        recordDngAccess(entry);
        return generateFrame(entry, pos, len, dst, result, async);
    }

    return -1;
}

std::vector<Entry> VirtualFileSystemImpl_MCRAW::getExpiredDngEntries(std::chrono::seconds ttl) {
    if (ttl.count() <= 0) {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<Entry> expired;

    std::lock_guard<std::mutex> lock(mAccessMutex);
    for (auto it = mLastAccessTimes.begin(); it != mLastAccessTimes.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second);
        if (age > ttl) {
            expired.push_back(it->first);
            it = mLastAccessTimes.erase(it);
        } else {
            ++it;
        }
    }

    return expired;
}

std::vector<std::pair<Entry, std::chrono::steady_clock::time_point>> VirtualFileSystemImpl_MCRAW::getDngAccessEntries() const {
    std::vector<std::pair<Entry, std::chrono::steady_clock::time_point>> entries;
    std::lock_guard<std::mutex> lock(mAccessMutex);
    entries.reserve(mLastAccessTimes.size());
    for (const auto& entry : mLastAccessTimes) {
        entries.emplace_back(entry.first, entry.second);
    }
    return entries;
}

void VirtualFileSystemImpl_MCRAW::forgetDngAccess(const Entry& entry) {
    std::lock_guard<std::mutex> lock(mAccessMutex);
    mLastAccessTimes.erase(entry);
}

void VirtualFileSystemImpl_MCRAW::updateOptions(const RenderSettings& settings) {
    FileRenderOptions options = settings.renderOptions;
    const int draftScale = settings.draftQuality;
    const auto& cfrTarget = settings.cfrTarget;
    const auto& cropTarget = settings.cropTarget;
    const auto& cameraModel = settings.cameraModel;
    const auto& levels = settings.levels;
    const std::string logTransform = "";
    const auto& exposureCompensation = settings.exposureCompensation;
    const std::string quadBayerOption = "";
    const bool matrixOverrideEnabled = settings.matrixOverrideEnabled;
    const auto& matrixProfile = settings.matrixProfile;
    const auto& matrixFilePath = settings.matrixFilePath;
    mDraftScale = draftScale;
    mOptions = options;
    mCFRTarget = cfrTarget;
    mCropTarget = cropTarget;
    mCameraModel = cameraModel;
    mLevels = levels;
    mLogTransform = logTransform;
    mExposureCompensation = exposureCompensation;
    mQuadBayerOption = quadBayerOption;
    mUseMatrixOverride = matrixOverrideEnabled;
    mMatrixProfile = matrixProfile;
    mMatrixFilePath = matrixFilePath;

    mCache.clear();
    {
        std::lock_guard<std::mutex> lock(mAccessMutex);
        mLastAccessTimes.clear();
    }
    init(options);
}

FileInfo VirtualFileSystemImpl_MCRAW::getFileInfo() const {
    return FileInfo{
        mMedFps,
        mAvgFps,
        mFps,
        mTotalFrames,
        mDroppedFrames,
        mDuplicatedFrames,
        mWidth,
        mHeight
    };
}

} // namespace motioncam
