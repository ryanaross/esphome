#include "color_noise_media_source.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio_transfer_buffer.h"

#include <cstdlib>
#include <memory>

namespace esphome {
namespace color_noise {

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

static const uint32_t GENERATE_TASK_STACK_SIZE = 3 * 1024;
static const uint32_t READ_WRITE_TIMEOUT_MS = 20;

static const char *const TAG = "color_noise_media_source";

enum class SourceControls : uint8_t {
  START = 0,
  STOP = 1,
  PAUSE = 2,
  RESUME = 3,
};

struct ControlMessage {
  SourceControls control;
  size_t total_samples_to_generate{0};  // 0 = infinite playback
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

bool ColorNoiseMediaSource::play_uri(const std::string &uri) {
  // Check if pipeline is already playing
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': pipeline is busy", uri.c_str());
    return false;
  }

  // Validate URI starts with "color-noise://"
  if (!uri.starts_with("color-noise://")) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }

  // Parse noise type from host part (between "color-noise://" and "/")
  size_t host_start = 14;  // Length of "color-noise://"
  size_t host_end = uri.find('/', host_start);
  if (host_end == std::string::npos) {
    ESP_LOGE(TAG, "Invalid URI format: '%s' (missing '/' after host)", uri.c_str());
    return false;
  }

  size_t host_len = host_end - host_start;
  NoiseType noise_type;

  if (uri.compare(host_start, host_len, "white") == 0) {
    noise_type = NoiseType::WHITE;
  } else if (uri.compare(host_start, host_len, "brown") == 0) {
    noise_type = NoiseType::BROWN;
  } else if (uri.compare(host_start, host_len, "pink") == 0) {
    noise_type = NoiseType::PINK;
  } else {
    ESP_LOGE(TAG, "Invalid noise type in URI: '%s'. Must be 'white', 'brown', or 'pink'", uri.c_str());
    return false;
  }

  if (!this->is_ready() || this->is_failed()) {
    return false;
  }

  // Store the noise type
  this->noise_type_ = noise_type;

  // Parse URI for optional duration parameter
  // Format: color-noise://<type>/ or color-noise://<type>/?duration=10
  // where <type> is 'white', 'brown', or 'pink'
  uint32_t duration_seconds = 0;  // 0 = infinite playback

  size_t query_pos = uri.find('?');
  if (query_pos != std::string::npos) {
    size_t query_start = query_pos + 1;

    // Simple query parser for duration parameter
    size_t duration_pos = uri.find("duration=", query_start);
    if (duration_pos != std::string::npos && (duration_pos == query_start || uri[duration_pos - 1] == '&')) {
      duration_seconds = std::strtoul(uri.c_str() + duration_pos + 9, nullptr, 10);
    }
  }

  // Calculate total samples to generate based on duration
  size_t total_samples = 0;
  if (duration_seconds > 0) {
    // Use AudioStreamInfo to convert duration to samples
    audio::AudioStreamInfo stream_info(16, 1, this->sample_rate_);
    total_samples = stream_info.ms_to_samples(duration_seconds * 1000);
  }

  const char *noise_type_name = (noise_type == NoiseType::WHITE)   ? "white"
                                : (noise_type == NoiseType::BROWN) ? "brown"
                                                                   : "pink";

  if (duration_seconds > 0) {
    ESP_LOGD(TAG, "Playing %s noise, duration: %u seconds (%zu samples)", noise_type_name, duration_seconds,
             total_samples);
  } else {
    ESP_LOGD(TAG, "Playing %s noise (infinite playback)", noise_type_name);
  }

