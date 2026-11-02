/*
 * pipe output driver. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Modifications for audio synchronisation
 * and related work, copyright (c) Mike Brady 2014--2025
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio.h"
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "utilities/network_utilities.h"

static int fd = -1;
static int warned = 0;
static unsigned int bytes_per_frame = 0;
static unsigned int output_rate = 0;

// Optional record framing: a 32-byte little-endian header before every payload, so the reader
// knows when each frame should be played. Without a delay() method the player hands over every
// buffer contiguously from its playtime, so a record's frames follow on from its should_be time.
#define TIMING_HEADER_MAGIC 0x48545053 // "SPTH"
#define TIMING_HEADER_VERSION 1
#define TIMING_HEADER_SIZE 32
enum { TIMING_FLAG_TIMED = 1, TIMING_FLAG_FLUSH = 2, TIMING_FLAG_STOP = 4 };

static int timing_headers = 0;
static uint8_t *record_buf = NULL;
static size_t record_buf_size = 0;
// flush() can come from another thread while the player thread writes
static pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;

char *pipename = NULL;
char *default_pipe_name = "/tmp/shairport-sync-audio";

static int prepare() {
  int result = 0;
  // this will leave fd as -1 if a reader hasn't been attached to the pipe
  // we check that it's not a "real" error though. From the "man 2 open" page:
  // "ENXIO  O_NONBLOCK | O_WRONLY is set, the named file is a FIFO, and no process has the FIFO
  // open for reading."

  fd = try_to_open_pipe_for_writing(pipename);
  // we check that it's not a "real" error. From the "man 2 open" page:
  // "ENXIO  O_NONBLOCK | O_WRONLY is set, the named file is a FIFO, and no process has the FIFO
  // open for reading." Which is okay.
  if ((fd == -1) && (errno != ENXIO)) {
    char errorstring[1024];
    strerror_r(errno, (char *)errorstring, sizeof(errorstring));
    debug(1, "audio_pipe start -- error %d (\"%s\") opening pipe: \"%s\".", errno,
          (char *)errorstring, pipename);
    warn("can not open audio pipe -- error %d (\"%s\") opening pipe: \"%s\".", errno,
         (char *)errorstring, pipename);
    result = -1;
  }
  return result;
}

static void put_le(uint8_t *p, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++)
    p[i] = (v >> (8 * i)) & 0xFF;
}

static int write_record(uint16_t flags, const void *payload, int frames, uint32_t timestamp,
                        uint64_t playtime) {
  size_t payload_size = (size_t)frames * bytes_per_frame;
  size_t size = TIMING_HEADER_SIZE + payload_size;
  int rc = 0;
  int oldState;
  // a record cut short by cancellation would desync the reader, and stop() runs from the
  // cancelled player thread's cleanup handler, so the lock must never be held at a cancellation
  // point.
  // The write below can therefore block uncancellably (and flush() on record_lock) while the
  // pipe is full. The reader must always drain it: Music Assistant reads it from start-up,
  // independent of any stream. A stalled reader holds up session teardown only until it reads
  // again; a reader that is gone makes write() fail with EPIPE (SIGPIPE is ignored).
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState);
  pthread_mutex_lock(&record_lock);
  if (size > record_buf_size) {
    uint8_t *b = realloc(record_buf, size);
    if (b == NULL) {
      rc = -1;
      errno = ENOMEM;
      goto out;
    }
    record_buf = b;
    record_buf_size = size;
  }
  put_le(record_buf + 0, TIMING_HEADER_MAGIC, 4);
  put_le(record_buf + 4, TIMING_HEADER_VERSION, 2);
  put_le(record_buf + 6, flags, 2);
  put_le(record_buf + 8, frames, 4);
  put_le(record_buf + 12, timestamp, 4);
  put_le(record_buf + 16, playtime, 8);
  put_le(record_buf + 24, output_rate, 4);
  put_le(record_buf + 28, bytes_per_frame, 4);
  if (payload_size)
    memcpy(record_buf + TIMING_HEADER_SIZE, payload, payload_size);
  for (size_t done = 0; done < size;) {
    ssize_t n = write(fd, record_buf + done, size - done);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      rc = -1;
      goto out;
    }
    done += n;
    rc += n;
  }
out:
  pthread_mutex_unlock(&record_lock);
  pthread_setcancelstate(oldState, NULL);
  return rc;
}

static void write_marker(uint16_t flags) {
  if (timing_headers && fd > 0)
    write_record(flags, NULL, 0, 0, 0);
}

static int play(void *buf, int samples, __attribute__((unused)) int sample_type,
                uint32_t timestamp, uint64_t playtime) {
  // if the file is not open, try to open it.
  char errorstring[1024];
  if (fd == -1) {
    fd = try_to_open_pipe_for_writing(pipename);
  }
  // if it's got a reader, write to it.
  if (fd > 0) {
    // int rc = non_blocking_write(fd, buf, samples * 4);
    if (bytes_per_frame == 0)
      debug(1, "pipe: bytes per frame not initialised before play()!");
    int rc;
    if (timing_headers)
      rc = write_record(playtime ? TIMING_FLAG_TIMED : 0, buf, samples, timestamp, playtime);
    else
      rc = write(fd, buf, samples * bytes_per_frame);
    if ((rc < 0) && (errno != EPIPE) && (warned == 0)) {
      strerror_r(errno, (char *)errorstring, 1024);
      debug(1, "error %d writing to the pipe named \"%s\": \"%s\".", errno, pipename, errorstring);
      warned = 1;
    }
  }
  return 0;
}

static void stop(void) {
  // Don't close the pipe just because a play session has stopped.
  write_marker(TIMING_FLAG_STOP);
}

static void flush(void) { write_marker(TIMING_FLAG_FLUSH); }

static int init(int argc, char **argv) {
  //  debug(1, "pipe init");
  //  const char *str;
  //  int value;
  //  double dvalue;

  // set up default values first

  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  // get settings from settings file, passing in defaults for format_set, rate_set and channel_set
  // Note, these options may be in the "general" stanza or the named stanza
#ifdef CONFIG_AIRPLAY_2
  parse_audio_options("pipe", (1 << SPS_FORMAT_S32_LE), (1 << SPS_RATE_48000), (1 << 2));
#else
  parse_audio_options("pipe", (1 << SPS_FORMAT_S16_LE), (1 << SPS_RATE_44100), (1 << 2));
#endif
  if (config.cfg != NULL) {
    /* Get the Output Pipename. */
    const char *str;
    if (config_lookup_non_empty_string(config.cfg, "pipe.name", &str)) {
      pipename = (char *)str;
    } else {
      die("pipename needed");
    }
    config_set_lookup_bool(config.cfg, "pipe.timing_headers", &timing_headers);
  }

  if (argc > 1)
    die("too many command-line arguments to pipe");

  if (argc == 1)
    pipename = argv[0]; // command line argument has priority

  if ((pipename) && (strcasecmp(pipename, "STDOUT") == 0))
    die("Can't use \"pipe\" backend for STDOUT. Use the \"stdout\" backend instead.");

  if (pipename == NULL)
    pipename = default_pipe_name; // if none specified

  // here, create the pipe
  mode_t oldumask = umask(000);
  if (mkfifo(pipename, 0666) && errno != EEXIST)
    die("Could not create audio pipe \"%s\"", pipename);
  umask(oldumask);

  debug(1, "audio pipe name is \"%s\"%s", pipename,
        timing_headers ? ", with timing headers" : "");

  return 0;
}

