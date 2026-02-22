#pragma once

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace color_noise {

enum class NoiseType : uint8_t {
  WHITE,
  BROWN,
  PINK,
};

enum class ColorNoiseGenerationState : uint8_t {
  START_TASK,
  GENERATING,
  IDLE,
};

// Forward declaration
class ColorNoiseMediaSource;

/// @brief Context for a single pipeline's color noise generation
struct ColorNoiseSourcePipeline {
  uint32_t seed{0};
  NoiseType noise_type{NoiseType::WHITE};
  bool paused{false};
  ColorNoiseGenerationState generation_state{ColorNoiseGenerationState::IDLE};
  EventGroupHandle_t event_group{nullptr};
  QueueHandle_t controls_queue{nullptr};
  TaskHandle_t generate_task_handle{nullptr};
  StaticTask_t generate_task_stack;
  StackType_t *generate_task_stack_buffer{nullptr};
  size_t total_samples_to_generate{0};  // 0 = infinite playback, >0 = stop after this many samples
  size_t samples_generated{0};          // Counter for tracking playback progress
  // Brown noise state
  int32_t brown_y_accumulator{0};
  int32_t brown_leakage{0};
  int32_t brown_scaling{0};
  // Pink noise state (Voss-McCartney algorithm with 5 octave generators)
  std::array<int32_t, 7> pink_buffers{};
  int32_t amplitude_q15{29490};
};

/// @brief Parameters passed to generate task
struct GenerateTaskParams {
  ColorNoiseMediaSource *source;
};

class ColorNoiseMediaSource : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  media_source::MediaSourceCapabilities get_capabilities() override;
  bool can_handle(const std::string &uri) const override { return uri.starts_with("color-noise://"); }

  // Configuration setters
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_default_seed(uint32_t seed) { this->default_seed_ = seed; }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

 protected:
  ColorNoiseSourcePipeline pipeline_ctx_;
  uint32_t sample_rate_{16000};
  uint32_t default_seed_{0};
  bool task_stack_in_psram_{false};

  static void generate_task(void *params);

  /// @brief xorshift32 PRNG for noise generation
  /// @param state PRNG state (will be modified)
  /// @return Random 32-bit value
  static inline uint32_t xorshift32(uint32_t &state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }

  /// @brief Generate white noise samples using xorshift32 PRNG
  static void generate_white_noise_samples(int16_t *samples, size_t sample_count, uint32_t &prng_state,
                                           int32_t amplitude);

  /// @brief Generate brown noise samples (random walk / integrated white noise)
  static void generate_brown_noise_samples(int16_t *samples, size_t sample_count, uint32_t &prng_state,
                                           int32_t &y_accumulator, int32_t leakage, int32_t scaling, int32_t amplitude);

  static void initialize_brown_coefficients(uint32_t sample_rate, int32_t &leakage, int32_t &scaling);

  /// @brief Generate pink noise samples using Voss-McCartney algorithm
  static void generate_pink_noise_samples(int16_t *samples, size_t sample_count, uint32_t &prng_state,
                                          std::array<int32_t, 7> &buffers, int32_t amplitude);
};

}  // namespace color_noise
}  // namespace esphome
