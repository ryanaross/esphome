#include "speaker_source_media_player.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esphome/components/audio/audio.h"

#include <algorithm>

namespace esphome {
namespace speaker_source {

static const uint32_t MEDIA_CONTROLS_QUEUE_LENGTH = 20;

static const char *const TAG = "speaker_source_media_player";

void SpeakerSourceMediaPlayer::setup() {
  this->state = media_player::MEDIA_PLAYER_STATE_IDLE;

  this->media_control_command_queue_ = xQueueCreate(MEDIA_CONTROLS_QUEUE_LENGTH, sizeof(MediaPlayerControlCommand));

  this->pref_ = this->make_entity_preference<VolumeRestoreState>();

  VolumeRestoreState volume_restore_state;
  if (this->pref_.load(&volume_restore_state)) {
    this->set_volume_(volume_restore_state.volume);
    this->set_mute_state_(volume_restore_state.is_muted);
  } else {
    this->set_volume_(this->volume_initial_);
    this->set_mute_state_(false);
  }

  // Register this player as listener for all sources
  for (auto &ps : this->pipelines_) {
    for (auto *media_source : ps.media_sources) {
      media_source->set_listener(this);
    }
  }

  // Determine pipeline count for logging and speaker callback registration
  size_t pipeline_count = 1;
  if (this->pipelines_[ANNOUNCEMENT_PIPELINE].is_configured()) {
    pipeline_count = 2;
  }

  // Register callbacks to receive playback notifications from speakers
  for (size_t i = 0; i < pipeline_count; i++) {
    if (this->pipelines_[i].is_configured()) {
      this->pipelines_[i].speaker->add_audio_output_callback([this, i](uint32_t frames, int64_t timestamp) {
        this->handle_speaker_playback_callback_(frames, timestamp, i);
      });
    }
  }

  ESP_LOGI(TAG, "Set up speaker media player with %zu pipeline(s)", pipeline_count);
}

void SpeakerSourceMediaPlayer::set_playlist_delay_ms(uint8_t pipeline, uint32_t delay_ms) {
  if (pipeline < this->pipelines_.size()) {
    this->pipelines_[pipeline].playlist_delay_ms = delay_ms;
  }
}

uint8_t SpeakerSourceMediaPlayer::find_pipeline_for_source_(media_source::MediaSource *source) const {
  for (size_t i = 0; i < this->pipelines_.size(); i++) {
    if (this->pipelines_[i].active_source == source || this->pipelines_[i].pending_source == source ||
        this->pipelines_[i].stopping_source == source) {
      return i;
    }
  }
  ESP_LOGW(TAG, "Source %p not found in any pipeline, defaulting to media pipeline", source);
  return MEDIA_PIPELINE;  // fallback
}

void SpeakerSourceMediaPlayer::handle_speaker_playback_callback_(uint32_t frames, int64_t timestamp, uint8_t pipeline) {
  PipelineState &ps = this->pipelines_[pipeline];

  // Copy pointer to local variable to avoid TOCTOU race
  media_source::MediaSource *active_source = ps.active_source;

  // Check once - if null after this, we've avoided the race
  if (active_source == nullptr) {
    return;
  }

  // Calculate how many frames belong to this source
  uint32_t source_frames = std::min(frames, ps.pending_frames.load(std::memory_order_relaxed));
  ps.pending_frames.fetch_sub(source_frames, std::memory_order_relaxed);

  if (source_frames > 0) {
    // Notify the source about the played audio
    active_source->notify_audio_played(source_frames, timestamp);
  }
}

void SpeakerSourceMediaPlayer::on_volume_request(media_source::MediaSource *source, float volume) {
  // Update the media player's volume
  this->set_volume_(volume);
  this->publish_state();
}

void SpeakerSourceMediaPlayer::on_mute_request(media_source::MediaSource *source, bool is_muted) {
  // Update the media player's mute state
  this->set_mute_state_(is_muted);
  this->publish_state();
}

void SpeakerSourceMediaPlayer::on_play_uri_request(media_source::MediaSource *source, const std::string &uri) {
  // Smart source is requesting the player to play a different URI
  // Determine pipeline from the requesting source
  uint8_t pipeline = this->find_pipeline_for_source_(source);

  auto call = this->make_call();
  call.set_media_url(uri);
  call.set_announcement(pipeline == ANNOUNCEMENT_PIPELINE);
  call.perform();
}

void SpeakerSourceMediaPlayer::on_media_state_changed(media_source::MediaSource *source,
                                                      media_source::MediaSourceState state) {
  // Find which pipeline this source belongs to
  uint8_t pipeline = this->find_pipeline_for_source_(source);
  PipelineState &ps = this->pipelines_[pipeline];

  if (state == media_source::MediaSourceState::IDLE) {
    // Source went idle - clear stopping flag if this was the source we asked to stop
    if (ps.stopping_source == source) {
      ps.stopping_source = nullptr;
    }

    // Clear pending flag if this was the source we asked to play
    if (ps.pending_source == source) {
      ps.pending_source = nullptr;
    }

    // Source went idle - clear it if it's the active source
    if (ps.active_source == source) {
      ps.last_source = ps.active_source;
      ps.active_source = nullptr;

      // Finish the speaker to ensure it's ready for the next playback
      if (ps.is_configured()) {
        ps.speaker->finish();
      }

      // Queue PLAYLIST_ADVANCE to handle track completion - all playlist logic is in process_control_queue_
      this->queue_command_(MediaPlayerControlCommand::PLAYLIST_ADVANCE, pipeline);
    }
  } else if (state == media_source::MediaSourceState::PLAYING) {
    // Source started playing - make it the active source if no one else is active
    if (ps.active_source == nullptr) {
      ps.active_source = source;
      ps.last_source = nullptr;

      // Clear pending flag now that the source is active
      if (ps.pending_source == source) {
        ps.pending_source = nullptr;
      }
    }
  }
}

size_t SpeakerSourceMediaPlayer::on_media_output(media_source::MediaSource *source, uint8_t *data, size_t length,
                                                 TickType_t ticks, const audio::AudioStreamInfo &stream_info) {
  uint8_t pipeline = this->find_pipeline_for_source_(source);
  PipelineState &ps = this->pipelines_[pipeline];

  if (!ps.is_configured()) {
    vTaskDelay(ticks);
    return 0;
  }

  if (ps.active_source == source) {
    // This source is active - play the audio
    if (ps.speaker->get_audio_stream_info() != stream_info) {
      ps.speaker->set_audio_stream_info(stream_info);
      vTaskDelay(ticks);
      return 0;
    }
    size_t bytes_written = ps.speaker->play(data, length, ticks);
    if (bytes_written > 0) {
      // Track frames sent to speaker for this source
      ps.pending_frames.fetch_add(stream_info.bytes_to_frames(bytes_written), std::memory_order_relaxed);
    }
    return bytes_written;
  }

  // Not the active source - wait for state callback to set us as active when we transition to PLAYING
  vTaskDelay(ticks);
  return 0;
}

media_player::MediaPlayerState SpeakerSourceMediaPlayer::get_media_pipeline_state_(
    media_source::MediaSource *source, bool has_next_item, media_player::MediaPlayerState old_state) const {
  if (source != nullptr) {
    switch (source->get_state()) {
      case media_source::MediaSourceState::PLAYING:
        return media_player::MEDIA_PLAYER_STATE_PLAYING;
      case media_source::MediaSourceState::PAUSED:
        return media_player::MEDIA_PLAYER_STATE_PAUSED;
      case media_source::MediaSourceState::ERROR:
        ESP_LOGE(TAG, "Media source is in error state");
        return media_player::MEDIA_PLAYER_STATE_IDLE;
      case media_source::MediaSourceState::IDLE:
      default:
        return media_player::MEDIA_PLAYER_STATE_IDLE;
    }
  }

  // No active source — stay PLAYING during playlist transitions
  if (has_next_item && old_state == media_player::MEDIA_PLAYER_STATE_PLAYING) {
    return media_player::MEDIA_PLAYER_STATE_PLAYING;
  }
  return media_player::MEDIA_PLAYER_STATE_IDLE;
}

void SpeakerSourceMediaPlayer::loop() {
  // Process queued control commands
  this->process_control_queue_();

  // Update state based on active sources - announcement pipeline takes priority
  media_player::MediaPlayerState old_state = this->state;

  PipelineState &ann_ps = this->pipelines_[ANNOUNCEMENT_PIPELINE];
  PipelineState &media_ps = this->pipelines_[MEDIA_PIPELINE];

  // Check playlist state to detect transitions between items
  bool announcement_has_next_item = (ann_ps.playlist_index < ann_ps.playlist.size()) ||
                                    (ann_ps.repeat_mode != REPEAT_OFF && !ann_ps.playlist.empty());
  bool media_has_next_item = (media_ps.playlist_index < media_ps.playlist.size()) ||
                             (media_ps.repeat_mode != REPEAT_OFF && !media_ps.playlist.empty());

  // Check announcement pipeline first
  // Copy pointer to local variable to avoid TOCTOU race
  media_source::MediaSource *announcement_source = ann_ps.active_source;
  if (announcement_source != nullptr) {
    media_source::MediaSourceState announcement_state = announcement_source->get_state();
    if (announcement_state != media_source::MediaSourceState::IDLE) {
      // Announcement is active - announcements take priority and never report PAUSED
      switch (announcement_state) {
        case media_source::MediaSourceState::PLAYING:
        case media_source::MediaSourceState::PAUSED:  // Treat paused announcements as announcing
          this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
          break;
        case media_source::MediaSourceState::ERROR:
          this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
          ESP_LOGE(TAG, "Announcement source is in error state");
          break;
        default:
          break;
      }
    } else {
      // Announcement source is idle, fall through to media pipeline
      this->state = this->get_media_pipeline_state_(media_ps.active_source, media_has_next_item, old_state);
    }
  } else if (announcement_has_next_item && old_state == media_player::MEDIA_PLAYER_STATE_ANNOUNCING) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  } else {
    // No active announcement, check media pipeline
    this->state = this->get_media_pipeline_state_(media_ps.active_source, media_has_next_item, old_state);
  }

