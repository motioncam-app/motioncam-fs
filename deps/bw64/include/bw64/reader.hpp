/// @file reader.hpp
#pragma once
#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <type_traits>
#include <vector>
#include "chunks.hpp"
#include "utils.hpp"
#include "parser.hpp"
#include "internal_stream.hpp"

#include <iostream>

namespace bw64 {

  /**
   * @brief Representation of a BW64 file
   *
   * Normally, you will create an instance of this class using bw64::readFile().
   *
   * This is a
   * [RAII](https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization)
   * class, meaning that the file will be openend and initialized (parse header,
   * format etc.) on construction, and closed on destruction.
   */
  class Bw64Reader {
   public:
      
   void init() {
      readRiffChunk();
      if (fileFormat_ == utils::fourCC("BW64") ||
          fileFormat_ == utils::fourCC("RF64")) {
        auto chunkHeader = parseHeader();
        if (chunkHeader.id != utils::fourCC("ds64")) {
          throw std::runtime_error(
              "mandatory ds64 chunk for BW64 or RF64 file not found");
        }
        auto ds64Chunk =
            parseDataSize64Chunk(*fileStream_, chunkHeader.id, chunkHeader.size);
        chunks_.push_back(ds64Chunk);
        chunkHeaders_.push_back(chunkHeader);
      }
      parseChunkHeaders();
      for (auto chunkHeader : chunkHeaders_) {
        if (chunkHeader.id != utils::fourCC("ds64")) {
          auto chunk = parseChunk(*fileStream_, chunkHeader);
          chunks_.push_back(chunk);
        }
      }

      auto fmtChunk = formatChunk();
      if (!fmtChunk) {
        throw std::runtime_error("mandatory fmt chunk not found");
      }
      channelCount_ = fmtChunk->channelCount();
      formatTag_ = fmtChunk->formatTag();
      sampleRate_ = fmtChunk->sampleRate();
      bitsPerSample_ = fmtChunk->bitsPerSample();

      if (!dataChunk())
        throw std::runtime_error("mandatory data chunk not found");

      seek(0);
    }
      
    /**
     * @brief Open a new BW64 file for reading
     *
     * Opens a new BW64 file for reading, parses the whole file to read the
     * format and identify all chunks in it.
     *
     * @note For convenience, you might consider using the `readFile` helper
     * function.
     */
    Bw64Reader(const char* filename) {
        fileStream_ = std::unique_ptr<StreamWrapper>(new FileStreamWrapper(filename, false));
      init();
    }

    Bw64Reader(const int fd) {
      fileStream_ = std::unique_ptr<StreamWrapper>(new FileStreamWrapper(fd, false));
      init();
    }

    /**
     * @brief Bw64Reader destructor
     *
     * The destructor will automatically close the file opened in the
     * constructor.
     */
    ~Bw64Reader() { fileStream_ = nullptr; }

    /// @brief Get file format (RIFF, BW64 or RF64)
    uint32_t fileFormat() const { return fileFormat_; }
    /// @brief Get file size
    uint32_t fileSize() const { return fileSize_; }
    /// @brief Get format tag
    uint16_t formatTag() const { return formatTag_; };
    /// @brief Get number of channels
    uint16_t channels() const { return channelCount_; };
    /// @brief Get sample rate
    uint32_t sampleRate() const { return sampleRate_; };
    /// @brief Get bit depth
    uint16_t bitDepth() const { return bitsPerSample_; };
    /// @brief Get number of frames
    uint64_t numberOfFrames() const {
      return dataChunk()->size() / blockAlignment();
    }
    /// @brief Get block alignment
    uint16_t blockAlignment() const {
      return utils::safeCast<uint16_t>(static_cast<uint32_t>(channels()) *
                                       bitDepth() / 8);
    }

    template <typename ChunkType>
    std::vector<std::shared_ptr<ChunkType>> chunksWithId(
        const std::vector<Chunk>& chunks, uint32_t chunkId) const {
      std::vector<char> foundChunks;
      auto chunk =
          std::copy_if(chunks.begin(), chunks.end(), foundChunks.begin(),
                       [chunkId](const std::shared_ptr<Chunk> chunk) {
                         return chunk->id() == chunkId;
                       });
      return foundChunks;
    }

