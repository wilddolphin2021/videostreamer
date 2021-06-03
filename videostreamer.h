#ifndef _VIDEOSTREAMER_H
#define _VIDEOSTREAMER_H

#include <libavformat/avformat.h>
#include <stdbool.h>
#include <stdint.h>

struct VSInput {
	AVFormatContext * format_ctx;
	int video_stream_index;
};

#define STREAM_DURATION   10.0
 // a wrapper around a single output AVStream
typedef struct VSOutputAudio {
  	AVCodecContext* c;
  	AVCodec* codec;
    AVStream *st;
    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;
    AVFrame *frame;
    AVFrame *tmp_frame;
    float t, tincr, tincr2;
    struct AVAudioResampleContext *avr_ctx;
} VSOutputAudio;

struct VSOutput {
	AVFormatContext * format_ctx;

  // Track the last dts we output. We use it to double check that dts is
  // monotonic.
  //
  // I am not sure if it is available anywhere already. I tried
  // AVStream->info->last_dts and that is apparently not set.
  int64_t last_dts;

  // Audio structure
  VSOutputAudio audio;
};

void
vs_setup(void);

struct VSInput *
vs_open_input(const char * const,
		const char * const, const bool);

void
vs_destroy_input(struct VSInput * const);

struct VSOutput *
vs_open_output(const char * const,
		const char * const, const struct VSInput * const,
		const bool, const bool);

void
vs_destroy_output(struct VSOutput * const);

int
vs_read_packet(const struct VSInput *, AVPacket * const,
		const bool);

int
vs_write_packet(const struct VSInput * const,
		struct VSOutput * const, AVPacket * const, const bool);

#endif