  if (this->state != old_state) {
    this->publish_state();
    ESP_LOGD(TAG, "State changed to %s", media_player::media_player_state_to_string(this->state));
  }
}

media_source::MediaSource *SpeakerSourceMediaPlayer::find_source_for_uri_(const std::string &uri, uint8_t pipeline) {
  PipelineState &ps = this->pipelines_[pipeline];
  for (auto *source : ps.media_sources) {
    if (source->can_handle(uri)) {
      // Check if this source is idle
      if (source->get_state() == media_source::MediaSourceState::IDLE) {
        return source;  // First idle match wins
      }
    }
  }
  // If no idle source found, try again without checking state (will be stopped by try_execute_play_uri_)
  for (auto &source : ps.media_sources) {
    if (source->can_handle(uri)) {
      return source;  // First match wins
    }
  }
  return nullptr;
}

bool SpeakerSourceMediaPlayer::try_execute_play_uri_(const std::string &uri, uint8_t pipeline) {
  // Find target source
  media_source::MediaSource *target_source = this->find_source_for_uri_(uri, pipeline);
  if (target_source == nullptr) {
    ESP_LOGW(TAG, "No source found for URI: %s", uri.c_str());
    return true;  // Remove from queue (unrecoverable)
  }

  PipelineState &ps = this->pipelines_[pipeline];

  // Get the active source for this pipeline (copy to local to avoid TOCTOU race)
  media_source::MediaSource *active_source = ps.active_source;

  // If active source exists and is not IDLE, stop it and wait
  if (active_source != nullptr) {
    media_source::MediaSourceState active_state = active_source->get_state();
    if (active_state != media_source::MediaSourceState::IDLE) {
      // Only send END command once per source - check if we've already asked this source to stop
      if (ps.stopping_source != active_source) {
        ESP_LOGD(TAG, "Pipeline %zu: Stopping active source before playing: %s", pipeline, uri.c_str());
        active_source->handle_command(media_source::MediaSourceCommand::STOP);
        if (ps.is_configured()) {
          ps.speaker->stop();
        }
        ps.stopping_source = active_source;
      }
      return false;  // Leave in queue, retry next loop
    }
  }

  // Also check target source directly - handles case where source errored before PLAYING state
  media_source::MediaSourceState target_state = target_source->get_state();
  if (target_state != media_source::MediaSourceState::IDLE) {
    // Only send STOP command once per source
    if (ps.stopping_source != target_source) {
      ESP_LOGD(TAG, "Pipeline %zu: Target source busy (state=%d), stopping before playing: %s", pipeline,
               static_cast<int>(target_state), uri.c_str());
      target_source->handle_command(media_source::MediaSourceCommand::STOP);
      if (ps.is_configured()) {
        ps.speaker->stop();
      }
      ps.stopping_source = target_source;
    }
    return false;  // Leave in queue, retry next loop
  }

  // Clear stopping flag since we're past the stopping phase
  ps.stopping_source = nullptr;

  // Check if speaker is ready
  if (!ps.is_configured() || !ps.speaker->is_stopped()) {
    return false;  // Speaker not ready yet, retry later
  }

  // Set pending source so find_pipeline_for_source_ can resolve callbacks to this pipeline
  ps.pending_source = target_source;

  // Speaker is ready, try to play
  if (!target_source->play_uri(uri)) {
    ESP_LOGE(TAG, "Pipeline %zu: Failed to play URI: %s", pipeline, uri.c_str());
    ps.pending_source = nullptr;
  }

  // Reset pending frame counter for this pipeline since we're starting a new source
  ps.pending_frames.store(0, std::memory_order_relaxed);

  return true;  // Remove from queue
}

void SpeakerSourceMediaPlayer::queue_command_(MediaPlayerControlCommand::Type type, uint8_t pipeline) {
  MediaPlayerControlCommand cmd;
  cmd.type = type;
  cmd.pipeline = pipeline;
  if (xQueueSend(this->media_control_command_queue_, &cmd, 0) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to queue command type %d", static_cast<int>(type));
  }
}

void SpeakerSourceMediaPlayer::queue_play_current_(uint8_t pipeline, uint32_t delay_ms) {
  if (delay_ms > 0) {
    this->set_timeout(PipelineState::TIMEOUT_IDS[pipeline], delay_ms,
                      [this, pipeline]() { this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline); });
  } else {
    this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
  }
}

