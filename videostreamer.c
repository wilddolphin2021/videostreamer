//
// This library provides remuxing from a video stream (such as an RTSP URL) to
// an MP4 container. It writes a fragmented MP4 so that it can be streamed to a
// pipe.
//
// There is no re-encoding. The stream is copied as is.
//
// The logic here is heavily based on remuxing.c by Stefano Sabatini.
//

#include <errno.h>
#include <libavdevice/avdevice.h>
#include <libavutil/timestamp.h>
#include <libavresample/avresample.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "videostreamer.h"

static void
__vs_log_packet(const AVFormatContext * const,
		const AVPacket * const, const char * const);
int vs_open_audio(struct VSOutput* output, AVCodec* acodec);

void
vs_setup(void)
{
	// Set up library.

	// Register muxers, demuxers, and protocols.
	av_register_all();

	// Make formats available.
	avdevice_register_all();

	avformat_network_init();
}

struct VSInput *
vs_open_input(const char * const input_format_name,
		const char * const input_url, const bool verbose)
{
	if (!input_format_name || strlen(input_format_name) == 0 ||
			!input_url || strlen(input_url) == 0) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct VSInput * const input = calloc(1, sizeof(struct VSInput));
	if (!input) {
		printf("%s\n", strerror(errno));
		return NULL;
	}


	AVInputFormat * const input_format = av_find_input_format(input_format_name);
	if (!input_format) {
		printf("input format not found\n");
		vs_destroy_input(input);
		return NULL;
	}

	AVDictionary * opts = NULL;

	// IP Cam options
	if(!strcmp(input_format_name, "rtsp")) {
		printf("setting rtsp options\n");

		av_dict_set( &opts, "max_delay", "5000000", 0 );		
		av_dict_set( &opts, "flags", "low_delay", 0 );
		av_dict_set( &opts, "rtsp_transport", "tcp", 0 );
		av_dict_set( &opts, "framerate", "25", 0 );
		av_dict_set( &opts, "allowed_media_types", "video", 0 );
	}

	int const open_status = avformat_open_input(&input->format_ctx, input_url,
			input_format, &opts);
	if (open_status != 0) {
		printf("unable to open input: %s\n", av_err2str(open_status));
		vs_destroy_input(input);
		return NULL;
	}

	if (avformat_find_stream_info(input->format_ctx, NULL) < 0) {
		printf("failed to find stream info\n");
		vs_destroy_input(input);
		return NULL;
	}


	if (verbose) {
		av_dump_format(input->format_ctx, 0, input_url, 0);
	}


	// Find the first video stream.
	input->video_stream_index = -1;

	for (unsigned int i = 0; i < input->format_ctx->nb_streams; i++) {
		AVStream * const in_stream = input->format_ctx->streams[i];

		if (in_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
			if (verbose) {
				printf("skip non-video stream %u\n", i);
			}
			continue;
		}

		input->video_stream_index = (int) i;
		break;
	}

	if (input->video_stream_index == -1) {
		printf("no video stream found\n");
		vs_destroy_input(input);
		return NULL;
	}

	av_dict_free(&opts);

	return input;
}

void
vs_destroy_input(struct VSInput * const input)
{
	if (!input) {
		return;
	}

	if (input->format_ctx) {
		avformat_close_input(&input->format_ctx);		
	}

	free(input);
}

