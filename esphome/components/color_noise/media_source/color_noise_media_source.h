#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "noise_generator.h"

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace color_noise {

enum class ColorNoiseGenerationState : uint8_t {
  START_TASK,
  GENERATING,
  IDLE,
};

class ColorNoiseMediaSource : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;
  // TODO: implement dump_config() to log sample rate and default seed

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  bool can_handle(const std::string &uri) const override { return uri.starts_with("color-noise://"); }

  // Configuration setters
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_default_seed(uint32_t seed) { this->default_seed_ = seed; }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

 protected:
  static void generate_task(void *params);

  EventGroupHandle_t event_group_{nullptr};
  QueueHandle_t controls_queue_{nullptr};
  TaskHandle_t generate_task_handle_{nullptr};
  StaticTask_t generate_task_stack_;
  StackType_t *generate_task_stack_buffer_{nullptr};
  size_t total_samples_to_generate_{0};  // 0 = infinite playback, >0 = stop after this many samples
  size_t samples_generated_{0};          // Counter for tracking playback progress

  uint32_t seed_{0};
  uint32_t sample_rate_{16000};
  uint32_t default_seed_{0};

  int32_t amplitude_q15_{29490};

  ColorNoiseGenerationState generation_state_{ColorNoiseGenerationState::IDLE};
  NoiseType noise_type_{NoiseType::WHITE};
  bool paused_{false};
  bool task_stack_in_psram_{false};
};

}  // namespace color_noise
}  // namespace esphome

#endif  // USE_ESP32
