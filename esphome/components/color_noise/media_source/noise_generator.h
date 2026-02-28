#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace color_noise {

enum class NoiseType : uint8_t {
  WHITE,
  BROWN,
  PINK,
};

class NoiseGenerator {
 public:
  NoiseGenerator();
  virtual ~NoiseGenerator() = default;
  virtual void generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) = 0;

 protected:
  uint32_t prng_state_;

  /// @brief xorshift32 PRNG for noise generation
  /// @param state PRNG state (will be modified)
  /// @return Random 32-bit value
  static inline uint32_t xorshift32(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
};

class WhiteNoiseGenerator : public NoiseGenerator {
 public:
  using NoiseGenerator::NoiseGenerator;
  void generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) override;
};

class BrownNoiseGenerator : public NoiseGenerator {
 public:
  BrownNoiseGenerator(uint32_t sample_rate);
  void generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) override;

 protected:
  int32_t y_accumulator_{0};
  int32_t leakage_{0};
  int32_t scaling_{0};
};

class PinkNoiseGenerator : public NoiseGenerator {
 public:
  using NoiseGenerator::NoiseGenerator;
  void generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) override;

 protected:
  std::array<int32_t, 7> buffers_{};
};

}  // namespace color_noise
}  // namespace esphome

#endif  // USE_ESP32
