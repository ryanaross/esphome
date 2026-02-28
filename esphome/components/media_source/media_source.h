#pragma once

#include "esphome/components/audio/audio.h"
#include "esphome/core/helpers.h"

#include <cstdint>
#include <string>

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#else
using TickType_t = uint32_t;
#endif

namespace esphome {
namespace media_source {

/// @brief Represents the current state of a media source
enum class MediaSourceState : uint8_t {
  IDLE = 0,     // Not playing, ready to accept play_uri
  PLAYING = 1,  // Currently playing media
  PAUSED = 2,   // Playback paused, can be resumed
  ERROR = 4,    // Error occurred during playback
};

/// @brief Commands that can be sent to a media source
enum class MediaSourceCommand : uint8_t {
  MEDIA_SOURCE_COMMAND_PLAY,
  MEDIA_SOURCE_COMMAND_PAUSE,
  MEDIA_SOURCE_COMMAND_TOGGLE,  // Toggle play/pause (media player converts to PLAY or PAUSE)
  MEDIA_SOURCE_COMMAND_STOP,    // Indicates source should end and, if smart, forward the command to the group
  MEDIA_SOURCE_COMMAND_NEXT,
  MEDIA_SOURCE_COMMAND_PREVIOUS,
  MEDIA_SOURCE_COMMAND_CLEAR_PLAYLIST,  // Clear internal playlist
  MEDIA_SOURCE_COMMAND_REPEAT_ALL,      // Enable repeat-all mode
  MEDIA_SOURCE_COMMAND_REPEAT_ONE,      // Enable repeat-one mode
  MEDIA_SOURCE_COMMAND_REPEAT_OFF,      // Disable repeat mode
  MEDIA_SOURCE_COMMAND_SHUFFLE,         // Shuffle playlist
  MEDIA_SOURCE_COMMAND_UNSHUFFLE,       // Unshuffle playlist
  MEDIA_SOURCE_COMMAND_GROUP_JOIN,      // Join another group
};

// Forward declaration
class MediaSource;

/// @brief Interface for receiving callbacks from a MediaSource.
/// Replaces std::function callbacks with a single listener pointer to minimize overhead.
/// The MediaSource pointer is passed as the first argument so the listener can identify
/// which source is calling.
class MediaSourceListener {
 public:
  virtual size_t on_media_output(MediaSource *source, uint8_t *data, size_t length, TickType_t ticks_to_wait,
                                 audio::AudioStreamInfo stream_info) = 0;
  virtual void on_media_state_changed(MediaSource *source, MediaSourceState state) = 0;
  virtual void on_volume_request(MediaSource *source, float volume) = 0;
  virtual void on_mute_request(MediaSource *source, bool is_muted) = 0;
  virtual void on_play_uri_request(MediaSource *source, const std::string &uri) = 0;
};

/// @brief Abstract base class for media sources
/// MediaSource provides audio data to a MediaPlayer. Sources are "dumb" - they don't
/// automatically stop themselves or switch tracks. The MediaPlayer is responsible for
/// orchestrating multiple sources.
class MediaSource {
 public:
  virtual ~MediaSource() = default;

  // === Playback Control ===

  /// @brief Start playing the given URI
  /// Sources should validate the URI and state, returning false if the source is busy.
  /// The MediaPlayer is responsible for stopping active sources before starting a new one.
  /// @param uri The URI to play (e.g., "file://my_audio", "http://stream_url")
  /// @return true if playback started successfully, false otherwise
  virtual bool play_uri(const std::string &uri) = 0;

  /// @brief Handle playback commands (pause, stop, next, etc.)
  /// @param command The command to execute
  virtual void handle_command(MediaSourceCommand command) = 0;

  /// @brief Whether this source manages its own playlist internally
  /// Override to return true for smart sources (e.g., Sendspin) that handle
  /// next/previous/repeat/shuffle themselves.
  virtual bool has_internal_playlist() const { return false; }

  // === State Access ===

  /// @brief Get current playback state
  /// @return Current state of this source
  MediaSourceState get_state() const { return this->state_; }

  // === URI Matching ===

  /// @brief Check if this source can handle the given URI
  /// Each source must override this to match its supported URI scheme(s).
  /// @param uri The URI to check
  /// @return true if this source can handle the URI
  virtual bool can_handle(const std::string &uri) const = 0;

  // === Listener: Source → Player ===

  /// @brief Set the listener that receives callbacks from this source
  /// @param listener Pointer to the MediaSourceListener implementation. Caller must ensure it outlives this source.
  void set_listener(MediaSourceListener *listener) { this->listener_ = listener; }

  /// @brief Get the current listener
  MediaSourceListener *get_listener() const { return this->listener_; }

  // === Callbacks: Player → Source ===

  /// @brief Notify source that volume changed
  /// Called when volume changes from Home Assistant or another source
  /// Most sources can ignore this. Override for smart sources like Sendspin.
  /// @param volume New volume level (0.0 to 1.0)
  virtual void notify_volume_changed(float volume) {}

  /// @brief Notify source that mute state changed
  /// Most sources can ignore this. Override for smart sources like Sendspin.
  /// @param is_muted New mute state
  virtual void notify_mute_changed(bool is_muted) {}

  /// @brief Notify source about audio that has been played
  /// Called when the speaker reports that audio frames have been written to the DAC.
  /// Sources can override this to track playback progress for synchronization or logging.
  /// @param frames Number of audio frames that were played
  /// @param timestamp System time in microseconds when the frames were written to the DAC
  virtual void notify_audio_played(uint32_t frames, int64_t timestamp) {}

 protected:
  /// @brief Helper to update state and notify listener
  /// Sources should use this instead of directly modifying state_
  /// @param state New state to set
  void set_state_(MediaSourceState state) {
    if (this->state_ != state) {
      this->state_ = state;
      if (this->listener_ != nullptr) {
        this->listener_->on_media_state_changed(this, state);
      }
    }
  }

  MediaSourceState state_{MediaSourceState::IDLE};
  MediaSourceListener *listener_{nullptr};
};

}  // namespace media_source
}  // namespace esphome
