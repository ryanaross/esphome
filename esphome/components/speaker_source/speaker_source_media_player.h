#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/audio/audio.h"

#include "esphome/components/media_source/media_source.h"
#include "esphome/components/media_player/media_player.h"
#include "esphome/components/speaker/speaker.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <array>
#include <atomic>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace speaker_source {

enum Pipeline : uint8_t {
  MEDIA_PIPELINE = 0,
  ANNOUNCEMENT_PIPELINE = 1,
};

enum RepeatMode : uint8_t {
  REPEAT_OFF = 0,
  REPEAT_ONE = 1,
  REPEAT_ALL = 2,
};

struct PipelineState {
  /// @brief Timeout IDs for playlist delay, indexed by Pipeline enum
  static constexpr const char *const TIMEOUT_IDS[] = {"next_media", "next_ann"};

  speaker::Speaker *speaker{nullptr};
  optional<media_player::MediaPlayerSupportedFormat> format;

  media_source::MediaSource *active_source{nullptr};
  media_source::MediaSource *last_source{nullptr};
  media_source::MediaSource *stopping_source{nullptr};  // Source we've asked to stop, awaiting IDLE
  media_source::MediaSource *pending_source{nullptr};   // Source we've asked to play, awaiting PLAYING

  std::vector<media_source::MediaSource *> media_sources;

  std::vector<std::string> playlist;
  size_t playlist_index{0};
  RepeatMode repeat_mode{REPEAT_OFF};
  uint32_t playlist_delay_ms{0};

  // When non-empty, playlist_index indexes into these vectors
  // which contain the actual playlist indices in shuffled order
  std::vector<size_t> shuffle_indices;

  // Track frames sent to speaker to correlate with playback callbacks.
  // Atomic because it is written from the main loop/source tasks and read/decremented from the speaker playback
  // callback.
  std::atomic<uint32_t> pending_frames{0};

  /// @brief Check if this pipeline is configured (has a speaker assigned)
  bool is_configured() const { return this->speaker != nullptr; }
};

struct MediaPlayerControlCommand {
  enum Type : uint8_t {
    PLAY_URI,          // Clear playlist, reset index, add URI, queue PLAY_CURRENT
    ENQUEUE_URI,       // Add URI to playlist, queue PLAY_CURRENT if idle
    PLAYLIST_ADVANCE,  // Advance index (or wrap for repeat_all), queue PLAY_CURRENT if more items
    PLAY_CURRENT,      // Play item at current playlist index (can retry if speaker not ready)
    SEND_COMMAND,      // Send command to active source
  };
  Type type;
  uint8_t pipeline;  // MEDIA_PIPELINE or ANNOUNCEMENT_PIPELINE

  union {
    std::string *uri;  // Owned pointer, must delete after xQueueReceive (for PLAY_URI and ENQUEUE_URI)
    media_player::MediaPlayerCommand command;
  } data;
};

struct VolumeRestoreState {
  float volume;
  bool is_muted;
};

