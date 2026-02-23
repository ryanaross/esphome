#include "http_media_source.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio_decoder.h"
#include "esphome/components/audio/audio_transfer_buffer.h"
#include "esphome/core/log.h"

namespace esphome {
namespace http_request {

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

static const uint32_t READ_TASK_STACK_SIZE = 5 * 1024;
static const uint32_t DECODE_TASK_STACK_SIZE = 3 * 1024;
static const size_t DEFAULT_TRANSFER_BUFFER_SIZE = 24 * 1024;

static const uint32_t READ_WRITE_TIMEOUT_MS = 20;
static const uint32_t CONNECTION_TIMEOUT_MS = 30000;  // 30 second timeout for no data
static const uint8_t MAX_CONNECTION_ATTEMPTS = 6;

static const char *const TAG = "http_media_source";

enum class SourceControls : uint8_t {
  START = 0,
  STOP = 1,
  PAUSE = 2,
  RESUME = 3,
};

struct ControlMessage {
  SourceControls control;
  std::string *uri{nullptr};  // Owned pointer, must delete after use
};

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),
  COMMAND_PAUSE = (1 << 1),
  READER_READY = (1 << 2),
  READER_FINISHED = (1 << 3),
  READER_ERROR = (1 << 4),
  DECODER_FINISHED = (1 << 5),
  DECODER_ERROR = (1 << 6),
  TASK_STARTING = (1 << 7),
  TASK_RUNNING = (1 << 8),
  DECODER_RINGBUF_ACQUIRED = (1 << 9),  // Decode task has acquired shared_ptr to ring buffer
  ALL_BITS = 0x00FFFFFF,                // All valid FreeRTOS event group bits
};

/// @brief Detect audio file type from Content-Type header or URL extension
/// @param content_type The Content-Type header value (can be empty)
/// @param url The URL to fallback to for extension detection
/// @return The detected AudioFileType, or NONE if unknown
static audio::AudioFileType detect_audio_type(const std::string &content_type, const std::string &url) {
  // Try to determine file type from Content-Type header first
  if (!content_type.empty()) {
    std::string ct_lower = str_lower_case(content_type);

#ifdef USE_AUDIO_MP3_SUPPORT
    if (ct_lower.find("audio/mp3") != std::string::npos || ct_lower.find("audio/mpeg") != std::string::npos) {
      return audio::AudioFileType::MP3;
    }
#endif
    if (ct_lower.find("audio/wav") != std::string::npos) {
      return audio::AudioFileType::WAV;
    }
#ifdef USE_AUDIO_FLAC_SUPPORT
    if (ct_lower.find("audio/flac") != std::string::npos || ct_lower.find("audio/x-flac") != std::string::npos) {
      return audio::AudioFileType::FLAC;
    }
#endif
#ifdef USE_AUDIO_OPUS_SUPPORT
    if (ct_lower.find("audio/opus") != std::string::npos ||
        (ct_lower.find("audio/ogg") != std::string::npos && ct_lower.find("opus") != std::string::npos)) {
      return audio::AudioFileType::OPUS;
    }
#endif
  }

  // Fallback to URL extension
  std::string url_lower = str_lower_case(url);

  if (str_endswith(url_lower, ".wav")) {
    return audio::AudioFileType::WAV;
  }
#ifdef USE_AUDIO_MP3_SUPPORT
  if (str_endswith(url_lower, ".mp3")) {
    return audio::AudioFileType::MP3;
  }
#endif
#ifdef USE_AUDIO_FLAC_SUPPORT
  if (str_endswith(url_lower, ".flac")) {
    return audio::AudioFileType::FLAC;
  }
#endif
#ifdef USE_AUDIO_OPUS_SUPPORT
  if (str_endswith(url_lower, ".opus") || str_endswith(url_lower, ".ogg")) {
    return audio::AudioFileType::OPUS;
  }
#endif

  return audio::AudioFileType::NONE;
}

