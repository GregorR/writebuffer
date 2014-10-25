`writebuffer` and `fwritebuffer` are very simple programs for connecting two
programs with a pipe in the following situation:

* The writer program vitally must not (or can not) buffer due to write
  failures, and
* The reader program is either considerably slower than the writer program, or
  is not faster with enough consistency to be trusted not to stall the writer.

Gregor Richards uses `writebuffer` for recording video from a live source. The
rest of this document will use that use-case as an example, but `writebuffer`
is quite general. Because ffmpeg (and most other such recorders) will throttle
the inut if the hard disk write speed or encoding speed is too slow, recording
high-definition real-time video is liable to cause unpredictable frameloss,
sometimes quite severe. `writebuffer` is currently not configurable in any
meaningful way. It's a “something I wrote which others might find useful”
program, not a “for everyone's immediate public consumption” program.

`writebuffer` simply provides a large (2GB) memory buffer between two programs
in a pipe. If the reader stalls, data will be stored in the buffer. Please note
that Unix pipes already provide this functionality, but with a much smaller
buffer size. To use `writebuffer`, simply place it in the pipeline like any
other program. For example:

    $ ffmpeg -framerate 30 -video_size 1920x1080 -i :0 \
      -c:v libx264 -f matroska - | \
      writebuffer > file.mkv

This is suitable for situations where the output must be written to disk, but
due to contention, disk write speed is inconsistent.

`fwritebuffer` (which is built from the same writebuffer.c source) is broadly
similar, but uses the disk as its buffer, allowing for much more space than a
paltry 2GB. To use it, place it in the pipeline like any other program; make
sure to use `writebuffer` as well, since disk write speed may be inconsistent.
For example:

    $ ffmpeg -framerate 30 -video_size 1920x1080 -i :0 \
      -c:v ffvhuff -f nut - | \
      writebuffer | \
      fwritebuffer | \
      ffmpeg -f nut -i - \
      -c:v libx264 \
      file.mkv

This is suitable for situations where the reader is much slower than the
writer. e.g., in the above example, encoding to libx264 is much slower than
encoding to ffvhuff (and presumably far too slow to encode 1080P video at
30FPS). The buffered data is stored to the disk, so the reader program can
catch up even well after the data has been collected.
