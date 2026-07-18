#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "misc.h"

enum output_channels
output_channels_from_string(const char *s)
{
  if (!s)
    return OUTPUT_CHANNELS_BOTH;
  if (strcmp(s, "left") == 0)
    return OUTPUT_CHANNELS_LEFT;
  if (strcmp(s, "right") == 0)
    return OUTPUT_CHANNELS_RIGHT;

  return OUTPUT_CHANNELS_BOTH;
}

const char *
output_channels_to_string(enum output_channels channels)
{
  if (channels == OUTPUT_CHANNELS_LEFT)
    return "left";
  if (channels == OUTPUT_CHANNELS_RIGHT)
    return "right";

  return "both";
}

void
channel_transform(uint8_t *buffer, size_t bufsize, int bits_per_sample, int channels, enum output_channels mode)
{
  int bytes_per_sample;
  int frame_size;
  size_t nframes;
  size_t i;
  uint8_t *frame;

  if (mode == OUTPUT_CHANNELS_BOTH || channels != 2)
    return;

  bytes_per_sample = bits_per_sample / 8;
  frame_size = bytes_per_sample * channels;
  nframes = bufsize / frame_size;

  for (i = 0, frame = buffer; i < nframes; i++, frame += frame_size)
    {
      if (mode == OUTPUT_CHANNELS_LEFT)
	memcpy(frame + bytes_per_sample, frame, bytes_per_sample); // right := left
      else // OUTPUT_CHANNELS_RIGHT
	memcpy(frame, frame + bytes_per_sample, bytes_per_sample); // left := right
    }
}