bool HTTPMediaSource::play_uri(const std::string &uri) {
  // Check if pipeline is already playing
  if (this->get_state() != media_source::MediaSourceState::IDLE) {
    ESP_LOGE(TAG, "Cannot play '%s': pipeline is busy", uri.c_str());
    return false;
  }

  // Validate URI starts with "http://" or "https://"
  if (!uri.starts_with("http://") && !uri.starts_with("https://")) {
    ESP_LOGE(TAG, "Invalid URI: '%s'", uri.c_str());
    return false;
  }

  if (!this->is_ready() || this->is_failed()) {
    return false;
  }

  // Queue playback start
  ControlMessage message = {.control = SourceControls::START, .uri = new std::string(uri)};
  if (xQueueSend(this->controls_queue_, &message, 0) != pdTRUE) {
    delete message.uri;
    ESP_LOGE(TAG, "Failed to queue play command");
    return false;
  }
  this->enable_loop_soon_any_context();
  return true;
}

void HTTPMediaSource::setup() {
  this->disable_loop();

  // Create event group and queue upfront so they're available when play_uri is called
  this->event_group_ = xEventGroupCreate();
  this->controls_queue_ = xQueueCreate(3, sizeof(ControlMessage));
}

void HTTPMediaSource::loop() {
  // Process control messages
  ControlMessage incoming_control;
  if (xQueueReceive(this->controls_queue_, &incoming_control, 0)) {
    switch (incoming_control.control) {
      case SourceControls::START:
        if (incoming_control.uri != nullptr) {
          this->current_uri_ = *incoming_control.uri;
          delete incoming_control.uri;
        }
        this->decoding_state_ = HTTPDecodingState::START_TASKS;
        break;
      case SourceControls::STOP:
        if (this->decoding_state_ == HTTPDecodingState::DECODING) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_STOP);
        }
        break;
      case SourceControls::PAUSE:
        if ((this->decoding_state_ == HTTPDecodingState::DECODING) &&
            (this->get_state() == media_source::MediaSourceState::PLAYING)) {
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PAUSED);
        }
        break;
      case SourceControls::RESUME:
        if ((this->decoding_state_ == HTTPDecodingState::DECODING) &&
            (this->get_state() == media_source::MediaSourceState::PAUSED)) {
          // Clear the pause command bit to resume
          xEventGroupClearBits(this->event_group_, EventGroupBits::COMMAND_PAUSE);
          this->set_state_(media_source::MediaSourceState::PLAYING);
        }
        break;
    }
  }

  // Process pipeline state machine
  switch (this->decoding_state_) {
    case HTTPDecodingState::START_TASKS: {
      // Start the read task
      if (this->read_task_handle_ == nullptr) {
        xEventGroupClearBits(this->event_group_, ALL_BITS);
        if (this->read_task_stack_buffer_ == nullptr) {
          // Reader task uses HttpContainer which uses esp_http_client. This crashes on IDF 5.4 if the task
          // stack is in PSRAM. As a workaround, always allocate the read task in internal memory.
          RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
          this->read_task_stack_buffer_ = stack_allocator.allocate(READ_TASK_STACK_SIZE);
        }
        if (this->read_task_stack_buffer_ == nullptr) {
          ESP_LOGE(TAG, "Failed to allocate read task stack");
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->decoding_state_ = HTTPDecodingState::IDLE;
          this->status_momentary_error("task_alloc", 15000);
          return;
        }

        this->read_task_handle_ = xTaskCreateStatic(read_task, "HTTPRead", READ_TASK_STACK_SIZE, this, 1,
                                                    this->read_task_stack_buffer_, &this->read_task_stack_);
        if (this->read_task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create read task");
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->decoding_state_ = HTTPDecodingState::IDLE;
          this->status_momentary_error("task_create", 15000);
          return;
        }
      }

      // Start the decode task
      if (this->decode_task_handle_ == nullptr) {
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
          // Signal read task to stop and let the DECODING state handle cleanup.
          // Set DECODER_FINISHED since no decode task exists to set it.
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_STOP | EventGroupBits::DECODER_FINISHED |
                                                     EventGroupBits::DECODER_RINGBUF_ACQUIRED);
          this->decoding_state_ = HTTPDecodingState::DECODING;
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->status_momentary_error("task_alloc", 15000);
          return;
        }

        this->decode_task_handle_ = xTaskCreateStatic(decode_task, "HTTPDecode", DECODE_TASK_STACK_SIZE, this, 1,
                                                      this->decode_task_stack_buffer_, &this->decode_task_stack_);
        if (this->decode_task_handle_ == nullptr) {
          ESP_LOGE(TAG, "Failed to create decode task");
          // Signal read task to stop and let the DECODING state handle cleanup.
          // Set DECODER_FINISHED since no decode task exists to set it.
          xEventGroupSetBits(this->event_group_, EventGroupBits::COMMAND_STOP | EventGroupBits::DECODER_FINISHED |
                                                     EventGroupBits::DECODER_RINGBUF_ACQUIRED);
          this->decoding_state_ = HTTPDecodingState::DECODING;
          this->set_state_(media_source::MediaSourceState::ERROR);
          this->status_momentary_error("task_create", 15000);
          return;
        }
      }

      ESP_LOGD(TAG, "Started read and decode tasks");
      this->decoding_state_ = HTTPDecodingState::DECODING;
      break;
    }
    case HTTPDecodingState::DECODING: {
      EventBits_t event_bits = xEventGroupGetBits(this->event_group_);

      if (event_bits & TASK_STARTING) {
        ESP_LOGD(TAG, "Pipeline starting");
        xEventGroupClearBits(this->event_group_, TASK_STARTING);
      }

      if (event_bits & TASK_RUNNING) {
        ESP_LOGD(TAG, "Pipeline running");
        xEventGroupClearBits(this->event_group_, TASK_RUNNING);
        this->set_state_(media_source::MediaSourceState::PLAYING);
      }

      if (event_bits & (READER_ERROR | DECODER_ERROR)) {
        ESP_LOGE(TAG, "Pipeline error occurred during playback");
        xEventGroupClearBits(this->event_group_, READER_ERROR | DECODER_ERROR);
        this->set_state_(media_source::MediaSourceState::ERROR);
      }

      // Check if both tasks are finished
      if ((event_bits & READER_FINISHED) && (event_bits & DECODER_FINISHED)) {
        ESP_LOGD(TAG, "Both tasks finished");

        // Delete tasks
        if (this->read_task_handle_ != nullptr) {
          vTaskDelete(this->read_task_handle_);
          this->read_task_handle_ = nullptr;
          if (this->read_task_stack_buffer_ != nullptr) {
            RAMAllocator<StackType_t> stack_allocator(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
            stack_allocator.deallocate(this->read_task_stack_buffer_, READ_TASK_STACK_SIZE);
            this->read_task_stack_buffer_ = nullptr;
          }
        }

        if (this->decode_task_handle_ != nullptr) {
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
        }

        // Clear the finished and stop bits now that both tasks are cleaned up
        xEventGroupClearBits(this->event_group_, READER_FINISHED | DECODER_FINISHED | COMMAND_STOP | COMMAND_PAUSE);

        this->set_state_(media_source::MediaSourceState::IDLE);
        this->decoding_state_ = HTTPDecodingState::IDLE;
      }
      break;
    }
    case HTTPDecodingState::IDLE: {
      // Nothing to do when idle
      break;
    }
  }

  // Check if we should disable loop when pipeline is idle
  if (this->decoding_state_ == HTTPDecodingState::IDLE) {
    this->disable_loop();
  }
}