class SpeakerSourceMediaPlayer : public Component,
                                 public media_player::MediaPlayer,
                                 public media_source::MediaSourceListener {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::PROCESSOR; }
  void setup() override;
  void loop() override;
  // TODO: implement dump_config() to log pipeline configuration, volume settings, and registered sources

  // MediaPlayer implementations
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->is_muted_; }

  // MediaSourceListener implementations (no pipeline params - uses find_pipeline_for_source_)
  size_t on_media_output(media_source::MediaSource *source, uint8_t *data, size_t length, TickType_t ticks,
                         const audio::AudioStreamInfo &stream_info) override;
  void on_media_state_changed(media_source::MediaSource *source, media_source::MediaSourceState state) override;
  void on_volume_request(media_source::MediaSource *source, float volume) override;
  void on_mute_request(media_source::MediaSource *source, bool is_muted) override;
  void on_play_uri_request(media_source::MediaSource *source, const std::string &uri) override;

  void set_task_stack_in_psram(bool task_stack_in_psram) { this->task_stack_in_psram_ = task_stack_in_psram; }

  // Percentage to increase or decrease the volume for volume up or volume down commands
  void set_volume_increment(float volume_increment) { this->volume_increment_ = volume_increment; }

  // Volume used initially on first boot when no volume had been previously saved
  void set_volume_initial(float volume_initial) { this->volume_initial_ = volume_initial; }

  void set_volume_max(float volume_max) { this->volume_max_ = volume_max; }
  void set_volume_min(float volume_min) { this->volume_min_ = volume_min; }

  void add_media_source(uint8_t pipeline, media_source::MediaSource *media_source) {
    this->pipelines_[pipeline].media_sources.push_back(media_source);
  }

  void set_speaker(uint8_t pipeline, speaker::Speaker *speaker) { this->pipelines_[pipeline].speaker = speaker; }
  void set_format(uint8_t pipeline, const media_player::MediaPlayerSupportedFormat &format) {
    this->pipelines_[pipeline].format = format;
  }

  Trigger<> *get_mute_trigger() { return &this->mute_trigger_; }
  Trigger<> *get_unmute_trigger() { return &this->unmute_trigger_; }
  Trigger<float> *get_volume_trigger() { return &this->volume_trigger_; }

  void set_playlist_delay_ms(uint8_t pipeline, uint32_t delay_ms);

 protected:
  /// @brief Find which pipeline a source belongs to by checking active, pending, and stopping sources
  /// @param source The source to find
  /// @return The pipeline index, or MEDIA_PIPELINE as fallback
  uint8_t find_pipeline_for_source_(media_source::MediaSource *source) const;

  void handle_speaker_playback_callback_(uint32_t frames, int64_t timestamp, uint8_t pipeline);

  // Receives commands from HA or from the voice assistant component
  // Sends commands to the media_control_command_queue_
  void control(const media_player::MediaPlayerCall &call) override;

  /// @brief Updates this->volume and saves volume/mute state to flash for restoration if publish is true.
  void set_volume_(float volume, bool publish = true);

  /// @brief Sets the mute state. Always saves volume/mute state to flash for restoration.
  /// @param mute_state If true, audio will be muted. If false, audio will be unmuted
  void set_mute_state_(bool mute_state);

  /// @brief Saves the current volume and mute state to the flash for restoration.
  void save_volume_restore_state_();

  void process_control_queue_();
  bool try_execute_play_uri_(const std::string &uri, uint8_t pipeline);
  media_source::MediaSource *find_source_for_uri_(const std::string &uri, uint8_t pipeline);
  void queue_command_(MediaPlayerControlCommand::Type type, uint8_t pipeline);
  void queue_play_current_(uint8_t pipeline, uint32_t delay_ms = 0);

  /// @brief Maps playlist_index through shuffle indices if shuffle is active
  size_t get_playlist_position_(uint8_t pipeline) const;

  /// @brief Generates shuffled indices for the playlist, keeping current track at current position
  void shuffle_playlist_(uint8_t pipeline);

  /// @brief Clears shuffle indices and adjusts playlist_index to maintain current track
  void unshuffle_playlist_(uint8_t pipeline);

  QueueHandle_t media_control_command_queue_;

  // Pipeline state for media (index 0) and announcement (index 1) pipelines
  // Note: No mutex needed - pipelines are only accessed from the main loop thread
  std::array<PipelineState, 2> pipelines_;

  bool task_stack_in_psram_;

  bool is_paused_{false};
  bool is_muted_{false};

  // The amount to change the volume on volume up/down commands
  float volume_increment_;

  // The initial volume used by Setup when no previous volume was saved
  float volume_initial_;

  float volume_max_;
  float volume_min_;

  // Used to save volume/mute state for restoration on reboot
  ESPPreferenceObject pref_;

  Trigger<> mute_trigger_;
  Trigger<> unmute_trigger_;
  Trigger<float> volume_trigger_;
};

}  // namespace speaker_source
}  // namespace esphome

#endif  // USE_ESP32
