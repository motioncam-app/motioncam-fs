#ifdef __APPLE__
#include "CrashDebug.h"

#include <atomic>
#include <cstring>
#include <cstdio>
#include <unistd.h>

namespace motioncam {
namespace debug {
namespace {
std::atomic<int> gFrameNumber{-1};
std::atomic<unsigned int> gWidth{0};
std::atomic<unsigned int> gHeight{0};
std::atomic<std::size_t> gInputBytes{0};
std::atomic<std::size_t> gProcessedBytes{0};
char gStage[128] = "init";
} // namespace

void setStage(const char* stage) {
    if (!stage) {
        return;
    }
    std::strncpy(gStage, stage, sizeof(gStage) - 1);
    gStage[sizeof(gStage) - 1] = '\0';
}

void setDngContext(int frameNumber,
                   unsigned int width,
                   unsigned int height,
                   std::size_t inputBytes,
                   std::size_t processedBytes) {
    gFrameNumber.store(frameNumber, std::memory_order_relaxed);
    gWidth.store(width, std::memory_order_relaxed);
    gHeight.store(height, std::memory_order_relaxed);
    gInputBytes.store(inputBytes, std::memory_order_relaxed);
    gProcessedBytes.store(processedBytes, std::memory_order_relaxed);
}

void dumpCrashContext(int fd) {
    char buf[256];
    const int len = std::snprintf(
        buf,
        sizeof(buf),
        "Last stage: %s\nFrame: %d\nSize: %ux%u\nInput bytes: %zu\nProcessed bytes: %zu\n",
        gStage,
        gFrameNumber.load(std::memory_order_relaxed),
        gWidth.load(std::memory_order_relaxed),
        gHeight.load(std::memory_order_relaxed),
        gInputBytes.load(std::memory_order_relaxed),
        gProcessedBytes.load(std::memory_order_relaxed));
    if (len > 0 && fd >= 0) {
        ::write(fd, buf, static_cast<size_t>(len));
    }
}
} // namespace debug
} // namespace motioncam
#endif