void HTTPMediaSource::handle_command(media_source::MediaSourceCommand command) {
  if (this->controls_queue_ == nullptr) {
    return;
  }

  ControlMessage message;
  switch (command) {
    case media_source::MediaSourceCommand::MEDIA_SOURCE_COMMAND_END:
      // Intentional fallthrough
    case media_source::MediaSourceCommand::MEDIA_SOURCE_COMMAND_STOP: {
      if (this->decoding_state_ == HTTPDecodingState::DECODING) {
        message.control = SourceControls::STOP;
        xQueueSend(this->controls_queue_, &message, 0);
      }
      break;
    }
    case media_source::MediaSourceCommand::MEDIA_SOURCE_COMMAND_PAUSE: {
      message.control = SourceControls::PAUSE;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    case media_source::MediaSourceCommand::MEDIA_SOURCE_COMMAND_PLAY: {
      message.control = SourceControls::RESUME;
      xQueueSend(this->controls_queue_, &message, 0);
      break;
    }
    default:
      break;
  }
}

media_source::MediaSourceCapabilities HTTPMediaSource::get_capabilities() {
  media_source::MediaSourceCapabilities caps;
  caps.supports_pause = true;  // HTTP streaming can be paused
  return caps;
}

void HTTPMediaSource::read_task(void *params) {
  HTTPMediaSource *this_source = static_cast<HTTPMediaSource *>(params);

  // Holds ring buffer alive until the decode task acquires its own shared_ptr reference.
  // Declared outside the inner scope so it survives past transfer_buffer cleanup.
  std::shared_ptr<RingBuffer> ring_buffer_guard;

  {  // Ensure all C++ objects fall out of scope and deallocate
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_STARTING);

    // Get the parent HttpRequestComponent to make HTTP requests
    HttpRequestComponent *http_client = this_source->get_parent();

    // Request Content-Type header for file type detection
    std::vector<std::string> collect_headers = {"content-type"};

    // Start HTTP request, retrying on transient failures (e.g., EAGAIN during header fetch)
    std::shared_ptr<HttpContainer> container;
    for (uint8_t attempt = 0; attempt < MAX_CONNECTION_ATTEMPTS; ++attempt) {
      if (xEventGroupGetBits(this_source->event_group_) & EventGroupBits::COMMAND_STOP) {
        break;
      }

      container = http_client->get(this_source->current_uri_, {}, collect_headers);

      if (container != nullptr && is_success(container->status_code)) {
        break;  // Success
      }

      // Clean up failed attempt
      if (container != nullptr) {
        ESP_LOGW(TAG, "HTTP request attempt %u failed with status %d", attempt + 1, container->status_code);
        container->end();
        container.reset();
      } else {
        ESP_LOGW(TAG, "HTTP request attempt %u failed to connect", attempt + 1);
      }

      if (attempt + 1 < MAX_CONNECTION_ATTEMPTS) {
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait before retry
      }
    }

    if (container == nullptr || !is_success(container->status_code)) {
      ESP_LOGE(TAG, "HTTP request failed after %u attempts", MAX_CONNECTION_ATTEMPTS);
      if (container != nullptr) {
        container->end();
      }
      xEventGroupSetBits(this_source->event_group_,
                         EventGroupBits::READER_ERROR | EventGroupBits::READER_FINISHED | EventGroupBits::COMMAND_STOP);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // Detect audio file type from Content-Type header or URL
    std::string content_type = container->get_response_header("content-type");
    this_source->current_audio_file_type_ = detect_audio_type(content_type, this_source->current_uri_);

    if (this_source->current_audio_file_type_ == audio::AudioFileType::NONE) {
      ESP_LOGE(TAG, "Unable to determine audio file type");
      container->end();
      xEventGroupSetBits(this_source->event_group_,
                         EventGroupBits::READER_ERROR | EventGroupBits::READER_FINISHED | EventGroupBits::COMMAND_STOP);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    // Create transfer buffer for efficient writes to ring buffer
    size_t transfer_buffer_size = std::min(this_source->buffer_size_ / 4, DEFAULT_TRANSFER_BUFFER_SIZE);
    std::unique_ptr<audio::AudioSinkTransferBuffer> transfer_buffer =
        audio::AudioSinkTransferBuffer::create(transfer_buffer_size);

    if (transfer_buffer == nullptr) {
      ESP_LOGE(TAG, "Failed to create transfer buffer");
      container->end();
      xEventGroupSetBits(this_source->event_group_,
                         EventGroupBits::READER_ERROR | EventGroupBits::READER_FINISHED | EventGroupBits::COMMAND_STOP);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    {  // Ensures temp_ring_buffer falls out of scope and deallocates
      std::shared_ptr<RingBuffer> temp_ring_buffer;
      if (this_source->raw_file_ring_buffer_.expired()) {
        temp_ring_buffer = RingBuffer::create(this_source->buffer_size_);
        this_source->raw_file_ring_buffer_ = temp_ring_buffer;
      }

      if (this_source->raw_file_ring_buffer_.expired()) {
        ESP_LOGE(TAG, "Failed to create ring buffer");
        container->end();
        xEventGroupSetBits(this_source->event_group_, EventGroupBits::READER_ERROR | EventGroupBits::READER_FINISHED |
                                                          EventGroupBits::COMMAND_STOP);
        while (true) {
          vTaskDelay(pdMS_TO_TICKS(1000));
        }
      }

      transfer_buffer->set_sink(this_source->raw_file_ring_buffer_);
      ring_buffer_guard = temp_ring_buffer;
    }

    // Signal that reader is ready
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::READER_READY);

    uint32_t last_data_time = millis();

    // Main read loop
    // We don't use http_read_loop_result() here because:
    // - We're on a dedicated FreeRTOS task, not the main loop
    // - esp_http_client_read() blocks, so tight-spinning isn't a concern
    // - Transient errors (e.g., -ESP_ERR_HTTP_EAGAIN) should retry, not fail immediately
    while (true) {
      EventBits_t event_bits = xEventGroupGetBits(this_source->event_group_);

      if (event_bits & EventGroupBits::COMMAND_STOP) {
        break;
      }

      // Transfer any buffered data to the ring buffer
      transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS), false);

      if (transfer_buffer->free() > 0) {
        int received_len = container->read(transfer_buffer->get_buffer_end(), transfer_buffer->free());

        if (received_len > 0) {
          last_data_time = millis();
          transfer_buffer->increase_buffer_length(received_len);
        } else if (received_len == 0 && container->is_read_complete()) {
          // Flush remaining buffered data to the ring buffer, retrying until empty
          while (transfer_buffer->available() > 0) {
            if (xEventGroupGetBits(this_source->event_group_) & EventGroupBits::COMMAND_STOP) {
              break;
            }
            transfer_buffer->transfer_data_to_sink(pdMS_TO_TICKS(READ_WRITE_TIMEOUT_MS), false);
          }
          ESP_LOGD(TAG, "Reader finished");
          break;
        } else if (millis() - last_data_time >= CONNECTION_TIMEOUT_MS) {
          ESP_LOGE(TAG, "Reader timed out");
          xEventGroupSetBits(this_source->event_group_, EventGroupBits::READER_ERROR | EventGroupBits::COMMAND_STOP);
          break;
        }
        // else: no data yet or transient error (e.g., EAGAIN), loop continues
      }
    }

    // Clean up
    transfer_buffer.reset();
    container->end();
  }
  // Set READER_FINISHED bit to signal we're done
  xEventGroupSetBits(this_source->event_group_, EventGroupBits::READER_FINISHED);

  // Wait for decode task to acquire the ring buffer shared_ptr before we release ours
  xEventGroupWaitBits(this_source->event_group_,
                      EventGroupBits::DECODER_RINGBUF_ACQUIRED | EventGroupBits::COMMAND_STOP, pdFALSE, pdFALSE,
                      portMAX_DELAY);

  // Safe to release now - decode task has acquired its own shared_ptr (or exited)
  ring_buffer_guard.reset();

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void HTTPMediaSource::decode_task(void *params) {
  HTTPMediaSource *this_source = static_cast<HTTPMediaSource *>(params);

  {  // Ensure all C++ objects fall out of scope and deallocate

    // Wait until the reader notifies us that it's ready or receive a stop command
    xEventGroupWaitBits(
        this_source->event_group_,
        EventGroupBits::READER_READY | EventGroupBits::COMMAND_STOP,  // Bit message to read
        pdFALSE,                                                      // Don't clear the bit on exit
        pdFALSE,                                                      // Wait for any bit
        pdMS_TO_TICKS(
            CONNECTION_TIMEOUT_MS));  // Timeout to avoid indefinitely waiting for the reader task to get ready

    // Read bits before clearing READER_READY so we can detect timeout vs actual readiness
    EventBits_t event_bits = xEventGroupGetBits(this_source->event_group_);
    xEventGroupClearBits(this_source->event_group_, EventGroupBits::READER_READY);

    // Exit if stop was requested or if READER_READY was never set (timeout)
    if ((event_bits & EventGroupBits::COMMAND_STOP) || !(event_bits & EventGroupBits::READER_READY)) {
      // Signal reader task so it doesn't wait forever for us to acquire the ring buffer
      xEventGroupSetBits(this_source->event_group_,
                         EventGroupBits::DECODER_RINGBUF_ACQUIRED | EventGroupBits::DECODER_FINISHED);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    size_t transfer_buffer_size = std::min(this_source->buffer_size_ / 4, DEFAULT_TRANSFER_BUFFER_SIZE);
    std::unique_ptr<audio::AudioDecoder> decoder =
        make_unique<audio::AudioDecoder>(transfer_buffer_size, transfer_buffer_size);

    esp_err_t err = decoder->start(this_source->current_audio_file_type_);
    decoder->add_source(this_source->raw_file_ring_buffer_);

    // Signal reader task that we've acquired the ring buffer shared_ptr
    xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_RINGBUF_ACQUIRED);

    if (err != ESP_OK) {
      decoder.reset();
      ESP_LOGE(TAG, "Failed to start decoder: %s", esp_err_to_name(err));
      xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_ERROR | EventGroupBits::DECODER_FINISHED |
                                                        EventGroupBits::COMMAND_STOP);
      while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
      }
    }

    xEventGroupSetBits(this_source->event_group_, EventGroupBits::TASK_RUNNING);

    AudioSinkAdapter audio_sink;
    bool has_stream_info = false;

    while (true) {
      event_bits = xEventGroupGetBits(this_source->event_group_);

      if (event_bits & EventGroupBits::COMMAND_STOP) {
        break;
      }

      decoder->set_pause_output_state(event_bits & EventGroupBits::COMMAND_PAUSE);

      // Will stop gracefully once reader finished
      audio::AudioDecoderState decoder_state = decoder->decode(event_bits & EventGroupBits::READER_FINISHED);

      if (decoder_state == audio::AudioDecoderState::FINISHED) {
        ESP_LOGD(TAG, "Decoding finished");
        break;
      } else if (decoder_state == audio::AudioDecoderState::FAILED) {
        ESP_LOGE(TAG, "Decoding failed");
        xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_ERROR | EventGroupBits::COMMAND_STOP);
        break;
      }

      if (!has_stream_info && decoder->get_audio_stream_info().has_value()) {
        ESP_LOGD(TAG, "Got stream info from decoder");
        has_stream_info = true;

        audio::AudioStreamInfo stream_info = decoder->get_audio_stream_info().value();

        if (stream_info.get_bits_per_sample() != 16) {
          ESP_LOGE(TAG, "Incompatible bits per sample. Only 16 bits per sample is supported");
          xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_ERROR | EventGroupBits::COMMAND_STOP);
          break;
        } else if ((stream_info.get_channels() > 2)) {
          ESP_LOGE(TAG, "Incompatible number of channels. Only 1 or 2 channel audio is supported.");
          xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_ERROR | EventGroupBits::COMMAND_STOP);
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
              xEventGroupSetBits(this_source->event_group_,
                                 EventGroupBits::DECODER_ERROR | EventGroupBits::COMMAND_STOP);
              break;
            }
            ESP_LOGD(TAG, "Successfully added callback sink to decoder");
          } else {
            ESP_LOGE(TAG, "Listener is not set! Make sure the HTTPMediaSource is added to media_sources "
                          "in your YAML config");
            xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_ERROR | EventGroupBits::COMMAND_STOP);
            break;
          }
        }
      }
    }

    decoder.reset();
  }
  // Set DECODER_FINISHED bit to signal we're done
  xEventGroupSetBits(this_source->event_group_, EventGroupBits::DECODER_FINISHED);

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace http_request
}  // namespace esphome

#endif  // USE_ESP32
