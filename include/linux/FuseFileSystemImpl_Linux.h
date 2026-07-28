#pragma once

#include <map>
#include <memory>

#include "IFuseFileSystem.h"

namespace BS { class thread_pool; }

namespace motioncam {

struct LinuxFuseSession;
class LRUCache;

// Linux counterpart to the Windows projected-filesystem provider.  FUSE3
// exposes generated frames as a read-only, kernel-cached filesystem.
class FuseFileSystemImpl_Linux : public IFuseFileSystem {
public:
    FuseFileSystemImpl_Linux();
    ~FuseFileSystemImpl_Linux();

    MountId mount(const RenderSettings& settings, const std::string& srcFile,
                  const std::string& dstPath) override;
    void unmount(MountId mountId) override;
    void updateOptions(MountId mountId, const RenderSettings& settings) override;
    std::optional<FileInfo> getFileInfo(MountId mountId) override;
    bool generateThumbnail(MountId mountId, const std::string& outputPath,
                           int width = 320, int height = 240) override;
    void setCachePolicy(CachePolicy policy) override;
    void setCacheQuotaBytes(std::uint64_t bytes) override;
    void cleanupCacheExpired() override;

private:
    MountId mNextMountId;
    std::map<MountId, std::unique_ptr<LinuxFuseSession>> mMountedFiles;
    std::unique_ptr<BS::thread_pool> mIoThreadPool;
    std::unique_ptr<BS::thread_pool> mProcessingThreadPool;
    std::unique_ptr<LRUCache> mCache;
    CachePolicy mCachePolicy;
    std::uint64_t mCacheQuotaBytes;
};

} // namespace motioncam