    template <typename ChunkType>
    std::shared_ptr<ChunkType> chunk(
        const std::vector<std::shared_ptr<Chunk>>& chunks,
        uint32_t chunkId) const {
      auto chunk = std::find_if(chunks.begin(), chunks.end(),
                                [chunkId](const std::shared_ptr<Chunk> chunk) {
                                  return chunk->id() == chunkId;
                                });
      if (chunk != chunks.end()) {
        return std::static_pointer_cast<ChunkType>(*chunk);
      } else {
        return nullptr;
      }
    }

    /**
     * @brief Get 'ds64' chunk
     *
     * @returns `std::shared_ptr` to DataSize64Chunk if present and otherwise
     * a nullptr.
     */
    std::shared_ptr<DataSize64Chunk> ds64Chunk() const {
      return chunk<DataSize64Chunk>(chunks_, utils::fourCC("ds64"));
    }
    /**
     * @brief Get 'fmt ' chunk
     *
     * @returns `std::shared_ptr` to FormatInfoChunk if present and otherwise
     * a nullptr.
     */
    std::shared_ptr<FormatInfoChunk> formatChunk() const {
      return chunk<FormatInfoChunk>(chunks_, utils::fourCC("fmt "));
    }
    /**
     * @brief Get 'data' chunk
     *
     * @warning This method usually should not be called, as the acces to the
     * DataChunk is handled seperately by the Bw64Reader class .
     *
     * @returns `std::shared_ptr` to DataChunk if present and otherwise
     * a nullptr.
     */
    std::shared_ptr<DataChunk> dataChunk() const {
      return chunk<DataChunk>(chunks_, utils::fourCC("data"));
    }
    /**
     * @brief Get 'chna' chunk
     *
     * @returns `std::shared_ptr` to ChnaChunk if present and otherwise a
     * nullptr.
     */
    std::shared_ptr<ChnaChunk> chnaChunk() const {
      return chunk<ChnaChunk>(chunks_, utils::fourCC("chna"));
    }
    /**
     * @brief Get 'axml' chunk
     *
     * @returns `std::shared_ptr` to AxmlChunk if present and otherwise a
     * nullptr.
     */
    std::shared_ptr<AxmlChunk> axmlChunk() const {
      return chunk<AxmlChunk>(chunks_, utils::fourCC("axml"));
    }

    /**
     * @brief Get list of all chunks which are present in the file
     */
    std::vector<ChunkHeader> chunks() const { return chunkHeaders_; }

    /**
     * @brief Check if a chunk with the given id is present
     */
    bool hasChunk(uint32_t id) const {
      auto foundHeader = std::find_if(
          chunkHeaders_.begin(), chunkHeaders_.end(),
          [id](const ChunkHeader header) { return header.id == id; });
      if (foundHeader == chunkHeaders_.end()) {
        return false;
      } else {
        return true;
      }
    }

    /**
     * @brief Seek a frame position in the DataChunk
     */
    void seek(int32_t offset, std::ios_base::seekdir way = std::ios::beg) {
      // where to seek relative to according to way
      int64_t startFrame = 0;
      if (way == std::ios::cur) {
        startFrame = tell();
      } else if (way == std::ios::beg) {
        startFrame = 0;
      } else if (way == std::ios::end) {
        startFrame = numberOfFrames();
      }

      // requested frame number, clamped to a frame within the data chunk
      int64_t frame = startFrame + offset;
      if (frame < 0)
        frame = 0;
      else if (frame > numberOfFrames())
        frame = numberOfFrames();

      // the position in the file of the frame
      const int64_t dataStartPos =
          getChunkHeader(utils::fourCC("data")).position + 8;
      const int64_t framePos =
          dataStartPos + frame * static_cast<int64_t>(blockAlignment());

      fileStream_->seekg(framePos);

      if (!fileStream_->good())
        throw std::runtime_error("file error while seeking");
    }

    /**
     * @brief Read frames from dataChunk
     *
     * @param[out] outBuffer Buffer to write the samples to
     * @param[in]  frames    Number of frames to read
     *
     * @returns number of frames read
     */
    template <typename T,
              typename = std::enable_if<std::is_floating_point<T>::value>>
    uint64_t read(T* outBuffer, uint64_t frames) {
      if (tell() + frames > numberOfFrames()) {
        frames = numberOfFrames() - tell();
      }

      if (frames) {
        rawDataBuffer_.resize(frames * blockAlignment());
        fileStream_->read(rawDataBuffer_.data(), frames * blockAlignment());
        if (fileStream_->eof())
          throw std::runtime_error("file ended while reading frames");
        if (!fileStream_->good())
          throw std::runtime_error("file error while reading frames");

        utils::decodePcmSamples(rawDataBuffer_.data(), outBuffer,
                                frames * channels(), bitDepth());
      }

      return frames;
    }