  // Queue playback start
  ControlMessage message = {.control = SourceControls::START, .total_samples_to_generate = total_samples};
  xQueueSend(this->controls_queue_, &message, 0);
  this->enable_loop_soon_any_context();
  return true;
}

void ColorNoiseMediaSource::setup() {
  this->disable_loop();

  // Create event group and queue upfront so they're available when play_uri is called
  this->event_group_ = xEventGroupCreate();
  this->controls_queue_ = xQueueCreate(3, sizeof(ControlMessage));
}

void ColorNoiseMediaSource::loop() {
  // Process control messages
  ControlMessage incoming_control;
  if (xQueueReceive(this->controls_queue_, &incoming_control, 0)) {
    switch (incoming_control.control) {
      case SourceControls::START:
        this->total_samples_to_generate_ = incoming_control.total_samples_to_generate;
        this->samples_generated_ = 0;  // Reset sample counter
        this->paused_ = false;
        this->generation_state_ = ColorNoiseGenerationState::START_TASK;
        break;
      case SourceControls::STOP:
        if (this->generation_state_ == ColorNoiseGenerationState::GENERATING) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_STOP);
        }
        break;
      case SourceControls::PAUSE:
        if ((this->generation_state_ == ColorNoiseGenerationState::GENERATING) &&
            (this->get_state() == media_source::MediaSourceState::PLAYING)) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PAUSED);
        }
        break;
      case SourceControls::RESUME:
        if ((this->generation_state_ == ColorNoiseGenerationState::GENERATING) &&
            (this->get_state() == media_source::MediaSourceState::PAUSED)) {
          // Clear the pause command bit to resume
          xEventGroupClearBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PLAYING);
        }
        break;
    }
  }

  // Process pipeline state machine
  switch (this->generation_state_) {
    case ColorNoiseGenerationState::START_TASK: {
      // Event group and queue already created in setup()
      // Start the task
      if (this->generate_task_handle_ == nullptr) {
        xEventGroupClearBits(this->event_group_, ALL_BITS);
        if (this->generate_task_stack_buffer_ == nullptr) {
          if (this->task_stack_in_psram_) {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
            this->generate_task_stack_buffer_ = stack_allocator.allocate(GENERATE_TASK_STACK_SIZE);
          } else {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
            this->generate_task_stack_buffer_ = stack_allocator.allocate(GENERATE_TASK_STACK_SIZE);
          }
        }
        if (this->generate_task_stack_buffer_ == nullptr) {
          ESP_LOGE(TAG, "Failed to allocate generate task stack");
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->generation_state_ = ColorNoiseGenerationState::IDLE;
          this->status_momentary_error("task_alloc", 15000);
          return;
        }

        this->generate_task_handle_ = xTaskCreateStatic(generate_task, "NoiseGen", GENERATE_TASK_STACK_SIZE, this, 1,
                                                        this->generate_task_stack_buffer_, &this->generate_task_stack_);
        if (this->generate_task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create generate task");
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->generation_state_ = ColorNoiseGenerationState::IDLE;
          this->status_momentary_error("task_create", 15000);
          return;
        }
      }
      ESP_LOGD(TAG, "Started generate task");
      this->generation_state_ = ColorNoiseGenerationState::GENERATING;
      break;
    }
    case ColorNoiseGenerationState::GENERATING: {
      // Only state when we handle event group bits
      EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

      if (event_bits & TASK_STARTING) {
        ESP_LOGD(TAG, "Task starting");
        xEventGroupClearBits(this->event_group_, TASK_STARTING);
      }

      if (event_bits & TASK_RUNNING) {
        ESP_LOGD(TAG, "Task running");
        xEventGroupClearBits(this->event_group_, TASK_RUNNING);
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }

      if (event_bits & TASK_STOPPING) {
        ESP_LOGD(TAG, "Task stopping");
        xEventGroupClearBits(this->event_group_, TASK_STOPPING);
      }

      if (event_bits & TASK_STOPPED) {
        ESP_LOGD(TAG, "Task stopped");
        xEventGroupClearBits(this->event_group_, TASK_STOPPED | COMMAND_STOP | COMMAND_PAUSE);

        vTaskDelete(this->generate_task_handle_);
        this->generate_task_handle_ = nullptr;
        if (this->generate_task_stack_buffer_ != nullptr) {
          if (this->task_stack_in_psram_) {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_EXTERNAL);
            stack_allocator.deallocate(this->generate_task_stack_buffer_, GENERATE_TASK_STACK_SIZE);
          } else {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
            stack_allocator.deallocate(this->generate_task_stack_buffer_, GENERATE_TASK_STACK_SIZE);
          }
          this->generate_task_stack_buffer_ = nullptr;
        }
        this->set_state_(media_source::MediaSourceState::IDLE);
        this->generation_state_ = ColorNoiseGenerationState::IDLE;
      }
      break;
    }
    case ColorNoiseGenerationState::IDLE: {
      // Nothing to do when idle
      break;
    }
  }

  // Check if we should disable loop when idle
  if (this->generation_state_ == ColorNoiseGenerationState::IDLE) {
    this->disable_loop();
  }
}

