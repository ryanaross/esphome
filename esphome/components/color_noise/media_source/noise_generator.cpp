#include "noise_generator.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"

#include <algorithm>
#include <cmath>

namespace esphome {
namespace color_noise {

NoiseGenerator::NoiseGenerator() {
  // Seed from ESPHome's RNG; xorshift32 requires non-zero state
  uint32_t seed = random_uint32();
  this->prng_state_ = (seed == 0) ? 1 : seed;
}

void WhiteNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) {
  for (size_t i = 0; i < sample_count; i++) {
    int32_t white = static_cast<int16_t>(xorshift32(this->prng_state_) >> 16);  // Q15 white noise sample
    samples[i] = static_cast<int16_t>((white * amplitude_q15) >> 15);
  }
}

BrownNoiseGenerator::BrownNoiseGenerator(uint32_t sample_rate) : NoiseGenerator() {
  // Double precision is unnecessary, but avoids single precision so the calling task isn't locked to its current CPU
  // core on an ESP32

  // Calculate leakage coefficient (high-pass filter to prevent DC drift)
  double leakage_f = (sample_rate - 144.0) / sample_rate;
  if (leakage_f >= 0.9999) {
    leakage_f = 0.9999;
  }
  this->leakage_ = static_cast<int32_t>(std::round(leakage_f * 32768.0));

  // Calculate scaling coefficient (compensates for sample rate)
  double scaling_f = 9.0 / sqrt(static_cast<double>(sample_rate));
  if (scaling_f <= 0.01) {
    scaling_f = 0.01;
  }
  this->scaling_ = static_cast<int32_t>(std::round(scaling_f * 32768.0));
}

void BrownNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) {
  for (size_t i = 0; i < sample_count; i++) {
    // Generate white noise
    int32_t white = static_cast<int16_t>(xorshift32(this->prng_state_) >> 16);  // Q15 white noise sample

    // z = leakage * y + white * scaling (all Q15)
    int32_t z = ((this->leakage_ * this->y_accumulator_) >> 15) + ((white * this->scaling_) >> 15);

    // Check if |z| > 1.0 (in Q15, that's > 32767)
    int32_t abs_z = (z < 0) ? -z : z;

    if (abs_z > 32767) {
      // Reflection: reverse direction to prevent clipping
      this->y_accumulator_ = ((this->leakage_ * this->y_accumulator_) >> 15) - ((white * this->scaling_) >> 15);
    } else {
      this->y_accumulator_ = z;
    }

    // Apply amplitude scaling (both operands are Q15-bounded, so result fits int16_t)
    samples[i] = static_cast<int16_t>((this->y_accumulator_ * amplitude_q15) >> 15);
  }
}

void PinkNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count, int32_t amplitude_q15) {
  // scale by normalization factor 0.129f in Q15
  int32_t amplitude = (amplitude_q15 * 4227) >> 15;

  for (size_t i = 0; i < sample_count; i++) {
    // Generate white noise in Q15 format
    int32_t white = static_cast<int16_t>(xorshift32(this->prng_state_) >> 16);  // Q15 white noise sample

    // Update Paul Kellett's 6 filters (all in Q15)
    this->buffers_[0] = ((this->buffers_[0] * 32730) >> 15) + ((white * 1820) >> 15);
    this->buffers_[1] = ((this->buffers_[1] * 32552) >> 15) + ((white * 2460) >> 15);
    this->buffers_[2] = ((this->buffers_[2] * 31752) >> 15) + ((white * 5038) >> 15);
    this->buffers_[3] = ((this->buffers_[3] * 28393) >> 15) + ((white * 10175) >> 15);
    this->buffers_[4] = ((this->buffers_[4] * 18022) >> 15) + ((white * 17464) >> 15);
    this->buffers_[5] = ((this->buffers_[5] * -24961) >> 15) + ((white * -553) >> 15);

    // Sum all filter outputs + differentiator + scaled white
    int32_t pink = this->buffers_[0] + this->buffers_[1] + this->buffers_[2] + this->buffers_[3] + this->buffers_[4] +
                   this->buffers_[5] + this->buffers_[6] + ((white * 17569) >> 15);

    // Update differentiator for next iteration
    this->buffers_[6] = (white * 3798) >> 15;

    // Clamp to Q15 range before scaling to prevent overflow in the multiplication
    pink = std::clamp(pink, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX));

    // Apply amplitude scaling
    samples[i] = static_cast<int16_t>((pink * amplitude) >> 15);
  }
}

}  // namespace color_noise
}  // namespace esphome

#endif  // USE_ESP32
