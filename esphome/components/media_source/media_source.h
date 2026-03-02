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

enum class MediaSourceState : uint8_t {
  IDLE,     // Not playing, ready to accept play_uri
  PLAYING,  // Currently playing media
  PAUSED,   // Playback paused, can be resumed
  ERROR,    // Error occurred during playback
};

/// @brief Commands that can be sent from the orchestrator to a media source
enum class MediaSourceCommand : uint8_t {
  // All sources should support these basic commands
  PLAY,
  PAUSE,
  STOP,

  // Only sources with internal playlists will handle these; simple sources should ignore them.
  NEXT,
  PREVIOUS,
  CLEAR_PLAYLIST,
  REPEAT_ALL,
  REPEAT_ONE,
  REPEAT_OFF,
  SHUFFLE,
  UNSHUFFLE,

  // Command to join a group for synchronized playback; simple sources should ignore this.
  GROUP_JOIN,
};

// Forward declaration
class MediaSource;

/// @brief Interface for receiving callbacks from a MediaSource.
/// The MediaSource pointer is passed as the first argument so the listener/orchestrator can identify
/// which source is calling.
class MediaSourceListener {
 public:
  // Used to send audio to the listener
  virtual size_t on_media_output(MediaSource *source, uint8_t *data, size_t length, TickType_t ticks_to_wait,
                                 const audio::AudioStreamInfo &stream_info) = 0;
  // Used to notify listener of state changes
  virtual void on_media_state_changed(MediaSource *source, MediaSourceState state) = 0;
  // Callbacks from smart sources requesting the orchestrator to change volume, mute, or start a new URI.
  // Simple sources never invoke these.
  virtual void on_volume_request(MediaSource *source, float volume) = 0;
  virtual void on_mute_request(MediaSource *source, bool is_muted) = 0;
  virtual void on_play_uri_request(MediaSource *source, const std::string &uri) = 0;
};

/// @brief Abstract base class for media sources
/// MediaSource provides audio data to an orchestrator via the MediaSourceListener interface. It also receives commands
/// from the orchestrator to control playback.
class MediaSource {
 public:
  virtual ~MediaSource() = default;

  // === Playback Control ===

  /// @brief Start playing the given URI
  /// Sources should validate the URI and state, returning false if the source is busy.
  /// The orchestrator is responsible for stopping active sources before starting a new one.
  /// @param uri The URI to play; e.g., "http://stream_url"
  /// @return true if playback started successfully, false otherwise
  virtual bool play_uri(const std::string &uri) = 0;

  /// @brief Handle playback commands (pause, stop, next, etc.)
  /// @param command The command to execute
  virtual void handle_command(MediaSourceCommand command) = 0;

  /// @brief Whether this source manages its own playlist internally
  /// Override to return true for smart sources that handle next/previous/repeat/shuffle themselves.
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

  // === Listener: Source → Orchestrator ===

  /// @brief Set the listener that receives callbacks from this source
  /// @param listener Pointer to the MediaSourceListener implementation. Caller must ensure it outlives this source.
  void set_listener(MediaSourceListener *listener) { this->listener_ = listener; }

  /// @brief Get the current listener
  MediaSourceListener *get_listener() const { return this->listener_; }

  // === Callbacks: Orchestrator → Source ===

  /// @brief Orchestrator interface to notify the source that volume changed
  /// Most sources can ignore this. Override for smart sources that track volume state.
  /// @param volume New volume level (0.0 to 1.0)
  virtual void notify_volume_changed(float volume) {}

  /// @brief Orchestrator interface to notify the source that mute state changed
  /// Most sources can ignore this. Override for smart sources that track mute state.
  /// @param is_muted New mute state
  virtual void notify_mute_changed(bool is_muted) {}

  /// @brief Orchestrator interface to notify the source about audio that has been played
  /// Called when the speaker reports that audio frames have been written to the DAC.
  /// Sources can override this to track playback progress for synchronization.
  /// @param frames Number of audio frames that were played
  /// @param timestamp System time in microseconds when the frames were written to the DAC
  virtual void notify_audio_played(uint32_t frames, int64_t timestamp) {}

 protected:
  /// @brief Update state and notify listener
  /// This is the only way to change state_, ensuring listener notifications always fire.
  /// @param state New state to set
  void set_state_(MediaSourceState state) {
    if (this->state_ != state) {
      this->state_ = state;
      if (this->listener_ != nullptr) {
        this->listener_->on_media_state_changed(this, state);
      }
    }
  }

 private:
  // Private to enforce the invariant that listener notifications always fire on state changes.
  // All state transitions must go through set_state_() which couples the update with notification.
  MediaSourceState state_{MediaSourceState::IDLE};
  MediaSourceListener *listener_{nullptr};
};

}  // namespace media_source
}  // namespace esphome
