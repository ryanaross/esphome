import esphome.codegen as cg
from esphome.components import media_source
import esphome.config_validation as cv
from esphome.const import CONF_TASK_STACK_IN_PSRAM

from .. import get_audio_file_ids

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["audio_file", "media_source", "audio"]

audio_file_ns = cg.esphome_ns.namespace("audio_file")
AudioFileMediaSource = audio_file_ns.class_(
    "AudioFileMediaSource", cg.Component, media_source.MediaSource
)

CONFIG_SCHEMA = (
    media_source.media_source_schema(
        AudioFileMediaSource,
    )
    .extend(
        {
            cv.Optional(CONF_TASK_STACK_IN_PSRAM): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[cv.CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)

    if CONF_TASK_STACK_IN_PSRAM in config:
        cg.add(var.set_task_stack_in_psram(config[CONF_TASK_STACK_IN_PSRAM]))

    # Add all shared audio files from the audio_file component
    for file_id_str, file_config_id in get_audio_file_ids().items():
        file_var = await cg.get_variable(file_config_id)
        cg.add(var.add_file(file_var, file_id_str))