void SpeakerSourceMediaPlayer::process_control_queue_() {
  MediaPlayerControlCommand control_command;

  // Use peek to check command without removing it
  if (xQueuePeek(this->media_control_command_queue_, &control_command, 0) != pdTRUE) {
    return;
  }

  bool command_executed = false;
  uint8_t pipeline = control_command.pipeline;

  // Get pipeline state
  PipelineState &ps = this->pipelines_[pipeline];
  media_source::MediaSource *active_source = ps.active_source;

  // Check if active source has internal playlist management
  bool has_internal_playlist = (active_source != nullptr) && active_source->has_internal_playlist();

  switch (control_command.type) {
    case MediaPlayerControlCommand::PLAY_URI: {
      // Always use our local playlist to start playback
      this->cancel_timeout(PipelineState::TIMEOUT_IDS[pipeline]);
      ps.playlist.clear();
      ps.shuffle_indices.clear();  // Clear shuffle when starting fresh playlist
      ps.playlist_index = 0;       // Reset index
      ps.playlist.push_back(*control_command.data.uri);

      // Queue PLAY_CURRENT to initiate playback
      this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
      command_executed = true;
      break;
    }

    case MediaPlayerControlCommand::ENQUEUE_URI: {
      // Always add to our local playlist
      ps.playlist.push_back(*control_command.data.uri);

      // If shuffle is active, add the new item to the end of the shuffle order
      if (!ps.shuffle_indices.empty()) {
        ps.shuffle_indices.push_back(ps.playlist.size() - 1);
      }

      // If nothing is playing and no upcoming items are queued, start the new item.
      bool nothing_playing =
          (active_source == nullptr) || (active_source->get_state() == media_source::MediaSourceState::IDLE);
      if (nothing_playing && ps.playlist_index >= ps.playlist.size() - 1) {
        ps.playlist_index = ps.playlist.size() - 1;  // Point to newly added item
        this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
      }
      command_executed = true;
      break;
    }

    case MediaPlayerControlCommand::PLAYLIST_ADVANCE: {
      // Internal message: a track finished, advance to next
      if (ps.repeat_mode != REPEAT_ONE) {
        ps.playlist_index++;
      }

      // Check if we should continue playback
      if (ps.playlist_index < ps.playlist.size()) {
        this->queue_play_current_(pipeline, ps.playlist_delay_ms);
      } else if (ps.repeat_mode == REPEAT_ALL && !ps.playlist.empty()) {
        ps.playlist_index = 0;
        this->queue_play_current_(pipeline, ps.playlist_delay_ms);
      }
      command_executed = true;
      break;
    }

    case MediaPlayerControlCommand::PLAY_CURRENT: {
      // Play the item at current playlist index (mapped through shuffle if active)
      if (ps.playlist_index < ps.playlist.size()) {
        size_t actual_position = this->get_playlist_position_(pipeline);
        command_executed = this->try_execute_play_uri_(ps.playlist[actual_position], pipeline);
      } else {
        command_executed = true;  // Index out of bounds or empty playlist
      }
      break;
    }

    case MediaPlayerControlCommand::SEND_COMMAND: {
      media_player::MediaPlayerCommand player_command = control_command.data.command;

      // Determine target source: prefer active, fall back to last
      media_source::MediaSource *target_source = nullptr;
      if (ps.active_source != nullptr) {
        target_source = ps.active_source;
      } else if (ps.last_source != nullptr) {
        target_source = ps.last_source;
      }

      switch (player_command) {
        case media_player::MEDIA_PLAYER_COMMAND_TOGGLE: {
          // Convert TOGGLE to PLAY or PAUSE based on current state
          if ((active_source != nullptr) && (active_source->get_state() == media_source::MediaSourceState::PLAYING)) {
            if (target_source != nullptr) {
              target_source->handle_command(media_source::MediaSourceCommand::PAUSE);
            }
          } else if (!has_internal_playlist && active_source == nullptr && !ps.playlist.empty()) {
            bool last_has_internal_playlist = (ps.last_source != nullptr) && ps.last_source->has_internal_playlist();
            if (last_has_internal_playlist) {
              ps.last_source->handle_command(media_source::MediaSourceCommand::PLAY);
            } else {
              if (ps.playlist_index >= ps.playlist.size()) {
                ps.playlist_index = 0;
              }
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            }
          } else {
            if (target_source != nullptr) {
              target_source->handle_command(media_source::MediaSourceCommand::PLAY);
            }
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_PLAY: {
          if (!has_internal_playlist && active_source == nullptr && !ps.playlist.empty()) {
            bool last_has_internal_playlist = (ps.last_source != nullptr) && ps.last_source->has_internal_playlist();
            if (last_has_internal_playlist) {
              ps.last_source->handle_command(media_source::MediaSourceCommand::PLAY);
            } else {
              if (ps.playlist_index >= ps.playlist.size()) {
                ps.playlist_index = 0;
              }
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            }
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::PLAY);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_PAUSE: {
          if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::PAUSE);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_STOP: {
          if (!has_internal_playlist) {
            this->cancel_timeout(PipelineState::TIMEOUT_IDS[pipeline]);
            ps.playlist.clear();
            ps.shuffle_indices.clear();
            ps.playlist_index = 0;
          }
          if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::STOP);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_NEXT: {
          if (!has_internal_playlist) {
            if (ps.playlist_index + 1 < ps.playlist.size()) {
              ps.playlist_index++;
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            } else if (ps.repeat_mode == REPEAT_ALL && !ps.playlist.empty()) {
              ps.playlist_index = 0;
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            }
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::NEXT);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_PREVIOUS: {
          if (!has_internal_playlist) {
            if (ps.playlist_index > 0) {
              ps.playlist_index--;
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            } else if (ps.repeat_mode == REPEAT_ALL && !ps.playlist.empty()) {
              ps.playlist_index = ps.playlist.size() - 1;
              this->queue_command_(MediaPlayerControlCommand::PLAY_CURRENT, pipeline);
            }
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::PREVIOUS);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ONE:
          if (!has_internal_playlist) {
            ps.repeat_mode = REPEAT_ONE;
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::REPEAT_ONE);
          }
          break;

        case media_player::MEDIA_PLAYER_COMMAND_REPEAT_OFF:
          if (!has_internal_playlist) {
            ps.repeat_mode = REPEAT_OFF;
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::REPEAT_OFF);
          }
          break;

        case media_player::MEDIA_PLAYER_COMMAND_REPEAT_ALL:
          if (!has_internal_playlist) {
            ps.repeat_mode = REPEAT_ALL;
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::REPEAT_ALL);
          }
          break;

        case media_player::MEDIA_PLAYER_COMMAND_CLEAR_PLAYLIST: {
          if (!has_internal_playlist) {
            this->cancel_timeout(PipelineState::TIMEOUT_IDS[pipeline]);
            if (ps.playlist_index < ps.playlist.size()) {
              size_t actual_position = this->get_playlist_position_(pipeline);
              std::string current = ps.playlist[actual_position];
              ps.playlist.clear();
              ps.playlist.push_back(current);
              ps.playlist_index = 0;
            } else {
              ps.playlist.clear();
              ps.playlist_index = 0;
            }
            ps.shuffle_indices.clear();
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::CLEAR_PLAYLIST);
          }
          break;
        }

        case media_player::MEDIA_PLAYER_COMMAND_SHUFFLE:
          if (!has_internal_playlist) {
            this->shuffle_playlist_(pipeline);
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::SHUFFLE);
          }
          break;

        case media_player::MEDIA_PLAYER_COMMAND_UNSHUFFLE:
          if (!has_internal_playlist) {
            this->unshuffle_playlist_(pipeline);
          } else if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::UNSHUFFLE);
          }
          break;

        case media_player::MEDIA_PLAYER_COMMAND_GROUP_JOIN:
          if (target_source != nullptr) {
            target_source->handle_command(media_source::MediaSourceCommand::GROUP_JOIN);
          }
          break;

        default:
          // TURN_ON, TURN_OFF, ENQUEUE (handled separately with URL) — no-op
          break;
      }

      command_executed = true;
      break;
    }
  }

  // Only remove from queue if successfully executed
  if (command_executed) {
    xQueueReceive(this->media_control_command_queue_, &control_command, 0);

    // Delete the allocated string for PLAY_URI and ENQUEUE_URI commands
    if (control_command.type == MediaPlayerControlCommand::PLAY_URI ||
        control_command.type == MediaPlayerControlCommand::ENQUEUE_URI) {
      delete control_command.data.uri;
    }
  }
}

void SpeakerSourceMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  if (!this->is_ready()) {
    return;
  }

  MediaPlayerControlCommand control_command;

  // Determine which pipeline to use based on announcement flag
  if (call.get_announcement().has_value() && call.get_announcement().value() &&
      this->pipelines_[ANNOUNCEMENT_PIPELINE].is_configured()) {
    control_command.pipeline = ANNOUNCEMENT_PIPELINE;
  } else {
    control_command.pipeline = MEDIA_PIPELINE;
  }

  if (call.get_media_url().has_value()) {
    bool enqueue =
        call.get_command().has_value() && call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_ENQUEUE;

    if (enqueue) {
      control_command.type = MediaPlayerControlCommand::ENQUEUE_URI;
    } else {
      control_command.type = MediaPlayerControlCommand::PLAY_URI;
    }
    control_command.data.uri = new std::string(call.get_media_url().value());
    if (xQueueSend(this->media_control_command_queue_, &control_command, 0) != pdTRUE) {
      delete control_command.data.uri;
      ESP_LOGE(TAG, "Failed to queue command, command dropped");
    }
    return;
  }

  if (call.get_volume().has_value()) {
    this->set_volume_(call.get_volume().value());
    this->publish_state();
    return;
  }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->set_mute_state_(true);
        this->publish_state();
        return;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->set_mute_state_(false);
        this->publish_state();
        return;
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP:
        this->set_volume_(std::min(1.0f, this->volume + this->volume_increment_));
        this->publish_state();
        return;
      case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN:
        this->set_volume_(std::max(0.0f, this->volume - this->volume_increment_));
        this->publish_state();
        return;
      default:
        break;
    }

    control_command.type = MediaPlayerControlCommand::SEND_COMMAND;
    control_command.data.command = call.get_command().value();
    xQueueSend(this->media_control_command_queue_, &control_command, 0);
  }
}

