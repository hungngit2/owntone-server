// src/misc_channels_test.c
// Manual test: gcc -o /tmp/misc_channels_test src/misc_channels_test.c -lpthread
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

// Enum definition for the functions we're testing
enum output_channels {
  OUTPUT_CHANNELS_BOTH = 0,
  OUTPUT_CHANNELS_LEFT,
  OUTPUT_CHANNELS_RIGHT,
};

// Standalone implementations for testing (normally these come from misc.c)
static enum output_channels
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

static const char *
output_channels_to_string(enum output_channels channels)
{
  if (channels == OUTPUT_CHANNELS_LEFT)
    return "left";
  if (channels == OUTPUT_CHANNELS_RIGHT)
    return "right";

  return "both";
}

static void
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

static void
test_from_to_string(void)
{
  assert(output_channels_from_string("both") == OUTPUT_CHANNELS_BOTH);
  assert(output_channels_from_string("left") == OUTPUT_CHANNELS_LEFT);
  assert(output_channels_from_string("right") == OUTPUT_CHANNELS_RIGHT);
  assert(output_channels_from_string("bogus") == OUTPUT_CHANNELS_BOTH);
  assert(output_channels_from_string(NULL) == OUTPUT_CHANNELS_BOTH);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_BOTH), "both") == 0);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_LEFT), "left") == 0);
  assert(strcmp(output_channels_to_string(OUTPUT_CHANNELS_RIGHT), "right") == 0);
  printf("test_from_to_string: OK\n");
}

static void
test_transform_both_is_noop(void)
{
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }; // 4 stereo frames
  int16_t orig[8];
  memcpy(orig, buf, sizeof(buf));

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_BOTH);

  assert(memcmp(buf, orig, sizeof(buf)) == 0);
  printf("test_transform_both_is_noop: OK\n");
}

static void
test_transform_left(void)
{
  // frames: (L,R) = (1,2), (3,4), (5,6), (7,8)
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  int16_t expected[8] = { 1, 1, 3, 3, 5, 5, 7, 7 }; // right := left

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_LEFT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_left: OK\n");
}

static void
test_transform_right(void)
{
  int16_t buf[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
  int16_t expected[8] = { 2, 2, 4, 4, 6, 6, 8, 8 }; // left := right

  channel_transform((uint8_t *)buf, sizeof(buf), 16, 2, OUTPUT_CHANNELS_RIGHT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_right: OK\n");
}

static void
test_transform_24bit(void)
{
  // 2 frames, 24-bit stereo, 3 bytes/sample, little-endian: L=0x010203, R=0x040506
  uint8_t buf[12] = {
    0x03, 0x02, 0x01,  0x06, 0x05, 0x04,
    0x03, 0x02, 0x01,  0x06, 0x05, 0x04,
  };
  uint8_t expected[12] = {
    0x03, 0x02, 0x01,  0x03, 0x02, 0x01, // right := left
    0x03, 0x02, 0x01,  0x03, 0x02, 0x01,
  };

  channel_transform(buf, sizeof(buf), 24, 2, OUTPUT_CHANNELS_LEFT);

  assert(memcmp(buf, expected, sizeof(buf)) == 0);
  printf("test_transform_24bit: OK\n");
}

int
main(void)
{
  test_from_to_string();
  test_transform_both_is_noop();
  test_transform_left();
  test_transform_right();
  test_transform_24bit();
  printf("All misc_channels tests passed.\n");
  return 0;
}