static void deinit(void) {
  safe_socket_close(&fd);
  free(record_buf);
  record_buf = NULL;
  record_buf_size = 0;
}

static void help(void) {
  printf("    Provide the pipe's pathname. The default is \"%s\".\n", default_pipe_name);
}

static int32_t get_configuration(unsigned int channels, unsigned int rate, unsigned int format) {
  // use the standard format/rate/channel search to get a suitable configuration. No
  // check_configuration() method needs to be passed to search_for_suitable_configuration() because
  // it will always return a valid choice based on any settings and the defaults
  return search_for_suitable_configuration(channels, rate, format, NULL);
}

static int configure(int32_t requested_encoded_format, __attribute__((unused)) char **channel_map) {
  int response = 0;
  unsigned int bytes_per_sample =
      sps_format_sample_size(FORMAT_FROM_ENCODED_FORMAT(requested_encoded_format));
  if (bytes_per_sample == 0) {
    debug(1, "pipe: unknown output format.");
    bytes_per_sample = 4; // emergency hack
    response = EINVAL;
  }
  bytes_per_frame = bytes_per_sample * CHANNELS_FROM_ENCODED_FORMAT(requested_encoded_format);
  output_rate = RATE_FROM_ENCODED_FORMAT(requested_encoded_format);
  return response;
}

audio_output audio_pipe = {.name = "pipe",
                           .help = &help,
                           .init = &init,
                           .deinit = &deinit,
                           .prepare = &prepare,
                           .get_configuration = &get_configuration,
                           .configure = &configure,
                           .stop = &stop,
                           .is_running = NULL,
                           .flush = &flush,
                           .delay = NULL,
                           .stats = NULL,
                           .play = &play,
                           .volume = NULL,
                           .parameters = NULL,
                           .mute = NULL};
