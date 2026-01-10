#pragma once

#ifdef __APPLE__
#include <cstddef>

namespace motioncam {
namespace debug {
void setStage(const char* stage);
void setDngContext(int frameNumber,
                   unsigned int width,
                   unsigned int height,
                   std::size_t inputBytes,
                   std::size_t processedBytes);
void dumpCrashContext(int fd);
} // namespace debug
} // namespace motioncam
#endif
