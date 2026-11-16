# Block Audio Output

The `blockaudio` backend writes audio to a named pipe, like the `pipe` backend, but in records: each block of audio is preceded by a header that gives its format, its RTP timestamp and the time at which its first frame should be played. This means that the program reading the pipe can schedule the audio itself – for example, to keep its own outputs in sync with the other AirPlay speakers the source is playing to – and it will see every change of format as it happens.

To use it, build Shairport Sync with the `--with-blockaudio` configuration flag (see [here](../CONFIGURATION%20FLAGS.md)). To check that it is built in, make sure that the string `blockaudio` is included in the version string. (Enter `$ shairport-sync -V` to get the version string.)

Then select the backend and, optionally, the pipe in the configuration file:

```
general =
{
	output_backend = "blockaudio";
};

blockaudio =
{
	name = "/tmp/shairport-sync-blockaudio"; // this is the default
};
```

The `output_rate`, `output_format` and `output_channels` settings work just as they do for the `pipe` backend.

## Records

The pipe carries a sequence of records. Each record is a header, followed by `payload_length` bytes of audio. Header-only records have no audio.

Header fields are in the host's native byte order. The play time is only meaningful on the same machine anyway (see [Timing](#timing)).

| Offset | Size | Field | Description |
| ---: | ---: | :--- | :--- |
| 0 | 4 | `magic` | The bytes `SSBA`. Written as bytes, so the same on any host. |
| 4 | 2 | `version` | `1`. |
| 6 | 2 | `header_length` | Length of the header in bytes: `40` in version 1. |
| 8 | 4 | `payload_length` | Length of the audio that follows, in bytes. `0` for header-only records. |
| 12 | 4 | `sequence` | One more than in the record before, including header-only records. Wraps around. |
| 16 | 4 | `header_crc32` | CRC-32 of the first `header_length` bytes, computed with this field set to `0`. |
| 20 | 2 | `flags` | See below. |
| 22 | 2 | reserved | `0`. |
| 24 | 4 | `format` | Format of the audio, encoded as below. |
| 28 | 4 | `rtp` | RTP timestamp of the first frame, at the source's rate. `0` if not timed. |
| 32 | 8 | `play_time` | Time at which the first frame should be played, in nanoseconds. `0` if not timed. |

The CRC-32 is the one used by zlib, Ethernet and PNG (reflected polynomial `0xEDB88320`), so you can check it with Python's `zlib.crc32()`, for example.

The flags are:

| Bit | Name | Meaning |
| ---: | :--- | :--- |
| 0 | `timed` | `rtp` and `play_time` are valid. Records that are not timed are silence inserted by Shairport Sync. |
| 1 | `flush` | Discard any audio received but not yet played, for example after the source seeks or pauses. |
| 2 | `stop` | The play session has ended. |

Other bits are reserved and are `0`. Flush and stop are sent in header-only records, and both may be set in the same record.

### Format

`format` is Shairport Sync's encoded output format:

| Bits | Content |
| :--- | :--- |
| 0–5 | Sample format, see below. |
| 6–24 | Frames per second, divided by 2. |
| 25–31 | Number of channels. |

The audio is interleaved. The sample formats are:

| Value | Format | Bytes per sample |
| ---: | :--- | ---: |
| 1 | `S8` | 1 |
| 2 | `U8` | 1 |
| 3 | `S16_LE` | 2 |
| 4 | `S16_BE` | 2 |
| 5 | `S24_LE` (in 32 bits) | 4 |
| 6 | `S24_BE` (in 32 bits) | 4 |
| 7 | `S24_3LE` | 3 |
| 8 | `S24_3BE` | 3 |
| 9 | `S32_LE` | 4 |
| 10 | `S32_BE` | 4 |

Header-only records carry the current format, or `0` if no format has been set yet.

### Later Versions

Later versions only append fields after the ones above, which keep their offsets and meanings. A reader that knows version 1 can read any later version: it uses the fields it knows and skips the rest of the header using `header_length`.

## Timing

The frames of each record follow on directly from those of the record before, so frame `n` of a timed record should be played at `play_time + n × 1,000,000,000 / rate` nanoseconds. A record that is not timed plays straight after the record before it.

The play time is taken from the clock Shairport Sync uses internally: `CLOCK_MONOTONIC_RAW` (or `CLOCK_MONOTONIC` if that is not available) on Linux and the BSDs, and `mach_absolute_time()` on macOS. Your reader must compare it with the same clock, so it has to run on the same machine (or in a container on the same kernel).

The RTP timestamp can be matched against the RTP timestamps in the [metadata](Metadata.md) – to find out which track a block belongs to, for example.

## Reading the Pipe

#### Keep Reading
Read the pipe continuously from start-up, whether or not anything is playing. While the pipe is full, Shairport Sync waits to write the next record, which holds up the player, just as with the `pipe` backend.

#### Flush and Stop Can Arrive Late
Shairport Sync never waits to write a flush or a stop. If a record is being written at that moment, the marker follows straight after it. If the pipe is full, it is written before the next record instead, so a reader that keeps the pipe drained gets it at once. A late flush and stop may arrive together in one record.

#### End of File
If a write fails, or the play session is torn down while a record is being written, Shairport Sync closes its end of the pipe instead of finishing the record, and reopens it when the next block of audio is ready. If your reader gets an end-of-file, it should discard any partial record, treat it as a flush and then open the pipe again.

#### Resynchronising
A reader that starts reading a pipe that already has data in it – after a restart, say – may start in the middle of a record. To find the start of the next one, search for `SSBA` and check `header_crc32`. Optionally, check that the next record begins exactly `header_length + payload_length` bytes later. Audio is full of arbitrary bytes, so don't trust a header whose CRC doesn't match. Gaps can be detected from `sequence`.

### Example

Here is a minimal reader in Python:

```python
import struct, zlib

HEADER = struct.Struct("=4sHHIIIHHIIQ")  # native byte order, no padding

def read_exactly(pipe, n):
    data = pipe.read(n)
    if len(data) < n:
        raise EOFError  # discard the partial record, treat it as a flush, reopen the pipe
    return data

with open("/tmp/shairport-sync-blockaudio", "rb") as pipe:
    while True:
        fixed = read_exactly(pipe, HEADER.size)
        (magic, version, header_length, payload_length, sequence, crc,
         flags, _, fmt, rtp, play_time) = HEADER.unpack(fixed)
        if magic != b"SSBA" or header_length < HEADER.size:
            raise ValueError("lost sync: search for the next SSBA")  # see above
        header = bytearray(fixed + read_exactly(pipe, header_length - HEADER.size))
        header[16:20] = bytes(4)
        if zlib.crc32(header) != crc:
            raise ValueError("lost sync: search for the next SSBA")
        audio = read_exactly(pipe, payload_length)
        channels, rate, sample_format = fmt >> 25, ((fmt >> 6) & 0x7FFFF) * 2, fmt & 0x3F
        # ... schedule `audio` to start at `play_time` if flags & 1, handle flush/stop ...
```
