/**
 * @file utils.hpp
 *
 * Collection of helper functions.
 */
#pragma once
#include <sstream>
#include <stdexcept>
#include <limits>
#include <stdint.h>
#include "chunks.hpp"
#include "internal_stream.hpp"

namespace bw64 {
  namespace utils {

    /// @brief Convert char array chunkIds to uint32_t
    inline constexpr uint32_t fourCC(char const p[5]) {
      return (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
    }
    /// @brief Convert uint32_t chunkId to string
    inline std::string fourCCToStr(uint32_t value) {
      return std::string(reinterpret_cast<char*>(&value), 4);
    }

    /// @brief Read a value from a stream
    template <typename T>
    void readValue(StreamWrapper& stream, T& dest) {
      stream.read(reinterpret_cast<char*>(&dest), sizeof(dest));
      if (stream.eof())
        throw std::runtime_error("file ended while reading value");
      if (!stream.good())
        throw std::runtime_error("file error while reading value");
    }

    /// @brief Read size bytes from stream into dest
    ///
    /// Dest may be null if size == 0. EOF and stream errors are checked.
    inline void readChunk(StreamWrapper& stream, char* dest, size_t size) {
      if (size) {
        stream.read(dest, size);
        if (stream.eof())
          throw std::runtime_error("file ended while reading chunk");
        if (!stream.good())
          throw std::runtime_error("file error while reading chunk");
      }
    }

    /// @brief Write a value to a stream
    template <typename T>
    void writeValue(StreamWrapper& stream, const T& src) {
      stream.write(reinterpret_cast<const char*>(&src), sizeof(src));
    }

    template <typename ChunkType>
    void writeChunk(StreamWrapper& stream, std::shared_ptr<ChunkType> chunk,
                    uint32_t chunkSizeForHeader) {
      writeValue(stream, chunk->id());
      writeValue(stream, chunkSizeForHeader);
      if (chunk->id() != fourCC("data")) {
        chunk->write(stream);
        if (chunk->size() % 2 == 1) {
          writeValue(stream, '\0');
        }
      }
    }

    inline void writeChunkPlaceholder(StreamWrapper& stream, uint32_t id, uint32_t size) {
      utils::writeValue(stream, id);
      utils::writeValue(stream, size);
      for (unsigned int i = 0; i < size; ++i) {
        writeValue(stream, '\0');
      }
    }

    /// @brief Decode (integer) PCM samples as float from char array
    template <typename T,
              typename = std::enable_if<std::is_floating_point<T>::value>>
    void decodePcmSamples(const char* inBuffer, T* outBuffer,
                          uint64_t numberOfSamples, uint16_t bitsPerSample) {
      uint16_t bytesPerSample = bitsPerSample / 8;
      if (bitsPerSample == 16) {
        for (uint64_t i = 0; i < numberOfSamples; ++i) {
          int16_t sampleValue = (inBuffer[i * bytesPerSample + 1] & 0xff) << 8 |
                                (inBuffer[i * bytesPerSample] & 0xff);
          outBuffer[i] = sampleValue;
        }
      } else if (bitsPerSample == 24) {
        for (uint64_t i = 0; i < numberOfSamples; ++i) {
          int32_t sampleValue =
              (inBuffer[i * bytesPerSample + 2] & 0xff) << 24 |
              (inBuffer[i * bytesPerSample + 1] & 0xff) << 16 |
              (inBuffer[i * bytesPerSample] & 0xff) << 8;
          outBuffer[i] = sampleValue / 2147483647.f;
        }
      } else if (bitsPerSample == 32) {
        for (uint64_t i = 0; i < numberOfSamples; ++i) {
          int32_t sampleValue =
              (inBuffer[i * bytesPerSample + 3] & 0xff) << 24 |
              (inBuffer[i * bytesPerSample + 2] & 0xff) << 16 |
              (inBuffer[i * bytesPerSample + 1] & 0xff) << 8 |
              (inBuffer[i * bytesPerSample] & 0xff);
          outBuffer[i] = sampleValue / 2147483647.f;
        }
      } else {
        std::stringstream errorString;
        errorString << "unsupported number of bits: " << bitsPerSample;
        throw std::runtime_error(errorString.str());
      }
    }

    /// @brief Limit sample to [-1,+1]
    template <typename T,
              typename = std::enable_if<std::is_floating_point<T>::value>>
    T clipSample(T value) {
      if (value > 1.f) {
        return 1.f;
      }
      if (value < -1.f) {
        return -1.f;
      }
      return value;
    }

    /// @brief Encode PCM samples from int16/float array to char array
    template <typename T,
              typename = std::enable_if<std::is_floating_point<T>::value>>
    void encodePcmSamples(const T* inBuffer, char* outBuffer,
                          uint64_t numberOfSamples, uint16_t bitsPerSample) {
      uint16_t bytesPerSample = bitsPerSample / 8;
      if (bitsPerSample == 16) {
          std::memcpy(outBuffer, inBuffer, numberOfSamples*bytesPerSample);
      } else if (bitsPerSample == 24) {
        for (uint64_t i = 0; i < numberOfSamples; ++i) {
          auto sampleValueClipped = clipSample(inBuffer[i]);
          int32_t sampleValue =
              static_cast<int32_t>(sampleValueClipped * 8388607.);
          outBuffer[i * bytesPerSample] = sampleValue & 0xff;
          outBuffer[i * bytesPerSample + 1] = (sampleValue >> 8) & 0xff;
          outBuffer[i * bytesPerSample + 2] = (sampleValue >> 16) & 0xff;
        }
      } else if (bitsPerSample == 32) {
        for (uint64_t i = 0; i < numberOfSamples; ++i) {
          auto sampleValueClipped = clipSample(inBuffer[i]);
          int32_t sampleValue =
              static_cast<int32_t>(sampleValueClipped * 2147483647.);
          outBuffer[i * bytesPerSample] = sampleValue & 0xff;
          outBuffer[i * bytesPerSample + 1] = (sampleValue >> 8) & 0xff;
          outBuffer[i * bytesPerSample + 2] = (sampleValue >> 16) & 0xff;
          outBuffer[i * bytesPerSample + 3] = (sampleValue >> 24) & 0xff;
        }
      } else {
        std::stringstream errorString;
        errorString << "unsupported number of bits: " << bitsPerSample;
        throw std::runtime_error(errorString.str());
      }
    }

    /// check x against the maximum value that To can hold
    template <typename To, typename From>
    void checkUpper(From x) {
      // additional parenthesis around numeric_limits are for preventing
      // conflict with max(a,b) macro on windows without requiring user to
      // define NOMINMAX

      // only need to do this check if From can hold values bigger than To
      if ((std::numeric_limits<From>::max)() >
          (std::numeric_limits<To>::max)()) {
        if (x > static_cast<From>((std::numeric_limits<To>::max)()))
          throw std::runtime_error("overflow");
      }
    }

    /// check x against the minimum value that To can hold
    template <typename To, typename From>
    void checkLower(From x) {
      using FromS = typename std::make_signed<From>::type;
      using ToS = typename std::make_signed<To>::type;

      // only need to do this check if From can hold values smaller than To
      //
      // convert limits to signed, otherwise this comparison can be promoted to
      // unsigned
      if (static_cast<FromS>((std::numeric_limits<From>::min)()) <
          static_cast<ToS>((std::numeric_limits<To>::min)())) {
        // from is signed, to maybe unsigned
        if (x < static_cast<FromS>((std::numeric_limits<To>::min)()))
          throw std::runtime_error("underflow");
      }
    }

    /// convert signed or unsigned integer x to To, checking for overflow and
    /// underflow
    template <typename To, typename From>
    To safeCast(From x) {
      checkUpper<To>(x);
      checkLower<To>(x);
      return static_cast<To>(x);
    }

    /// add x and y, checking for overflow and underflow. X and Y most both be
    /// representable by T; use safeCast to make sure
    template <typename T>
    T safeAdd(T x, T y) {
      if (((y > 0) && (x > ((std::numeric_limits<T>::max)() - y))) ||
          ((y < 0) && (x < ((std::numeric_limits<T>::min)() - y))))
        throw std::runtime_error(y > 0 ? "overflow" : "underflow");
      return x + y;
    }
  }  // namespace utils
}  // namespace bw64
