#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"
#include "esphome/components/media_source/media_source.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include "../http_request.h"

#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

namespace esphome {
namespace http_request {

enum class HTTPDecodingState : uint8_t {
  START_TASKS,
  DECODING,
  IDLE,
};

// Forward declaration
class HTTPMediaSource;

class HTTPMediaSource : public Component, public media_source::MediaSource, public Parented<HttpRequestComponent> {
 public:
  void setup() override;
  void loop() override;
  // TODO: implement dump_config() to log buffer size and task stack configuration

  // MediaSource interface implementation
  bool play_uri(const std::string &uri) override;
  void handle_command(media_source::MediaSourceCommand command) override;
  media_source::MediaSourceCapabilities get_capabilities() override;
  bool can_handle(const std::string &uri) const override {
    return uri.starts_with("http://") || uri.starts_with("https://");
  }

  void set_buffer_size(size_t buffer_size) { this->buffer_size_ = buffer_size; }
  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

 protected:
  size_t buffer_size_{24 * 1024};  // Ring buffer size between read and decode tasks
  bool task_stack_in_psram_{false};

  // Pipeline state
  std::string current_uri_;
  audio::AudioFileType current_audio_file_type_{audio::AudioFileType::NONE};
  HTTPDecodingState decoding_state_{HTTPDecodingState::IDLE};
  EventGroupHandle_t event_group_{nullptr};
  QueueHandle_t controls_queue_{nullptr};
  std::weak_ptr<RingBuffer> raw_file_ring_buffer_;
  TaskHandle_t read_task_handle_{nullptr};
  StaticTask_t read_task_stack_;
  StackType_t *read_task_stack_buffer_{nullptr};
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};

  static void read_task(void *params);
  static void decode_task(void *params);
};

}  // namespace http_request
}  // namespace esphome

#endif  // USE_ESP32
