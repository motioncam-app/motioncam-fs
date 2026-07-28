#pragma once

#include <vector>
#include <memory>
#include <bw64/bw64.hpp>

namespace bw64 {
    class Bw64Writer;
}

namespace motioncam {
    class AudioWriter {
    public:
        AudioWriter(std::vector<uint8_t>& output, int numChannels, int sampleRate, int fpsNum, int fpsDen, int bitDepth);

        void write(const std::vector<int16_t>& data, int numFrames);
        void write(const std::vector<float>& data, int numFrames);
        
    private:
        const int mFd;
        std::unique_ptr<bw64::Bw64Writer> mWriter;
    };
}
