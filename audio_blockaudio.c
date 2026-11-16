/*
 * blockaudio output driver. This file is part of Shairport Sync.
 * Copyright (c) Marius DKM 2026
 * All rights reserved.
 *
 * Writes the audio to a named pipe as a sequence of records, each a header followed by a block of
 * audio. The header gives the block's format, its RTP timestamp and the time at which its first
 * frame should be played. The format is documented in "ADVANCED TOPICS/BlockAudio.md".
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
#include <limits.h>
#include <memory.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "utilities/network_utilities.h"

// Header layout, version 1. Fields are in the host's byte order. Later versions only append
// fields, so the offsets below never change.
#define BLOCKAUDIO_MAGIC "SSBA"
#define BLOCKAUDIO_VERSION 1
#define BLOCKAUDIO_HEADER_LENGTH 40
#define OFFSET_MAGIC 0
#define OFFSET_VERSION 4
#define OFFSET_HEADER_LENGTH 6
#define OFFSET_PAYLOAD_LENGTH 8
#define OFFSET_SEQUENCE 12
#define OFFSET_HEADER_CRC32 16
#define OFFSET_FLAGS 20
#define OFFSET_RESERVED 22
#define OFFSET_FORMAT 24
#define OFFSET_RTP 28
#define OFFSET_PLAY_TIME 32

enum {
  BLOCKAUDIO_FLAG_TIMED = 1, // rtp and play_time are valid
  BLOCKAUDIO_FLAG_FLUSH = 2, // discard audio received but not yet played
  BLOCKAUDIO_FLAG_STOP = 4,  // the play session has ended
};

// a header-only record must be written atomically by a single non-blocking write()
#if BLOCKAUDIO_HEADER_LENGTH > PIPE_BUF
#error "a blockaudio header must not be longer than PIPE_BUF"
#endif

static int fd = -1;
static int warned = 0;
static int32_t encoded_format = 0;
static unsigned int bytes_per_frame = 0;
static uint32_t sequence = 0;

static uint8_t *record_buf = NULL;
static size_t record_buf_size = 0;

// fd, sequence and record_buf are only used with this held.
static pthread_mutex_t record_lock = PTHREAD_MUTEX_INITIALIZER;

// flush() is called with the player's flush_mutex held and must not block, so a flush or stop
// marker that can't be written at once is left here and written before the next record.
// Never held across a write.
static pthread_mutex_t pending_lock = PTHREAD_MUTEX_INITIALIZER;
static uint16_t pending_flags = 0;

static char *pipename = NULL;
static char *default_pipe_name = "/tmp/shairport-sync-blockaudio";

// CRC-32 with the parameters zlib's crc32() uses (reflected polynomial 0xEDB88320, initial value
// and final XOR 0xFFFFFFFF), so readers can check it with a standard library. Computed bit by bit:
// it only ever covers a header.
static uint32_t crc32_of(const uint8_t *p, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
  }
  return crc ^ 0xFFFFFFFF;
}

// call with record_lock held. Consumes a sequence number.
static void put_header(uint8_t *p, uint16_t flags, uint32_t payload_length, uint32_t rtp,
                       uint64_t play_time) {
  uint16_t version = BLOCKAUDIO_VERSION;
  uint16_t header_length = BLOCKAUDIO_HEADER_LENGTH;
  uint16_t reserved = 0;
  uint32_t crc = 0;
  uint32_t format = (uint32_t)encoded_format;
  memcpy(p + OFFSET_MAGIC, BLOCKAUDIO_MAGIC, 4);
  memcpy(p + OFFSET_VERSION, &version, sizeof(version));
  memcpy(p + OFFSET_HEADER_LENGTH, &header_length, sizeof(header_length));
  memcpy(p + OFFSET_PAYLOAD_LENGTH, &payload_length, sizeof(payload_length));
  memcpy(p + OFFSET_SEQUENCE, &sequence, sizeof(sequence));
  memcpy(p + OFFSET_HEADER_CRC32, &crc, sizeof(crc));
  memcpy(p + OFFSET_FLAGS, &flags, sizeof(flags));
  memcpy(p + OFFSET_RESERVED, &reserved, sizeof(reserved));
  memcpy(p + OFFSET_FORMAT, &format, sizeof(format));
  memcpy(p + OFFSET_RTP, &rtp, sizeof(rtp));
  memcpy(p + OFFSET_PLAY_TIME, &play_time, sizeof(play_time));
  crc = crc32_of(p, BLOCKAUDIO_HEADER_LENGTH);
  memcpy(p + OFFSET_HEADER_CRC32, &crc, sizeof(crc));
  sequence++;
}

static void log_write_error(int error) {
  if ((error != EPIPE) && (warned == 0)) {
    char errorstring[1024];
    strerror_r(error, (char *)errorstring, sizeof(errorstring));
    debug(1, "blockaudio: error %d writing to the pipe \"%s\": \"%s\".", error, pipename,
          errorstring);
    warned = 1;
  }
}

// call with record_lock held. On failure, the pipe is closed rather than resumed mid-record, so
// that a reader never sees the rest of a record without its start. The next play() reopens it.
static int write_fully(const uint8_t *buf, size_t size) {
  for (size_t done = 0; done < size;) {
    ssize_t n = write(fd, buf + done, size - done); // a cancellation point
    if (n < 0) {
      if (errno == EINTR)
        continue;
      int error = errno;
      log_write_error(error);
      safe_socket_close(&fd);
      return -1;
    }
    done += n;
  }
  return 0;
}

// call with record_lock held. If nonblocking, a full pipe leaves the markers pending.
static void write_pending_markers(int nonblocking) {
  pthread_mutex_lock(&pending_lock);
  uint16_t flags = pending_flags;
  pthread_mutex_unlock(&pending_lock);
  if (flags == 0)
    return;
  if (fd >= 0) { // without a reader, there is nothing to mark
    uint8_t header[BLOCKAUDIO_HEADER_LENGTH];
    uint32_t saved_sequence = sequence;
    put_header(header, flags, 0, 0, 0);
    if (nonblocking) {
      // a pipe write of up to PIPE_BUF bytes is atomic: all of the header, or EAGAIN.
      int fd_flags = fcntl(fd, F_GETFL);
      if ((fd_flags == -1) || (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) == -1)) {
        sequence = saved_sequence; // try again before the next record
        return;
      }
      ssize_t n;
      do {
        n = write(fd, header, sizeof(header));
      } while ((n < 0) && (errno == EINTR));
      int error = errno;
      if (fcntl(fd, F_SETFL, fd_flags) == -1) {
        // records must be written in blocking mode, so start again on a fresh descriptor
        debug(1, "blockaudio: can not make the pipe blocking again.");
        safe_socket_close(&fd);
      }
      if ((n < 0) && ((error == EAGAIN) || (error == EWOULDBLOCK))) {
        sequence = saved_sequence; // the pipe is full; the next record writes the markers
        return;
      }
      if ((n < 0) && (fd >= 0)) {
        log_write_error(error);
        safe_socket_close(&fd);
      }
    } else {
      write_fully(header, sizeof(header));
    }
  }
  pthread_mutex_lock(&pending_lock);
  pending_flags &= ~flags;
  pthread_mutex_unlock(&pending_lock);
}

// if the player thread is cancelled in the middle of a record, close the pipe so that the reader
// sees the end of the stream instead of the rest of the record, and release the lock.
static void record_cleanup(void *arg) {
  int *in_record = (int *)arg;
  if (*in_record)
    safe_socket_close(&fd);
  pthread_mutex_unlock(&record_lock);
}

static int play(void *buf, int samples, int sample_type, uint32_t timestamp, uint64_t playtime) {
  // configure() and play() are both called from the player thread
  if (bytes_per_frame == 0) {
    static int reported = 0;
    if (reported == 0)
      debug(1, "blockaudio: play() called without a valid output format.");
    reported = 1;
    return 0;
  }
  if (samples <= 0)
    return 0;
  size_t payload_length = (size_t)samples * bytes_per_frame;
  if (payload_length > UINT32_MAX - BLOCKAUDIO_HEADER_LENGTH) {
    debug(1, "blockaudio: a block of %d frames is too long for a record.", samples);
    return 0;
  }
  size_t size = BLOCKAUDIO_HEADER_LENGTH + payload_length;
  // untimed frames are silence inserted by Shairport Sync
  int timed = (sample_type == play_samples_are_timed) && (playtime != 0);

  int in_record = 0;
  pthread_mutex_lock(&record_lock);
  pthread_cleanup_push(record_cleanup, &in_record);
  if (fd == -1) {
    fd = try_to_open_pipe_for_writing(pipename);
    if (fd >= 0)
      warned = 0;
  }
  if (fd >= 0) {
    in_record = 1;
    write_pending_markers(0); // markers belong before this record
    if ((fd >= 0) && (size > record_buf_size)) {
      uint8_t *b = realloc(record_buf, size);
      if (b == NULL) {
        debug(1, "blockaudio: can not allocate %zu bytes for a record.", size);
      } else {
        record_buf = b;
        record_buf_size = size;
      }
    }
    if ((fd >= 0) && (size <= record_buf_size)) {
      put_header(record_buf, timed ? BLOCKAUDIO_FLAG_TIMED : 0, (uint32_t)payload_length,
                 timed ? timestamp : 0, timed ? playtime : 0);
      memcpy(record_buf + BLOCKAUDIO_HEADER_LENGTH, buf, payload_length);
      write_fully(record_buf, size);
    }
    // a flush or stop that came in while the record was being written goes out now, rather than
    // waiting for a next record that may not come, e.g. when playback is paused.
    write_pending_markers(0);
    in_record = 0;
  }
  pthread_cleanup_pop(1);
  return 0;
}

// queue the marker and write it at once if that can be done without blocking.
static void write_marker(uint16_t flags) {
  pthread_mutex_lock(&pending_lock);
  pending_flags |= flags;
  pthread_mutex_unlock(&pending_lock);
  int oldState;
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &oldState); // the caller expects no cancellation
  // if a record is being written, the next record writes the marker
  if (pthread_mutex_trylock(&record_lock) == 0) {
    write_pending_markers(1);
    pthread_mutex_unlock(&record_lock);
  }
  pthread_setcancelstate(oldState, NULL);
}

static void flush(void) { write_marker(BLOCKAUDIO_FLAG_FLUSH); }

static void stop(void) {
  // Don't close the pipe just because a play session has stopped.
  write_marker(BLOCKAUDIO_FLAG_STOP);
}

static int prepare(void) {
  int result = 0;
  // this runs on the RTSP thread: if a record is being written, the pipe is open and has a reader.
  if (pthread_mutex_trylock(&record_lock) != 0)
    return 0;
  if (fd == -1)
    fd = try_to_open_pipe_for_writing(pipename);
  // "ENXIO  O_NONBLOCK | O_WRONLY is set, the named file is a FIFO, and no process has the FIFO
  // open for reading." Which is okay.
  if ((fd == -1) && (errno != ENXIO)) {
    char errorstring[1024];
    strerror_r(errno, (char *)errorstring, sizeof(errorstring));
    warn("blockaudio: can not open the pipe \"%s\" -- error %d (\"%s\").", pipename, errno,
         (char *)errorstring);
    result = -1;
  }
  pthread_mutex_unlock(&record_lock);
  return result;
}

static int init(int argc, char **argv) {
  config.audio_backend_buffer_desired_length = 1.0;
  config.audio_backend_latency_offset = 0;

  // get settings from settings file, passing in defaults for format_set, rate_set and channel_set
  // Note, these options may be in the "general" stanza or the named stanza
#ifdef CONFIG_AIRPLAY_2
  parse_audio_options("blockaudio", (1 << SPS_FORMAT_S32_LE), (1 << SPS_RATE_48000), (1 << 2));
#else
  parse_audio_options("blockaudio", (1 << SPS_FORMAT_S16_LE), (1 << SPS_RATE_44100), (1 << 2));
#endif
  if (config.cfg != NULL) {
    const char *str;
    if (config_lookup_non_empty_string(config.cfg, "blockaudio.name", &str))
      pipename = (char *)str;
  }

  if (argc > 1)
    die("too many command-line arguments to blockaudio");

  if (argc == 1)
    pipename = argv[0]; // command line argument has priority

  if ((pipename) && (strcasecmp(pipename, "STDOUT") == 0))
    die("Can't use the \"blockaudio\" backend for STDOUT.");

  if (pipename == NULL)
    pipename = default_pipe_name;

  mode_t oldumask = umask(000);
  if (mkfifo(pipename, 0666) && errno != EEXIST)
    die("Could not create the blockaudio pipe \"%s\"", pipename);
  umask(oldumask);

  debug(1, "blockaudio pipe name is \"%s\"", pipename);
  return 0;
}

static void deinit(void) {
  pthread_mutex_lock(&record_lock);
  safe_socket_close(&fd);
  free(record_buf);
  record_buf = NULL;
  record_buf_size = 0;
  pthread_mutex_unlock(&record_lock);
}

static void help(void) {
  printf("    Provide the pipe's pathname. The default is \"%s\".\n", default_pipe_name);
}

static int32_t get_configuration(unsigned int channels, unsigned int rate, unsigned int format) {
  // every record carries its format, so any configuration the settings allow will do
  return search_for_suitable_configuration(channels, rate, format, NULL);
}

static int configure(int32_t requested_encoded_format, __attribute__((unused)) char **channel_map) {
  int response = 0;
  unsigned int bytes_per_sample =
      sps_format_sample_size(FORMAT_FROM_ENCODED_FORMAT(requested_encoded_format));
  unsigned int channels = CHANNELS_FROM_ENCODED_FORMAT(requested_encoded_format);
  pthread_mutex_lock(&record_lock);
  if ((bytes_per_sample == 0) || (channels == 0)) {
    debug(1, "blockaudio: unsupported output format %s.",
          short_format_description(requested_encoded_format));
    encoded_format = 0;
    bytes_per_frame = 0; // play() won't write anything until a valid format is configured
    response = EINVAL;
  } else {
    encoded_format = requested_encoded_format;
    bytes_per_frame = bytes_per_sample * channels;
  }
  pthread_mutex_unlock(&record_lock);
  return response;
}

audio_output audio_blockaudio = {.name = "blockaudio",
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
