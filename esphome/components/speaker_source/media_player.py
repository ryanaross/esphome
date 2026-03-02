from esphome import automation
import esphome.codegen as cg
from esphome.components import audio, media_player, media_source, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_DELAY,
    CONF_FORMAT,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_SAMPLE_RATE,
    CONF_SPEAKER,
)
from esphome.core.entity_helpers import inherit_property_from

from . import speaker_source_ns

CONF_ANNOUNCEMENT_PIPELINE = "announcement_pipeline"
CONF_MEDIA_PIPELINE = "media_pipeline"
CONF_ON_MUTE = "on_mute"
CONF_ON_UNMUTE = "on_unmute"
CONF_ON_VOLUME = "on_volume"
CONF_PIPELINE = "pipeline"
CONF_PLAYLIST_DELAY = "playlist_delay"
CONF_SOURCES = "sources"
CONF_VOLUME_INCREMENT = "volume_increment"
CONF_VOLUME_INITIAL = "volume_initial"
CONF_VOLUME_MAX = "volume_max"
CONF_VOLUME_MIN = "volume_min"

SpeakerSourceMediaPlayer = speaker_source_ns.class_(
    "SpeakerSourceMediaPlayer", cg.Component, media_player.MediaPlayer
)

Pipeline = speaker_source_ns.enum("Pipeline")
PIPELINE_ENUM = {
    "media": Pipeline.MEDIA_PIPELINE,
    "announcement": Pipeline.ANNOUNCEMENT_PIPELINE,
}

MuteTrigger = speaker_source_ns.class_("MuteTrigger", automation.Trigger.template())
UnmuteTrigger = speaker_source_ns.class_("UnmuteTrigger", automation.Trigger.template())
VolumeTrigger = speaker_source_ns.class_(
    "VolumeTrigger", automation.Trigger.template(cg.float_)
)

SetPlaylistDelayAction = speaker_source_ns.class_(
    "SetPlaylistDelayAction", automation.Action
)


# Returns a media_player.MediaPlayerSupportedFormat struct with the configured
# format, sample rate, number of channels, purpose, and bytes per sample
def _get_supported_format_struct(pipeline, pipeline_type):
    args = [
        media_player.MediaPlayerSupportedFormat,
    ]

    if pipeline[CONF_FORMAT] == "FLAC":
        args.append(("format", "flac"))
    elif pipeline[CONF_FORMAT] == "MP3":
        args.append(("format", "mp3"))
    elif pipeline[CONF_FORMAT] == "OPUS":
        args.append(("format", "opus"))
    elif pipeline[CONF_FORMAT] == "WAV":
        args.append(("format", "wav"))

    args.append(("sample_rate", pipeline[CONF_SAMPLE_RATE]))
    args.append(("num_channels", pipeline[CONF_NUM_CHANNELS]))

    if pipeline_type == "MEDIA":
        args.append(
            (
                "purpose",
                media_player.MEDIA_PLAYER_FORMAT_PURPOSE_ENUM["default"],
            )
        )
    elif pipeline_type == "ANNOUNCEMENT":
        args.append(
            (
                "purpose",
                media_player.MEDIA_PLAYER_FORMAT_PURPOSE_ENUM["announcement"],
            )
        )
    # Omit sample_bytes for MP3: ffmpeg transcoding in Home Assistant fails
    # if the number of bytes per sample is specified for MP3.
    if pipeline[CONF_FORMAT] != "MP3":
        args.append(("sample_bytes", 2))

    return cg.StructInitializer(*args)


def _validate_pipeline(config):
    # Inherit settings from speaker if not manually set
    inherit_property_from(CONF_NUM_CHANNELS, CONF_SPEAKER)(config)
    inherit_property_from(CONF_SAMPLE_RATE, CONF_SPEAKER)(config)

    # Opus only supports 48 kHz
    if config.get(CONF_FORMAT) == "OPUS" and config.get(CONF_SAMPLE_RATE) != 48000:
        raise cv.Invalid("Opus only supports a sample rate of 48000 Hz")

    audio.final_validate_audio_schema(
        "speaker_source media_player",
        audio_device=CONF_SPEAKER,
        bits_per_sample=16,
        channels=config.get(CONF_NUM_CHANNELS),
        sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)

    return config


PIPELINE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Required(CONF_SOURCES): cv.ensure_list(cv.use_id(media_source.MediaSource)),
        cv.Optional(CONF_FORMAT, default="FLAC"): cv.enum(audio.AUDIO_FILE_TYPE_ENUM),
        cv.Optional(CONF_SAMPLE_RATE): cv.int_range(min=1),
        cv.Optional(CONF_NUM_CHANNELS): cv.int_range(1, 2),
        cv.Optional(
            CONF_PLAYLIST_DELAY, default="0ms"
        ): cv.positive_time_period_milliseconds,
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SpeakerSourceMediaPlayer),
            cv.Optional(CONF_VOLUME_INCREMENT, default=0.05): cv.percentage,
            cv.Optional(CONF_VOLUME_INITIAL, default=0.5): cv.percentage,
            cv.Optional(CONF_VOLUME_MAX, default=1.0): cv.percentage,
            cv.Optional(CONF_VOLUME_MIN, default=0.0): cv.percentage,
            cv.Optional(CONF_ANNOUNCEMENT_PIPELINE): PIPELINE_SCHEMA,
            cv.Optional(CONF_MEDIA_PIPELINE): PIPELINE_SCHEMA,
            cv.Optional(CONF_ON_MUTE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_UNMUTE): automation.validate_automation(single=True),
            cv.Optional(CONF_ON_VOLUME): automation.validate_automation(single=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(media_player.media_player_schema(SpeakerSourceMediaPlayer)),
    cv.only_on_esp32,
    cv.has_at_least_one_key(CONF_ANNOUNCEMENT_PIPELINE, CONF_MEDIA_PIPELINE),
)


