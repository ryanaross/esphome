#include "audio_file_media_source.h"

#include "esphome/components/audio/audio_decoder.h"

namespace esphome {
namespace audio_file {

namespace {
struct AudioSinkAdapter : public audio::AudioSinkCallback {
  media_source::MediaSourceListener *listener;
  media_source::MediaSource *source;
  audio::AudioStreamInfo stream_info;

  size_t audio_sink_write(uint8_t *data, size_t length, TickType_t ticks_to_wait) override {
    return this->listener->on_media_output(this->source, data, length, ticks_to_wait, this->stream_info);
  }
};
}  // namespace

static const uint32_t DECODE_TASK_STACK_SIZE = 3 * 1024;

static const char *const TAG = "audio_file_media_source";

enum class SourceControls : uint8_t {
  START = 0,
  STOP = 1,
  PAUSE = 2,
  RESUME = 3,
};

struct ControlMessage {
  SourceControls control;
  audio::AudioFile *new_file{nullptr};
};

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),
  COMMAND_PAUSE = (1 << 1),
  TASK_STARTING = (1 << 7),
  TASK_RUNNING = (1 << 8),
  TASK_STOPPING = (1 << 9),
  TASK_STOPPED = (1 << 10),
  ALL_BITS = 0x00FFFFFF,  // All valid FreeRTOS event group bits
};

void AudioFileMediaSource::setup() {
  this->disable_loop();

  // Create event group and queue for the single pipeline
  this->event_group_ = xEventGroupCreate();
  this->controls_queue_ = xQueueCreate(3, sizeof(ControlMessage));
}

bool AudioFileMediaSource::play_uri(const std::string &uri) {
  // Check if source is already playing
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': source is busy", uri.c_str());
    return false;
  }

  // Validate URI starts with "file://"
  if (!uri.starts_with("file://")) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }

  // Strip "file://" prefix and find the file
  std::string file_id = uri.substr(7);  // "file://" is 7 characters

  for (const auto &named_file : this->files_) {
    if (named_file.file_id == file_id) {
      if (!this->is_ready() || this->is_failed()) {
        return false;
      }

      // Queue playback start
      ControlMessage message = {.control = SourceControls::START, .new_file = named_file.file};
      xQueueSend(this->controls_queue_, &message, 0);
      this->enable_loop_soon_any_context();
      return true;
    }
  }

  ESP_LOGE(TAG, "File not found: '%s'", file_id.c_str());
  return false;
}

void AudioFileMediaSource::loop() {
  // Process control messages
  ControlMessage incoming_control;
  if (xQueueReceive(this->controls_queue_, &incoming_control, 0)) {
    switch (incoming_control.control) {
      case SourceControls::START:
        this->current_file_ = incoming_control.new_file;
        this->decoding_state_ = AudioFileDecodingState::START_TASK;
        break;
      case SourceControls::STOP:
        if (this->decoding_state_ == AudioFileDecodingState::DECODING) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_STOP);
        }
        break;
      case SourceControls::PAUSE:
        if ((this->decoding_state_ == AudioFileDecodingState::DECODING) &&
            (this->get_state() == media_source::MediaSourceState::PLAYING)) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PAUSED);
        }
        break;
      case SourceControls::RESUME:
        if ((this->decoding_state_ == AudioFileDecodingState::DECODING) &&
            (this->get_state() == media_source::MediaSourceState::PAUSED)) {
          // Clear the pause command bit to resume
          xEventGroupClearBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PLAYING);
        }
        break;
    }
  }

  // Process state machine
  switch (this->decoding_state_) {
    case AudioFileDecodingState::START_TASK: {
      if (this->decode_task_handle_ == nullptr) {
        xEventGroupClearBits(this->event_group_, ALL_BITS);
        if (this->decode_task_stack_buffer_ == nullptr) {
          if (this->task_stack_in_psram_) {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
            this->decode_task_stack_buffer_ = stack_allocator.allocate(DECODE_TASK_STACK_SIZE);
          } else {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
            this->decode_task_stack_buffer_ = stack_allocator.allocate(DECODE_TASK_STACK_SIZE);
          }
        }
        if (this->decode_task_stack_buffer_ == nullptr) {
          ESP_LOGE(TAG, "Failed to allocate decode task stack");
          this->mark_failed();
          return;
        }

        auto *params = new DecodeTaskParams{this};
        this->decode_task_handle_ = xTaskCreateStatic(decode_task, "AudioFileDec", DECODE_TASK_STACK_SIZE, params, 1,
                                                      this->decode_task_stack_buffer_, &this->decode_task_stack_);
        if (this->decode_task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create decode task");
          delete params;
          this->mark_failed();
          return;
        }
      }
      ESP_LOGD(TAG, "Started decode task");
      this->decoding_state_ = AudioFileDecodingState::DECODING;
      break;
    }
    case AudioFileDecodingState::DECODING: {
      EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

      if (event_bits & TASK_STARTING) {
        ESP_LOGD(TAG, "Decode task starting");
        xEventGroupClearBits(this->event_group_, TASK_STARTING);
      }

      if (event_bits & TASK_RUNNING) {
        ESP_LOGD(TAG, "Decode task running");
        xEventGroupClearBits(this->event_group_, TASK_RUNNING);
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }

      if (event_bits & TASK_STOPPING) {
        ESP_LOGD(TAG, "Decode task stopping");
        xEventGroupClearBits(this->event_group_, TASK_STOPPING);
      }

      if (event_bits & TASK_STOPPED) {
        ESP_LOGD(TAG, "Decode task stopped");
        xEventGroupClearBits(this->event_group_, TASK_STOPPED | COMMAND_STOP | COMMAND_PAUSE);

        vTaskDelete(this->decode_task_handle_);
        this->decode_task_handle_ = nullptr;
        if (this->decode_task_stack_buffer_ != nullptr) {
          if (this->task_stack_in_psram_) {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
            stack_allocator.deallocate(this->decode_task_stack_buffer_, DECODE_TASK_STACK_SIZE);
          } else {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
            stack_allocator.deallocate(this->decode_task_stack_buffer_, DECODE_TASK_STACK_SIZE);
          }
          this->decode_task_stack_buffer_ = nullptr;
        }
        this->set_state_(media_source::MediaSourceState::IDLE);
        this->decoding_state_ = AudioFileDecodingState::IDLE;
      }
      break;
    }
    case AudioFileDecodingState::IDLE: {
      // Nothing to do when idle
      break;
    }
  }

  if (this->decoding_state_ == AudioFileDecodingState::IDLE) {
    this->disable_loop();
  }
}

