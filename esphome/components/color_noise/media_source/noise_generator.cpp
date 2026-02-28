#include "noise_generator.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cmath>

namespace esphome {
namespace color_noise {

NoiseGenerator::NoiseGenerator(uint32_t seed, int32_t amplitude_q15) : amplitude_q15_(amplitude_q15) {
  // xorshift32 doesn't work with zero state
  this->prng_state_ = (seed == 0) ? 0xDEADBEEF : seed;
}

void WhiteNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count) {
  for (size_t i = 0; i < sample_count; i++) {
    uint32_t random = xorshift32(this->prng_state_);
    samples[i] = static_cast<int16_t>((static_cast<int32_t>(random) * this->amplitude_q15_) >> 15);
  }
}

BrownNoiseGenerator::BrownNoiseGenerator(uint32_t seed, int32_t amplitude_q15, uint32_t sample_rate)
    : NoiseGenerator(seed, amplitude_q15) {
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

void BrownNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count) {
  for (size_t i = 0; i < sample_count; i++) {
    // Generate white noise
    int32_t white = static_cast<int16_t>(xorshift32(this->prng_state_) >> 16);

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

    // Apply amplitude and clamp
    int32_t result = (this->y_accumulator_ * this->amplitude_q15_) >> 15;
    samples[i] =
        static_cast<int16_t>(std::clamp(result, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
  }
}

void PinkNoiseGenerator::generate_samples(int16_t *samples, size_t sample_count) {
  // scale by normalization factor 0.129f in Q15
  int32_t amplitude = (this->amplitude_q15_ * 4227) >> 15;

  for (size_t i = 0; i < sample_count; i++) {
    // Generate white noise in Q15 format
    int32_t white = static_cast<int16_t>(xorshift32(this->prng_state_) >> 16);

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

    // Apply amplitude and clamp
    int32_t result = (pink * amplitude) >> 15;
    samples[i] =
        static_cast<int16_t>(std::clamp(result, static_cast<int32_t>(INT16_MIN), static_cast<int32_t>(INT16_MAX)));
  }
}

}  // namespace color_noise
}  // namespace esphome

#endif  // USE_ESP32