def _final_validate_codecs(config):
    needed_formats = set()
    need_all = False

    for pipeline_key in (CONF_ANNOUNCEMENT_PIPELINE, CONF_MEDIA_PIPELINE):
        if pipeline := config.get(pipeline_key):
            fmt = pipeline[CONF_FORMAT]
            if fmt == "NONE":
                need_all = True
            else:
                needed_formats.add(fmt)

    if need_all:
        audio.request_flac_support()
        audio.request_mp3_support()
        audio.request_opus_support()
    else:
        if "FLAC" in needed_formats:
            audio.request_flac_support()
        if "MP3" in needed_formats:
            audio.request_mp3_support()
        if "OPUS" in needed_formats:
            audio.request_opus_support()

    return config


FINAL_VALIDATE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_ANNOUNCEMENT_PIPELINE): _validate_pipeline,
            cv.Optional(CONF_MEDIA_PIPELINE): _validate_pipeline,
        },
        extra=cv.ALLOW_EXTRA,
    ),
    _final_validate_codecs,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)

    cg.add(var.set_volume_increment(config[CONF_VOLUME_INCREMENT]))
    cg.add(var.set_volume_initial(config[CONF_VOLUME_INITIAL]))
    cg.add(var.set_volume_max(config[CONF_VOLUME_MAX]))
    cg.add(var.set_volume_min(config[CONF_VOLUME_MIN]))

    if announcement_pipeline_config := config.get(CONF_ANNOUNCEMENT_PIPELINE):
        for source in announcement_pipeline_config[CONF_SOURCES]:
            src = await cg.get_variable(source)
            cg.add(var.add_media_source(Pipeline.ANNOUNCEMENT_PIPELINE, src))

        cg.add(
            var.set_speaker(
                Pipeline.ANNOUNCEMENT_PIPELINE,
                await cg.get_variable(announcement_pipeline_config[CONF_SPEAKER]),
            )
        )

        cg.add(
            var.set_format(
                Pipeline.ANNOUNCEMENT_PIPELINE,
                _get_supported_format_struct(
                    announcement_pipeline_config, "ANNOUNCEMENT"
                ),
            )
        )
        cg.add(
            var.set_playlist_delay_ms(
                Pipeline.ANNOUNCEMENT_PIPELINE,
                announcement_pipeline_config[CONF_PLAYLIST_DELAY],
            )
        )

    if media_pipeline_config := config.get(CONF_MEDIA_PIPELINE):
        for source in media_pipeline_config[CONF_SOURCES]:
            src = await cg.get_variable(source)
            cg.add(var.add_media_source(Pipeline.MEDIA_PIPELINE, src))

        cg.add(
            var.set_speaker(
                Pipeline.MEDIA_PIPELINE,
                await cg.get_variable(media_pipeline_config[CONF_SPEAKER]),
            )
        )

        cg.add(
            var.set_format(
                Pipeline.MEDIA_PIPELINE,
                _get_supported_format_struct(media_pipeline_config, "MEDIA"),
            )
        )
        cg.add(
            var.set_playlist_delay_ms(
                Pipeline.MEDIA_PIPELINE, media_pipeline_config[CONF_PLAYLIST_DELAY]
            )
        )

    if on_mute := config.get(CONF_ON_MUTE):
        await automation.build_automation(
            var.get_mute_trigger(),
            [],
            on_mute,
        )
    if on_unmute := config.get(CONF_ON_UNMUTE):
        await automation.build_automation(
            var.get_unmute_trigger(),
            [],
            on_unmute,
        )
    if on_volume := config.get(CONF_ON_VOLUME):
        await automation.build_automation(
            var.get_volume_trigger(),
            [(cg.float_, "x")],
            on_volume,
        )


SET_PLAYLIST_DELAY_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(SpeakerSourceMediaPlayer),
        cv.Required(CONF_PIPELINE): cv.enum(PIPELINE_ENUM, lower=True),
        cv.Required(CONF_DELAY): cv.templatable(cv.positive_time_period_milliseconds),
    }
)


@automation.register_action(
    "speaker_source.set_playlist_delay",
    SetPlaylistDelayAction,
    SET_PLAYLIST_DELAY_ACTION_SCHEMA,
)
async def set_playlist_delay_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)

    cg.add(var.set_pipeline(config[CONF_PIPELINE]))

    template_ = await cg.templatable(config[CONF_DELAY], args, cg.uint32)
    cg.add(var.set_delay(template_))

    return var