void AudioFileMediaSource::handle_command(media_source::MediaSourceCommand command) {
  if (this->controls_queue_ == nullptr) {
    return;
  }

  ControlMessage message;
  switch (command) {
    case media_source::MEDIA_SOURCE_COMMAND_END:
      // Intentional fallthrough
    case media_source::MEDIA_SOURCE_COMMAND_STOP: {
      if (this->decoding_state_ == AudioFileDecodingState::DECODING) {
        message.control = SourceControls::STOP;
        xQueueSend(this->controls_queue_, &message, 0);
      }
      break;
    }
    case media_source::MEDIA_SOURCE_COMMAND_PAUSE: {
      message.control = SourceControls::PAUSE;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    case media_source::MEDIA_SOURCE_COMMAND_PLAY: {
      message.control = SourceControls::RESUME;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    default:
      break;
  }
}

media_source::MediaSourceCapabilities AudioFileMediaSource::get_capabilities() {
  media_source::MediaSourceCapabilities caps;
  caps.supports_pause = true;  // File playback can be paused
  return caps;
}

void AudioFileMediaSource::decode_task(void *params) {
  auto *task_params = static_cast<DecodeTaskParams *>(params);
  AudioFileMediaSource *this_source = task_params->source;
  delete task_params;

  {
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STARTING);

    // 0 bytes for input transfer buffer makes it an inplace buffer
    std::unique_ptr<audio::AudioDecoder> decoder = make_unique<audio::AudioDecoder>(0, 4096);

    esp_err_t err = decoder->start(this_source->current_file_->file_type);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to start decoder: %s", esp_err_to_name(err));
      xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPED);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // Add the file as a const data source
    decoder->add_source(this_source->current_file_->data, this_source->current_file_->length);

    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_RUNNING);

    AudioSinkAdapter audio_sink;
    bool has_stream_info = false;

    while (true) {
      EventBits_t event_bits = xEventGroupGetBits(this_source->event_group_);

      if (event_bits & EventGroupBits::COMMAND_STOP) {
        break;
      }

      decoder->set_pause_output_state(event_bits & EventGroupBits::COMMAND_PAUSE);

      // Will stop gracefully once finished with the current file
      audio::AudioDecoderState decoder_state = decoder->decode(true);

      if (decoder_state == audio::AudioDecoderState::FINISHED) {
        ESP_LOGD(TAG, "Decoding finished");
        break;
      } else if (decoder_state == audio::AudioDecoderState::FAILED) {
        ESP_LOGE(TAG, "Decoding failed");
        break;
      }

      if (!has_stream_info && decoder->get_audio_stream_info().has_value()) {
        ESP_LOGD(TAG, "Got stream info from decoder");
        has_stream_info = true;

        audio::AudioStreamInfo stream_info = decoder->get_audio_stream_info().value();

        if (stream_info.get_bits_per_sample() != 16) {
          ESP_LOGE(TAG, "Incompatible bits per sample. Only 16 bits per sample is supported");
          break;
        } else if ((stream_info.get_channels() > 2)) {
          ESP_LOGE(TAG, "Incompatible number of channels. Only 1 or 2 channel audio is supported.");
          break;
        } else {
          ESP_LOGD(TAG, "Bits per sample: %d, Channels: %d, Sample rate: %d", stream_info.get_bits_per_sample(),
                   stream_info.get_channels(), stream_info.get_sample_rate());

          if (this_source->get_listener() != nullptr) {
            audio_sink.listener = this_source->get_listener();
            audio_sink.source = this_source;
            audio_sink.stream_info = stream_info;
            esp_err_t err = decoder->add_sink(&audio_sink);
            if (err != ESP_OK) {
              ESP_LOGE(TAG, "Failed to add sink to decoder: %s", esp_err_to_name(err));
              break;
            }
            ESP_LOGD(TAG, "Successfully added callback sink to decoder");
          } else {
            ESP_LOGE(TAG, "Listener is not set! Make sure the AudioFileMediaSource is added to "
                          "media_sources in your YAML config");
            break;
          }
        }
      }
    }
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPING);
  }
  xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPED);
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace audio_file
}  // namespace esphome
