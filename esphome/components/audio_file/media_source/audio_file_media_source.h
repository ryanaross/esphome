#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace audio_file {

enum class AudioFileDecodingState : uint8_t {
  START_TASK,
  DECODING,
  IDLE,
};

struct NamedAudioFile {
  audio::AudioFile *file;
  const char *file_id;
};

// Forward declaration
class AudioFileMediaSource;

/// @brief Parameters passed to decode task
struct DecodeTaskParams {
  AudioFileMediaSource *source;
};

class AudioFileMediaSource : public Component, public media_source::MediaSource {
 public:
  void setup() override;
  void loop() override;
  // TODO: implement dump_config() to log registered audio files

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  media_source::MediaSourceCapabilities get_capabilities() override;
  bool can_handle(const std::string &uri) const override { return uri.starts_with("file://"); }

  void add_file(audio::AudioFile *media_file, const char *file_id) {
    this->files_.push_back(NamedAudioFile{media_file, file_id});
  }

  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

 protected:
  std::vector<NamedAudioFile> files_;
  bool task_stack_in_psram_{false};

  // Single pipeline context
  audio::AudioFile *current_file_{nullptr};
  AudioFileDecodingState decoding_state_{AudioFileDecodingState::IDLE};
  EventGroupHandle_t event_group_{nullptr};
  QueueHandle_t controls_queue_{nullptr};
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};

  static void decode_task(void *params);
};

}  // namespace audio_file
}  // namespace esphome

#endif  // USE_ESP32
