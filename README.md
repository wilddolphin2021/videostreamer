# videostreamer
videostreamer provides a way to stream IP Cam video from an input source to YouTube using RTMP.

## Build requirements
* ffmpeg libraries (libavcodec, libavformat, libavdevice, libavutil,
  libavresample).
  * It should work with versions 3.2.x or later.
  * It does not work with 3.0.x or earlier as it depends on new APIs.
  * I'm not sure whether it works with 3.1.x.
* C compiler. Currently it requires a compiler with C11 support.
* Go. It should work with any Go 1 version.


## Installation
* Install the build dependencies (including ffmpeg libraries and a C
  compiler).
  * On Debian/Ubuntu, these packages should include what you need:
    `git-core pkg-config libavutil-dev libavcodec-dev libavformat-dev
    libavdevice-dev`
* Build the daemon.
  * You need a working Go build environment.
  * Run `go get github.com/horgh/videostreamer`
  * This places the `videostreamer` binary at `$GOPATH/bin/videostreamer`.
* Place index.html somewhere accessible. Update the `<video>` element src
  attribute.
* Run the daemon. Its usage output shows the possible flags. There is no
  configuration file.


## Components
* `videostreamer`: The daemon.
* `index.html`: A small sample website with a `<video>` element which
  demonstrates how to stream from the daemon.
* `videostreamer.h`: A library using the ffmpeg libraries to read a video
  input and remux and write to another format.

## Runnning examples
./videostreamer -verbose -format rtsp -i rtsp://<user>:<password>@192.168.1.109 -f flv -o rtmp://a.rtmp.youtube.com/live2/<STREAMKEY> -a