    /**
     * @brief Tell the current frame position of the dataChunk
     *
     * @returns current frame position of the dataChunk
     */
    uint64_t tell() {
      return ((uint64_t)fileStream_->tellg() -
              getChunkHeader(utils::fourCC("data")).position - 8u) /
             formatChunk()->blockAlignment();
    }

    /**
     * @brief Check if end of data is reached
     *
     * @returns `true` if end of data is reached and otherwise `false`
     */
    bool eof() { return tell() == numberOfFrames(); }

   private:
    void readRiffChunk() {
      uint32_t riffType;
      utils::readValue(*fileStream_, fileFormat_);
      utils::readValue(*fileStream_, fileSize_);
      utils::readValue(*fileStream_, riffType);

      if (fileFormat_ != utils::fourCC("RIFF") &&
          fileFormat_ != utils::fourCC("BW64") &&
          fileFormat_ != utils::fourCC("RF64")) {
        throw std::runtime_error("File is not a RIFF, BW64 or RF64 file.");
      }
      if (riffType != utils::fourCC("WAVE")) {
        throw std::runtime_error("File is not a WAVE file.");
      }
    }

    ChunkHeader getChunkHeader(uint32_t id) {
      auto foundHeader = std::find_if(
          chunkHeaders_.begin(), chunkHeaders_.end(),
          [id](const ChunkHeader header) { return header.id == id; });
      if (foundHeader != chunkHeaders_.end()) {
        return *foundHeader;
      }
      std::stringstream errorMsg;
      errorMsg << "no chunk with id '" << utils::fourCCToStr(id) << "' found";
      throw std::runtime_error(errorMsg.str());
    }

    ChunkHeader parseHeader() {
      uint32_t chunkId;
      uint32_t chunkSize;
      uint64_t position = fileStream_->tellg();
      utils::readValue(*fileStream_, chunkId);
      utils::readValue(*fileStream_, chunkSize);
      uint64_t chunkSize64 = getChunkSize64(chunkId, chunkSize);
      return ChunkHeader(chunkId, chunkSize64, position);
    }

    uint64_t getChunkSize64(uint32_t id, uint64_t chunkSize) {
      if (ds64Chunk()) {
        if (id == utils::fourCC("BW64") || id == utils::fourCC("RF64")) {
          return ds64Chunk()->bw64Size();
        }
        if (id == utils::fourCC("data")) {
          return ds64Chunk()->dataSize();
        }
        if (ds64Chunk()->hasChunkSize(id)) {
          return ds64Chunk()->getChunkSize(id);
        }
      }
      return chunkSize;
    }

    void parseChunkHeaders() {
      // get the absolute end of the file
      const std::streamoff start = fileStream_->tellg();
      fileStream_->seekg(0, std::ios::end);
      const std::streamoff end = fileStream_->tellg();
      fileStream_->seekg(start, std::ios::beg);

      const std::streamoff header_size = 8;

      while (fileStream_->tellg() + header_size <= end) {
        auto chunkHeader = parseHeader();

        // determine chunk size, skipping a padding byte
        std::streamoff chunk_size =
            utils::safeCast<std::streamoff>(chunkHeader.size);
        if (chunk_size % 2 != 0)
          chunk_size = utils::safeAdd<std::streamoff>(chunk_size, 1);

        std::streamoff chunk_end =
            utils::safeAdd<std::streamoff>(fileStream_->tellg(), chunk_size);

        if (chunk_end > end)
          throw std::runtime_error("chunk ends after end of file");

        fileStream_->seekg(chunk_size, std::ios::cur);
        if (!fileStream_->good())
          throw std::runtime_error("file error while seeking past chunk");

        chunkHeaders_.push_back(chunkHeader);
      }
    }

    std::unique_ptr<StreamWrapper> fileStream_;
    uint32_t fileFormat_;
    uint32_t fileSize_;
    uint16_t channelCount_;
    uint32_t sampleRate_;
    uint16_t formatTag_;
    uint16_t bitsPerSample_;

    std::vector<char> rawDataBuffer_;
    std::vector<std::shared_ptr<Chunk>> chunks_;
    std::vector<ChunkHeader> chunkHeaders_;
  };
}  // namespace bw64
