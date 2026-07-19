// Manual test: gcc -o /tmp/transcode_headers_test src/transcode_headers_test.c
#include <assert.h>
#include <stdio.h>

// Mirrors the exact guard added to open_input() in transcode.c -- kept here
// as a small, isolated regression check since open_input() itself can't be
// unit-tested without linking ffmpeg and a live HTTP source.
static int
should_set_headers(const char *headers)
{
  return headers && *headers;
}

int
main(void)
{
  assert(should_set_headers(NULL) == 0);
  assert(should_set_headers("") == 0);
  assert(should_set_headers("Referer: https://www.youtube.com/") != 0);
  printf("All transcode_headers tests passed.\n");
  return 0;
}