media_player::MediaPlayerTraits SpeakerSourceMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);

  for (const auto &ps : this->pipelines_) {
    if (ps.format.has_value()) {
      traits.get_supported_formats().push_back(ps.format.value());
    }
  }

  return traits;
}

void SpeakerSourceMediaPlayer::save_volume_restore_state_() {
  VolumeRestoreState volume_restore_state;
  volume_restore_state.volume = this->volume;
  volume_restore_state.is_muted = this->is_muted_;
  this->pref_.save(&volume_restore_state);
}

void SpeakerSourceMediaPlayer::set_mute_state_(bool mute_state, bool publish) {
  for (auto &ps : this->pipelines_) {
    if (ps.is_configured()) {
      ps.speaker->set_mute_state(mute_state);
    }
  }

  bool old_mute_state = this->is_muted_;
  this->is_muted_ = mute_state;

  if (publish) {
    this->save_volume_restore_state_();
  }

  // Notify all media sources about the mute state change
  for (auto &ps : this->pipelines_) {
    for (auto *media_source : ps.media_sources) {
      media_source->notify_mute_changed(mute_state);
    }
  }

  if (old_mute_state != mute_state) {
    if (mute_state) {
      this->defer([this]() { this->mute_trigger_.trigger(); });
    } else {
      this->defer([this]() { this->unmute_trigger_.trigger(); });
    }
  }
}

