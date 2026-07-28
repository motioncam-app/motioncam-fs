#include "AudioWriter.h"

namespace motioncam {
    namespace {
        constexpr auto PROJECT = "RAW Video";
        constexpr auto NOTES = "-";
    
        constexpr auto METADATA =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<BWFXML>"
            "<IXML_VERSION>1.5</IXML_VERSION>"
            "<PROJECT>%s</PROJECT>"
            "<NOTE>%s</NOTE>"
            "<CIRCLED>FALSE</CIRCLED>"
            "<BLACKMAGIC-KEYWORDS>%s</BLACKMAGIC-KEYWORDS>"
            "<TAPE>%d</TAPE>"
            "<SCENE>%d</SCENE>"
            "<BLACKMAGIC-SHOT>%d</BLACKMAGIC-SHOT>"
            "<TAKE>%d</TAKE>"
            "<BLACKMAGIC-ANGLE>ms</BLACKMAGIC-ANGLE>"
            "<SPEED>"
            "<MASTER_SPEED>%d/%d</MASTER_SPEED>"
            "<CURRENT_SPEED>%d/%d</CURRENT_SPEED>"
            "<TIMECODE_RATE>%d/%d</TIMECODE_RATE>"
            "<TIMECODE_FLAG>NDF</TIMECODE_FLAG>"
            "</SPEED>"
            "</BWFXML>";
    }

    static std::string FormatMetadata(const int fpsNum,
                                      const int fpsDen,
                                      const std::string& project,
                                      const std::string& notes,
                                      const std::string& keywords,
                                      const int& tape,
                                      const int& scene,
                                      const int& shot,
                                      const int& take) {
    
        char result[1024];
        
        snprintf(&result[0],
                 1024,
                 METADATA,
                 project.c_str(),
                 notes.c_str(),
                 keywords.c_str(),
                 tape,
                 scene,
                 shot,
                 take,
                 fpsNum,
                 fpsDen,
                 fpsNum,
                 fpsDen,
                 fpsNum,
                 fpsDen);
        
        return std::string(result);
    }

    static std::shared_ptr<bw64::Chunk> CreateMetadata(int fpsNum, int fpsDen) {
        const int tape = 1;
        const int scene = 1;
        const int shot = 1;
        const int take = 1;
        
        auto metadata = FormatMetadata(fpsNum,
                                       fpsDen,
                                       PROJECT,
                                       NOTES,
                                       "",
                                       tape,
                                       scene,
                                       shot,
                                       take);
        
        return std::make_shared<bw64::iXmlChunk>(metadata);
    }

    AudioWriter::AudioWriter(std::vector<uint8_t>& output, int numChannels, int sampleRate, int fpsNum, int fpsDen, int bitDepth) : mFd(-1) {
        if(numChannels <= 0 || sampleRate <= 0)
            throw std::runtime_error("Invalid format");

        std::vector<std::shared_ptr<bw64::Chunk>> additionalChunks;
        additionalChunks.push_back(CreateMetadata(fpsNum, fpsDen));

        mWriter = std::unique_ptr<bw64::Bw64Writer>(new bw64::Bw64Writer(
                output, numChannels, sampleRate, bitDepth, additionalChunks));
    }

    void AudioWriter::write(const std::vector<int16_t>& data, int numFrames) {
        mWriter->write(data.data(), numFrames);
    }
    
    void AudioWriter::write(const std::vector<float>& data, int numFrames) {
        mWriter->write(data.data(), numFrames);
    }
}