void ColorNoiseMediaSource::handle_command(media_source::MediaSourceCommand command) {
  if (this->controls_queue_ == nullptr) {
    return;
  }

  ControlMessage message;
  switch (command) {
    case media_source::MediaSourceCommand::STOP: {
      if (this->generation_state_ == ColorNoiseGenerationState::GENERATING) {
        message.control = SourceControls::STOP;
        xQueueSend(this->controls_queue_, &message, 0);
      }
      break;
    }
    case media_source::MediaSourceCommand::PAUSE: {
      message.control = SourceControls::PAUSE;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    case media_source::MediaSourceCommand::PLAY: {
      message.control = SourceControls::RESUME;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    default:
      break;
  }
}

void ColorNoiseMediaSource::generate_task(void *params) {
  ColorNoiseMediaSource *this_source = static_cast<ColorNoiseMediaSource *>(params);

  {
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STARTING);

    // Create AudioStreamInfo with 1 channel (mono)
    audio::AudioStreamInfo stream_info(16, 1, this_source->sample_rate_);

    ESP_LOGD(TAG, "Bits per sample: %d, Channels: %d, Sample rate: %u", stream_info.get_bits_per_sample(),
             stream_info.get_channels(), stream_info.get_sample_rate());

    // Check if listener is set before using it
    if (this_source->get_listener() == nullptr) {
      ESP_LOGE(TAG, "Listener is not set! Make sure the ColorNoiseMediaSource is added to "
                    "media_sources in your YAML config");
      xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPED);
      vTaskSuspend(nullptr);  // Suspend this task indefinitely until the loop method deletes it
    }

    // Create output transfer buffer sized to store READ_WRITE_TIMEOUT_MS of audio
    size_t buffer_size = stream_info.ms_to_bytes(READ_WRITE_TIMEOUT_MS);
    std::unique_ptr<audio::AudioSinkTransferBuffer> output_buffer = audio::AudioSinkTransferBuffer::create(buffer_size);
    if (!output_buffer) {
      ESP_LOGE(TAG, "Failed to allocate output transfer buffer");
      xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPED);
      vTaskSuspend(nullptr);  // Suspend this task indefinitely until the loop method deletes it
    }

    AudioSinkAdapter audio_sink;
    audio_sink.listener = this_source->get_listener();
    audio_sink.source = this_source;
    audio_sink.stream_info = stream_info;
    output_buffer->set_sink(&audio_sink);

    // Construct the appropriate noise generator (allocated at task start, destroyed at task end)
    std::unique_ptr<NoiseGenerator> generator;
    switch (this_source->noise_type_) {
      case NoiseType::WHITE:
        generator = std::make_unique<WhiteNoiseGenerator>();
        break;
      case NoiseType::BROWN:
        generator = std::make_unique<BrownNoiseGenerator>(stream_info.get_sample_rate());
        break;
      case NoiseType::PINK:
        generator = std::make_unique<PinkNoiseGenerator>();
        break;
    }

    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_RUNNING);

    // Main generation loop
    while (true) {
      EventBits_t event_bits = xEventGroupGetBits(this_source->event_group_);

      if (event_bits & EventGroupBits::COMMAND_STOP) {
        break;
      }

      // Check if we've generated enough samples for the requested duration
      bool generation_complete = (this_source->total_samples_to_generate_ > 0) &&
                                 (this_source->samples_generated_ >= this_source->total_samples_to_generate_);

      if (generation_complete && output_buffer->available() == 0) {
        // All samples generated and buffer is empty, stop cleanly
        ESP_LOGD(TAG, "Duration complete, %zu samples generated", this_source->samples_generated_);
        break;
      }

      // Skip generation when paused but continue running
      if (!(event_bits & EventGroupBits::COMMAND_PAUSE)) {
        // Only generate more samples if we haven't reached the duration limit
        if (!generation_complete) {
          // Fill the output buffer with noise samples
          size_t bytes_to_generate = output_buffer->free();

          // Limit bytes to generate if we're close to the duration limit
          if (this_source->total_samples_to_generate_ > 0) {
            size_t samples_remaining = this_source->total_samples_to_generate_ - this_source->samples_generated_;
            size_t bytes_remaining = stream_info.samples_to_bytes(samples_remaining);
            if (bytes_to_generate > bytes_remaining) {
              bytes_to_generate = bytes_remaining;
            }
          }

          if (bytes_to_generate > 0) {
            // Generate noise samples directly into the transfer buffer
            int16_t *samples = reinterpret_cast<int16_t *>(output_buffer->get_buffer_end());
            size_t sample_count = bytes_to_generate / sizeof(int16_t);

            generator->generate_samples(samples, sample_count, this_source->amplitude_q15_);
            output_buffer->increase_buffer_length(bytes_to_generate);

            // Track the number of samples generated
            this_source->samples_generated_ += stream_info.bytes_to_samples(bytes_to_generate);
          }
        }

        // Transfer data from buffer to sink (never shift to avoid unnecessary data moves)
        output_buffer->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS), false);
      } else {
        // Paused - sleep to avoid busy waiting
        vTaskDelay(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS));
      }
    }
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPING);
  }
  xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STOPPED);
  vTaskSuspend(nullptr);  // Suspend this task indefinitely until the loop method deletes it
}

}  // namespace color_noise
}  // namespace esphome

#endif  // USE_ESP32