void SpeakerSourceMediaPlayer::set_volume_(float volume, bool publish) {
  // Remap the volume to fit within the configured limits
  float bounded_volume = remap<float, float>(volume, 0.0f, 1.0f, this->volume_min_, this->volume_max_);

  for (auto &ps : this->pipelines_) {
    if (ps.is_configured()) {
      ps.speaker->set_volume(bounded_volume);
    }
  }

  if (publish) {
    this->volume = volume;
    this->save_volume_restore_state_();
  }

  // Notify all media sources about the volume change
  for (auto &ps : this->pipelines_) {
    for (auto *media_source : ps.media_sources) {
      media_source->notify_volume_changed(volume);
    }
  }

  // Turn on the mute state if the volume is effectively zero, off otherwise.
  // Pass publish=false since set_volume_ already saved above.
  if (volume < 0.001) {
    this->set_mute_state_(true, false);
  } else {
    this->set_mute_state_(false, false);
  }

  this->defer([this, volume]() { this->volume_trigger_.trigger(volume); });
}

size_t SpeakerSourceMediaPlayer::get_playlist_position_(uint8_t pipeline) const {
  const PipelineState &ps = this->pipelines_[pipeline];

  if (ps.shuffle_indices.empty() || ps.playlist_index >= ps.shuffle_indices.size()) {
    return ps.playlist_index;
  }
  return ps.shuffle_indices[ps.playlist_index];
}