struct VSOutput *
vs_open_output(const char * const output_format_name,
		const char * const output_url, const struct VSInput * const input,
		const bool audio, const bool verbose)
{
	if (!output_format_name || strlen(output_format_name) == 0 ||
			!output_url || strlen(output_url) == 0 ||
			!input) {
		printf("%s\n", strerror(EINVAL));
		return NULL;
	}

	struct VSOutput * const output = calloc(1, sizeof(struct VSOutput));
	if (!output) {
		printf("%s\n", strerror(errno));
		return NULL;
	}


	AVOutputFormat * const output_format = av_guess_format(output_format_name,
			output_url, NULL);
	if (!output_format) {
		printf("output format not found\n");
		vs_destroy_output(output);
		return NULL;
	}

	if (avformat_alloc_output_context2(&output->format_ctx, output_format,
				NULL, NULL) < 0) {
		printf("unable to create output context\n");
		vs_destroy_output(output);
		return NULL;
	}


	// Copy the video stream.

	AVStream * const out_stream = avformat_new_stream(output->format_ctx, NULL);
	if (!out_stream) {
		printf("unable to add stream\n");
		vs_destroy_output(output);
		return NULL;
	}

	AVStream * const in_stream = input->format_ctx->streams[
		input->video_stream_index];

	if (avcodec_parameters_copy(out_stream->codecpar,
				in_stream->codecpar) < 0) {
		printf("unable to copy codec parameters\n");
		vs_destroy_output(output);
		return NULL;
	}

	AVCodecContext* cctx = in_stream->codec;

    out_stream->sample_aspect_ratio.num = cctx->sample_aspect_ratio.num;
    out_stream->sample_aspect_ratio.den = cctx->sample_aspect_ratio.den;
    // Assume r_frame_rate is accurate
    in_stream->r_frame_rate.num = 25;
    in_stream->r_frame_rate.den = 1;
    out_stream->r_frame_rate      = in_stream->r_frame_rate;
    out_stream->avg_frame_rate    = out_stream->r_frame_rate;

    in_stream->codec = cctx;
    out_stream->time_base.den = out_stream->time_base.num = 0;

	// Add the audio stream
	if(audio) {
	    /* find the encoder */
	    AVCodec* acodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	    if (acodec == NULL) {
	       printf("Could not find encoder for '%s'\n",
	                avcodec_get_name(AV_CODEC_ID_AAC));
	    }

		output->audio.st = avformat_new_stream(output->format_ctx, acodec);
		if (!output->audio.st) {
			printf("unable to add audio stream\n");
		} else {
			if (vs_open_audio(output, acodec) < -1) {
				printf("unable to open audio\n");
				vs_destroy_output(output);
			}
		}
	}

	if (verbose) {
		av_dump_format(output->format_ctx, 0, output_url, 1);
	}

	// Open output file.
	if (avio_open(&output->format_ctx->pb, output_url, AVIO_FLAG_WRITE) < 0) {
		printf("unable to open output file\n");
		vs_destroy_output(output);
		return NULL;
	}


	// Write file header.

	AVDictionary * opts = NULL;

	// -movflags frag_keyframe tells the mp4 muxer to fragment at each video
	// keyframe. This is necessary for it to support output to a non-seekable
	// file (e.g., pipe).
	//
	// -movflags isml+frag_keyframe is the same, except isml appears to be to
	// make the output a live smooth streaming feed (as opposed to not live). I'm
	// not sure the difference, but isml appears to be a microsoft
	// format/protocol.
	//
	// To specify both, use isml+frag_keyframe as the value.
	//
	// I found that while Chrome had no trouble displaying the resulting mp4 with
	// just frag_keyframe, Firefox would not until I also added empty_moov.
	// empty_moov apparently writes some info at the start of the file.
	if (av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov", 0) < 0) {
		printf("unable to set movflags opt\n");
		vs_destroy_output(output);
		return NULL;
	}

	if (av_dict_set_int(&opts, "flush_packets", 1, 0) < 0) {
		printf("unable to set flush_packets opt\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}


	if(!strcmp(output_format_name, "flv")) {
		printf("setting flv options\n");

		if (av_dict_set(&opts, "flvflags", "no_duration_filesize", 0) < 0) {
			printf("unable to set flvflags no_duration_filesize opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "flvflags", "aac_seq_header_detect", 0) < 0) {
			printf("unable to set flvflags aac_seq_header_detect opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "flvflags", "no_sequence_end", 0) < 0) {
			printf("unable to set flvflags no_sequence_end opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "flvflags", "no_metadata", 0) < 0) {
			printf("unable to set flvflags no_metadata opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set_int(&opts, "threads", 0, 0) < 0) {
			printf("unable to set threads opt\n");
			vs_destroy_output(output);
			av_dict_free(&opts);
			return NULL;
		}
	}

	if(!strcmp(output_format_name, "hls")) {
		printf("setting hls options\n");
		
		if (av_dict_set(&opts, "hls_time", "2", 0) < 0) {
			printf("unable to set hls_time opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "hls_list_size", "4", 0) < 0) {
			printf("unable to set hls_list_size opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "hls_playlist_type", "event", 0) < 0) {
			printf("unable to set hls_playlist_type event opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "http_persistent", "1", 0) < 0) {
			printf("unable to set http_persistent opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		if (av_dict_set(&opts, "method", "POST", 0) < 0) {
			printf("unable to set method opt\n");
			vs_destroy_output(output);
			return NULL;
		}

		AVDictionary * options = NULL;
		if (av_opt_set_dict(&output->format_ctx->av_class, &options) < 0) {
			printf("unable to set avio context option dictionary\n");
			vs_destroy_output(output);
			return NULL;
		}
		av_dict_free(&options);
	}

	if (avformat_write_header(output->format_ctx, &opts) < 0) {
		printf("unable to write header\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}

	// Check any options that were not set. Because I'm not sure if all are
	// appropriate to set through the avformat_write_header().
	/*if (av_dict_count(opts) != 0) {
		printf("some options not set\n");
		vs_destroy_output(output);
		av_dict_free(&opts);
		return NULL;
	}*/

	av_dict_free(&opts);

	output->last_dts = AV_NOPTS_VALUE;

	return output;
}

void
vs_destroy_output(struct VSOutput * const output)
{
	if (!output) {
		return;
	}

	if (output->format_ctx) {
		if (av_write_trailer(output->format_ctx) != 0) {
			printf("unable to write trailer\n");
		}

		if (avio_closep(&output->format_ctx->pb) != 0) {
			printf("avio_closep failed\n");
		}

		avformat_free_context(output->format_ctx);
	}

	free(output);
}

// AUDIO
int 
vs_write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;
    /* Write the compressed frame to the media file. */
    __vs_log_packet(fmt_ctx, pkt, "a");
    return av_interleaved_write_frame(fmt_ctx, pkt);
}

AVFrame*
alloc_audio_frame(enum AVSampleFormat sample_fmt,
                  uint64_t channel_layout,
                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    int ret;
    if (!frame) {
        printf("Error allocating an audio frame\n");
        return NULL;
    }
    frame->format = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;
    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            printf("Error allocating an audio buffer\n");
            return NULL;
        }
    }
    return frame;
}

int 
vs_open_audio(struct VSOutput *output, AVCodec* acodec)
{
    int nb_samples;
    int ret;

    VSOutputAudio *ost = &(output->audio);
	AVCodecContext *c = avcodec_alloc_context3(acodec);

    c = ost->st->codec;
    c->codec_type = AVMEDIA_TYPE_AUDIO;

    c->time_base.num = 1;
    c->time_base.den = 1000;

    c->sample_fmt  = acodec->sample_fmts ?
        acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = 64000;
    c->sample_rate = 44100;
    if (acodec->supported_samplerates) {
        c->sample_rate = acodec->supported_samplerates[0];
        for (int i = 0; acodec->supported_samplerates[i]; i++) {
            if (acodec->supported_samplerates[i] == 44100)
                c->sample_rate = 44100;
        }
    }
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    c->channel_layout = AV_CH_LAYOUT_STEREO;
    if (acodec->channel_layouts) {
        c->channel_layout = acodec->channel_layouts[0];
        for (int i = 0; acodec->channel_layouts[i]; i++) {
            if (acodec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
                c->channel_layout = AV_CH_LAYOUT_STEREO;
        }
    }
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    ost->st->time_base = (AVRational){ 1, c->sample_rate };

    if (output->format_ctx->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ost->st->codec = c;
    ost->st->id = output->format_ctx->nb_streams-1;
    ost->st->time_base.den = c->time_base.den;
    ost->st->time_base.num = c->time_base.num;

    AVDictionary *opts = NULL;
    /* open it */
    av_dict_set(&opts, "strict", "experimental", 0);

    ret = avcodec_open2(c, acodec, &opts);    
    av_dict_free(&opts);

    if (ret < 0) {
        printf("Could not open audio codec: %s\n", av_err2str(ret));
        return ret;
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        printf("Could not copy the stream parameters");
        return ret;
    }

    ost->next_pts  = 0;

    /* init signal generator */
    ost->t     = 0;
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;
    ost->frame     = alloc_audio_frame(c->sample_fmt, c->channel_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, c->channel_layout,
                                       c->sample_rate, nb_samples);
    /* create resampler context */
    ost->avr_ctx = avresample_alloc_context();
    if (!ost->avr_ctx) {
        printf("Could not allocate resampler context\n");
        return -1;
    }
    /* set options */
    av_opt_set_int       (ost->avr_ctx, "in_channel_layout",  c->channel_layout, 0);
    av_opt_set_int       (ost->avr_ctx, "out_channel_layout", c->channel_layout, 0);
    av_opt_set_int       (ost->avr_ctx, "in_sample_rate",     c->sample_rate,    0);
    //av_opt_set_sample_fmt(ost->avr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int       (ost->avr_ctx, "out_sample_rate",    44100,    0);
    //av_opt_set_sample_fmt(ost->avr_ctx, "out_sample_fmt",     c->sample_fmt,  0);
    /* initialize the resampling context */
    if ((ret = avresample_open(ost->avr_ctx)) < 0) {
        printf("Failed to initialize the resampling context\n");
        return ret;
    }

   return 0;     
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
AVFrame*
vs_get_audio_frame(struct VSOutput *output)
{
    VSOutputAudio *ost = &(output->audio);
    AVFrame *frame = ost->tmp_frame;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];
    /* check if we want to generate more frames */
    //if (av_compare_ts(ost->next_pts, ost->st->codec->time_base,
    //                  STREAM_DURATION, (AVRational){ 1, 1 }) >= 0)
    //    return NULL;
    for (j = 0; j <frame->nb_samples; j++) {
        v = (int)(sin(ost->t) * 10000);
        for (i = 0; i < ost->st->codec->channels; i++)
            *q++ = v;
        ost->t     += ost->tincr;
        ost->tincr += ost->tincr2;
    }
    frame->pts = ost->next_pts;
    ost->next_pts  += frame->nb_samples;
    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
int 
vs_write_audio_frame(struct VSOutput* output, AVPacket * const refPkt)
{
    AVCodecContext *c;
    AVPacket pkt = { 0 }; // data and size must be 0;
    AVFrame *frame;
    int ret;
    int got_packet;
    int dst_nb_samples;
 
	if (!output || !refPkt) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

    VSOutputAudio *ost = &(output->audio);
    AVFormatContext *fmt_ctx = output->format_ctx;

	ost->st = output->format_ctx->streams[refPkt->stream_index+1];
	if (!ost->st) {
		printf("output stream not found with stream index %d\n", refPkt->stream_index);
		return -1;
	}

    av_init_packet(&pkt);
    c = ost->st->codec;

    frame = vs_get_audio_frame(output);
    if (frame) {
        // convert samples from native format to destination codec format, using the resampler 
        // compute destination number of samples */
        dst_nb_samples = av_rescale_rnd(avresample_get_delay(ost->avr_ctx) + frame->nb_samples,
                                        c->sample_rate, c->sample_rate, AV_ROUND_UP);
        // assert (dst_nb_samples == frame->nb_samples);
        // when we pass a frame to the encoder, it may keep a reference to it
        // internally;
        // make sure we do not overwrite it here
        //
        ret = av_frame_make_writable(ost->frame);
        if (ret < 0)
           // convert to destination format
            ret = avresample_convert_frame(ost->avr_ctx,
                              ost->frame, frame);
            if (ret < 0) {
                printf("Error while converting\n");
                return ret;
            }
            frame = ost->frame;
        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        ost->samples_count += dst_nb_samples;
    }
    ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet);
    if (ret < 0) {
        printf("Error encoding audio frame: %s\n", av_err2str(ret));
        return ret;
    }
    if (got_packet) {
    	// fix pts
		pkt.pts = refPkt->pts;
		pkt.dts = refPkt->dts;

        ret = vs_write_frame(fmt_ctx, &c->time_base, ost->st, &pkt);
        if (ret < 0) {
            printf("Error while writing audio frame: %s\n",
                    av_err2str(ret));
            return ret;
        }
    }
    return (frame || got_packet) ? 0 : 1;
}

// Read a compressed and encoded frame as a packet.
//
// Returns:
// -1 if error
// 0 if nothing useful read (e.g., non-video packet)
// 1 if read a packet
int
vs_read_packet(const struct VSInput * input, AVPacket * const pkt,
		const bool verbose)
{
	if (!input || !pkt) {
		printf("%s\n", strerror(errno));
		return -1;
	}

	memset(pkt, 0, sizeof(AVPacket));


	// Read encoded frame (as a packet).

	if (av_read_frame(input->format_ctx, pkt) != 0) {
		printf("unable to read frame\n");
		return -1;
	}


	// Ignore it if it's not our video stream.

	if (pkt->stream_index != input->video_stream_index) {
		if (verbose) {
			printf("skipping packet from input stream %d, our video is from stream %d\n",
					pkt->stream_index, input->video_stream_index);
		}

		av_packet_unref(pkt);
		return 0;
	}


	if (verbose) {
		__vs_log_packet(input->format_ctx, pkt, "in");
	}

	return 1;
}

// We change the packet's pts, dts, duration, pos.
//
// We do not unref it.
//
// Returns:
// -1 if error
// 1 if we wrote the packet
int
vs_write_packet(const struct VSInput * const input,
		struct VSOutput * const output, AVPacket * const pkt, const bool verbose)
{
	if (!input || !output || !pkt) {
		printf("%s\n", strerror(EINVAL));
		return -1;
	}

	AVStream * const in_stream  = input->format_ctx->streams[pkt->stream_index];
	if (!in_stream) {
		printf("input stream not found with stream index %d\n", pkt->stream_index);
		return -1;
	}


	// If there are multiple input streams, then the stream index on the packet
	// may not match the stream index in our output. We need to ensure the index
	// matches. Note by this point we have checked that it is indeed a packet
	// from the stream we want (we do this when reading the packet).
	//
	// As we only ever have a single output stream (one, video), the index will
	// be 0.
	if (pkt->stream_index != 0) {
		if (verbose) {
			printf("updating packet stream index to 0 (from %d)\n",
					pkt->stream_index);
		}

		pkt->stream_index = 0;
	}


	AVStream * const out_stream = output->format_ctx->streams[pkt->stream_index];
	if (!out_stream) {
		printf("output stream not found with stream index %d\n", pkt->stream_index);
		return -1;
	}

	// It is possible that the input is not well formed. Its dts (decompression
	// timestamp) may fluctuate. av_write_frame() says that the dts must be
	// strictly increasing.
	//
	// Packets from such inputs might look like:
	//
	// in: pts:18750 pts_time:0.208333 dts:18750 dts_time:0.208333 duration:3750 duration_time:0.0416667 stream_index:1
	// in: pts:0 pts_time:0 dts:0 dts_time:0 duration:3750 duration_time:0.0416667 stream_index:1
	//
	// dts here is 18750 and then 0.
	//
	// If we try to write the second packet as is, we'll see this error:
	// [mp4 @ 0x10f1ae0] Application provided invalid, non monotonically increasing dts to muxer in stream 1: 18750 >= 0
	//
	// This is apparently a fairly common problem. In ffmpeg.c (as of ffmpeg
	// 3.2.4 at least) there is logic to rewrite the dts and warn if it happens.
	// Let's do the same. Note my logic is a little different here.
	bool fix_dts = pkt->dts != AV_NOPTS_VALUE &&
		output->last_dts != AV_NOPTS_VALUE &&
		pkt->dts <= output->last_dts;

	// It is also possible for input streams to include a packet with
	// dts/pts=NOPTS after packets with dts/pts set. These won't be caught by the
	// prior case. If we try to send these to the encoder however, we'll generate
	// the same error (non monotonically increasing DTS) since the output packet
	// will have dts/pts=0.
	fix_dts |= pkt->dts == AV_NOPTS_VALUE && output->last_dts != AV_NOPTS_VALUE;

	if (fix_dts) {
		int64_t const next_dts = output->last_dts+1;

		if (verbose) {
			printf("Warning: Non-monotonous DTS in input stream. Previous: %" PRId64 " current: %" PRId64 ". changing to %" PRId64 ".\n",
					output->last_dts, pkt->dts, next_dts);
		}

		// We also apparently (ffmpeg.c does this too) need to update the pts.
		// Otherwise we see an error like:
		//
		// [mp4 @ 0x555e6825ea60] pts (3780) < dts (22531) in stream 0

		if (pkt->pts != AV_NOPTS_VALUE && pkt->pts >= pkt->dts) {
			pkt->pts = FFMAX(pkt->pts, next_dts);
		}
		// In the case where pkt->dts was AV_NOPTS_VALUE, pkt->pts can be
		// AV_NOPTS_VALUE too which we fix as well.
		if (pkt->pts == AV_NOPTS_VALUE) {
			pkt->pts = next_dts;
		}

		pkt->dts = next_dts;
	}


	// Set pts/dts if not set. Otherwise we will receive warnings like
	//
	// [mp4 @ 0x55688397bc40] Timestamps are unset in a packet for stream 0. This
	// is deprecated and will stop working in the future. Fix your code to set
	// the timestamps properly
	//
	// [mp4 @ 0x55688397bc40] Encoder did not produce proper pts, making some up.
	if (pkt->pts == AV_NOPTS_VALUE) {
		pkt->pts = 0;
	} else {
		pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base,
				out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	}

	if (pkt->dts == AV_NOPTS_VALUE) {
		pkt->dts = 0;
	} else {
		pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base,
				out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
	}

	pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base,
			out_stream->time_base);
	pkt->pos = -1;


	if (verbose) {
		__vs_log_packet(output->format_ctx, pkt, "out");
	}


	// Track last dts we see (see where we use it for why).
	output->last_dts = pkt->dts;


	// Write encoded frame (as a packet).

	// av_interleaved_write_frame() works too, but I don't think it is needed.
	// Using av_write_frame() skips buffering.
	const int write_res = av_write_frame(output->format_ctx, pkt);
	if (write_res != 0) {
		printf("unable to write frame: %s\n", av_err2str(write_res));
		return -1;
	}

	// Write audio packet
	if(output->audio.st) {
		const int write_audio_res = vs_write_audio_frame(output, pkt);
		if (write_audio_res != 0) {
			printf("unable to write audio frame: %s\n", av_err2str(write_audio_res));
		}
	}

	return 1;
}

static void
__vs_log_packet(const AVFormatContext * const format_ctx,
		const AVPacket * const pkt, const char * const tag)
{
		AVRational * const time_base = &format_ctx->streams[pkt->stream_index]->time_base;

		printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
				tag, av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
				av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
				av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
				pkt->stream_index);
}