void SpeakerSourceMediaPlayer::shuffle_playlist_(uint8_t pipeline) {
  PipelineState &ps = this->pipelines_[pipeline];

  if (ps.playlist.size() <= 1) {
    ps.shuffle_indices.clear();
    return;
  }

  // Capture current actual position BEFORE modifying shuffle_indices
  size_t current_actual = this->get_playlist_position_(pipeline);

  // Build indices vector
  ps.shuffle_indices.resize(ps.playlist.size());
  for (size_t i = 0; i < ps.playlist.size(); i++) {
    ps.shuffle_indices[i] = i;
  }

  // Fisher-Yates shuffle using ESPHome's random helper
  for (size_t i = ps.shuffle_indices.size() - 1; i > 0; i--) {
    size_t j = random_uint32() % (i + 1);
    std::swap(ps.shuffle_indices[i], ps.shuffle_indices[j]);
  }

  // Move current track to current position (so playback continues seamlessly)
  if (ps.playlist_index < ps.shuffle_indices.size()) {
    for (size_t i = 0; i < ps.shuffle_indices.size(); i++) {
      if (ps.shuffle_indices[i] == current_actual) {
        std::swap(ps.shuffle_indices[i], ps.shuffle_indices[ps.playlist_index]);
        break;
      }
    }
  }
}

void SpeakerSourceMediaPlayer::unshuffle_playlist_(uint8_t pipeline) {
  PipelineState &ps = this->pipelines_[pipeline];

  if (!ps.shuffle_indices.empty() && ps.playlist_index < ps.shuffle_indices.size()) {
    ps.playlist_index = ps.shuffle_indices[ps.playlist_index];
  }
  ps.shuffle_indices.clear();
}

}  // namespace speaker_source
}  // namespace esphome

#endif  // USE_ESP32
