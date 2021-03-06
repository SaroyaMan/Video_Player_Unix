/***  
 *  Unix Programming - Project 2 

 *
 *  Special two channel video player based on FFMPEG and SDL
 *  Made by Yoav Saroya (304835887) & Amit Shmuel (305213621)
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

#include <SDL.h>
#include <SDL_thread.h>
#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif
#include <stdio.h>
#include <math.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)
#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0
#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20
#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)
#define VIDEO_PICTURE_QUEUE_SIZE 1
#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

SDL_AudioSpec wanted_spec, spec;
int is_multi_videos = 1;
int mute = 0;
int is_fast = 0;

typedef struct PacketQueue {
	AVPacketList *first_pkt, *last_pkt;
	int nb_packets;
	int size;
	SDL_mutex *mutex;
	SDL_cond *cond;
}PacketQueue;

typedef struct VideoPicture {
	SDL_Overlay *bmp;
	int width, height; /* source height & width */
	int allocated;
	double pts;
}VideoPicture;

typedef struct VideoState {
	AVFormatContext *pFormatCtx;
	int             videoStream, audioStream;

	int             av_sync_type;
	int64_t         external_clock_time;
	int             seek_req;
	int             seek_flags;
	int64_t         seek_pos;

	double          audio_clock;
	AVStream        *audio_st;
	PacketQueue     audioq;
	AVFrame         audio_frame;
	uint8_t         audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int    audio_buf_size;
	unsigned int    audio_buf_index;
	AVPacket        audio_pkt;
	uint8_t         *audio_pkt_data;
	int             audio_pkt_size;
	int             audio_hw_buf_size;
	double          audio_diff_cum; /* used for AV difference average computation */
	double          audio_diff_avg_coef;
	double          audio_diff_threshold;
	int             audio_diff_avg_count;
	double          frame_timer;
	double          frame_last_pts;
	double          frame_last_delay;
	double          video_clock; ///<pts of last decoded frame / predicted pts of next decoded frame
	double          video_current_pts; ///<current displayed pts (different from video_clock if frame fifos are used)
	int64_t         video_current_pts_time;  ///<time (av_gettime) at which we updated video_current_pts - used to have 		running video pts
	AVStream        *video_st;
	PacketQueue     videoq;
	VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int             pictq_size, pictq_rindex, pictq_windex;

	AVFrame		*colorq[VIDEO_PICTURE_QUEUE_SIZE];
	int             colorq_size, colorq_rindex, colorq_windex;

	SDL_mutex       *pictq_mutex;
	SDL_cond        *pictq_cond;

	SDL_mutex		 *colorq_mutex;
	SDL_cond		 *colorq_cond;

	SDL_Thread      *parse_tid;
	SDL_Thread      *video_tid;

	char            filename[1024];
	int             quit;

	AVIOContext     *io_context;
	struct SwsContext *sws_ctx;

	struct SwsContext *sws_ctx_audio;

	int			  color_flag, save_picture_flag, flag_sound;
	double			curr_pts;

	struct VideoState	*is2;	// another VideoState representing the small video
	int				is_small; // small video flag	

}VideoState;

enum {AV_SYNC_AUDIO_MASTER,AV_SYNC_VIDEO_MASTER,AV_SYNC_EXTERNAL_MASTER};

SDL_Surface     *screen;

/* Since we only have one decoding thread, the Big Struct
   can be global in case we need it. */
VideoState *global_video_state;
AVPacket flush_pkt;

void packet_queue_init(PacketQueue *q) {

	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt) {

	AVPacketList *pkt1;
	if(pkt != &flush_pkt && av_dup_packet(pkt) < 0) {
		return -1;
	}
	pkt1 = av_malloc(sizeof(AVPacketList));
	if (!pkt1)
		return -1;
	pkt1->pkt = *pkt;
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);

	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	q->nb_packets++;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);

	SDL_UnlockMutex(q->mutex);
	return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
	AVPacketList *pkt1;
	int ret;

	SDL_LockMutex(q->mutex);

	for(;;) {

		if(global_video_state->quit) {
			ret = -1;
			break;
		}

		pkt1 = q->first_pkt;
		if (pkt1) {
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			q->nb_packets--;
			q->size -= pkt1->pkt.size;
			*pkt = pkt1->pkt;
			av_free(pkt1);
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

static void packet_queue_flush(PacketQueue *q) {

	AVPacketList *pkt, *pkt1;

	SDL_LockMutex(q->mutex);
	for(pkt = q->first_pkt; pkt != NULL; pkt = pkt1) {
		pkt1 = pkt->next;
		av_free_packet(&pkt->pkt);
		av_freep(&pkt);
	}
	q->last_pkt = NULL;
	q->first_pkt = NULL;
	q->nb_packets = 0;
	q->size = 0;
	SDL_UnlockMutex(q->mutex);
}

double get_audio_clock(VideoState *is) {

	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = is->audio_clock; /* maintained in the audio thread */
	hw_buf_size = is->audio_buf_size - is->audio_buf_index;
	bytes_per_sec = 0;
	n = is->audio_st->codec->channels * 2;
	if(is->audio_st) {
		bytes_per_sec = is->audio_st->codec->sample_rate * n;
	}
	if(bytes_per_sec) {
		pts -= (double)hw_buf_size / bytes_per_sec;
	}
	return pts;
}

double get_video_clock(VideoState *is) {

	double delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}

double get_external_clock(VideoState *is) {

	return av_gettime() / 1000000.0;
}

double get_master_clock(VideoState *is) {

	if(is->av_sync_type == AV_SYNC_VIDEO_MASTER) return get_video_clock(is);
	else if(is->av_sync_type == AV_SYNC_AUDIO_MASTER) return get_audio_clock(is);
	else return get_external_clock(is);
}

/* Add or subtract samples to get a better sync, return new
   audio buffer size */
int synchronize_audio(VideoState *is, short *samples,int samples_size, double pts) {

	int n;

	n = 2 * is->audio_st->codec->channels;

	if(is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int wanted_size, min_size, max_size /*, nb_samples */;

		diff = get_audio_clock(is) - get_master_clock(is);

		if(diff < AV_NOSYNC_THRESHOLD) {
			// accumulate the diffs
			is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
			if(is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				is->audio_diff_avg_count++;
			}
			else {
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);
				if(fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_size = samples_size + ((int)(diff * is->audio_st->codec->sample_rate) * n);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
					if(wanted_size < min_size) {
						wanted_size = min_size;
					}
					else if (wanted_size > max_size) {
						wanted_size = max_size;
					}
					if(wanted_size < samples_size) {
						/* remove samples */
						samples_size = wanted_size;
					}
					else if(wanted_size > samples_size) {
						uint8_t *samples_end, *q;
						int nb;

						/* add samples by copying final sample*/
						nb = (samples_size - wanted_size);
						samples_end = (uint8_t *)samples + samples_size - n;
						q = samples_end + n;
						while(nb > 0) {
							memcpy(q, samples_end, n);
							q += n;
							nb -= n;
						}
						samples_size = wanted_size;
					}
				}
			}
		}
		else {
			/* difference is TOO big; reset diff stuff */
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	if(is_fast) samples_size/=2;
	return samples_size;
}

int decode_frame_from_packet(VideoState *is, AVFrame decoded_frame)
{
	int64_t src_ch_layout, dst_ch_layout;
	int src_rate, dst_rate;
	uint8_t **src_data = NULL, **dst_data = NULL;
	int src_nb_channels = 0, dst_nb_channels = 0;
	int src_linesize, dst_linesize;
	int src_nb_samples, dst_nb_samples;
	enum AVSampleFormat src_sample_fmt, dst_sample_fmt;
	int dst_bufsize;
	int ret;

	src_nb_samples = decoded_frame.nb_samples;
	src_linesize = (int) decoded_frame.linesize[0];
	src_data = decoded_frame.data;

	if (decoded_frame.channel_layout == 0) {
		decoded_frame.channel_layout = av_get_default_channel_layout(decoded_frame.channels);
	}

	src_rate = decoded_frame.sample_rate;
	dst_rate = decoded_frame.sample_rate;
	src_ch_layout = decoded_frame.channel_layout;
	dst_ch_layout = decoded_frame.channel_layout;
	src_sample_fmt = decoded_frame.format;
	dst_sample_fmt = AV_SAMPLE_FMT_S16;

	av_opt_set_int(is->sws_ctx_audio, "in_channel_layout", src_ch_layout, 0);
	av_opt_set_int(is->sws_ctx_audio, "out_channel_layout", dst_ch_layout,  0);
	av_opt_set_int(is->sws_ctx_audio, "in_sample_rate", src_rate, 0);
	av_opt_set_int(is->sws_ctx_audio, "out_sample_rate", dst_rate, 0);
	av_opt_set_sample_fmt(is->sws_ctx_audio, "in_sample_fmt", src_sample_fmt, 0);
	av_opt_set_sample_fmt(is->sws_ctx_audio, "out_sample_fmt", dst_sample_fmt,  0);

	/* initialize the resampling context */
	if ((ret = swr_init((void*)is->sws_ctx_audio)) < 0) {
		fprintf(stderr, "Failed to initialize the resampling context\n");
		return -1;
	}

	/* allocate source and destination samples buffers */
	src_nb_channels = av_get_channel_layout_nb_channels(src_ch_layout);
	ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels, src_nb_samples, src_sample_fmt, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate source samples\n");
		return -1;
	}

	/* compute the number of converted samples: buffering is avoided
	 * ensuring that the output buffer will contain at least all the
	 * converted input samples */
	dst_nb_samples = av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

	/* buffer is going to be directly written to a rawaudio file, no alignment */
	dst_nb_channels = av_get_channel_layout_nb_channels(dst_ch_layout);
	ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels, dst_nb_samples, dst_sample_fmt, 0);
	if (ret < 0) {
		fprintf(stderr, "Could not allocate destination samples\n");
		return -1;
	}

	/* compute destination number of samples */
	dst_nb_samples = av_rescale_rnd(swr_get_delay((void*)is->sws_ctx_audio, src_rate) + src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

	/* convert to destination format */
	ret = swr_convert((void*)is->sws_ctx_audio,dst_data,dst_nb_samples,(const uint8_t **)decoded_frame.data, src_nb_samples);
	if (ret < 0) {
		fprintf(stderr, "Error while converting\n");
		return -1;
	}

	dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels, ret, dst_sample_fmt, 1);
	if (dst_bufsize < 0) {
		fprintf(stderr, "Could not get sample buffer size\n");
		return -1;
	}

	memcpy(is->audio_buf, dst_data[0], dst_bufsize);

	if (src_data) {
		av_freep(&src_data[0]);
	}
	av_freep(&src_data);

	if (dst_data) {
		av_freep(&dst_data[0]);
	}
	av_freep(&dst_data);

	return dst_bufsize;
}

int audio_decode_frame(VideoState *is, double *pts_ptr) {

	int len1, data_size = 0, n;
	AVPacket *pkt = &is->audio_pkt;
	double pts;

	for(;;) {
		while(is->audio_pkt_size > 0) {
			int got_frame = 0;
			len1 = avcodec_decode_audio4(is->audio_st->codec, &is->audio_frame, &got_frame, pkt);
			if(len1 < 0) {
				/* if error, skip frame */
				is->audio_pkt_size = 0;
				break;
			}
			if (got_frame) {
				if (is->audio_frame.format != AV_SAMPLE_FMT_S16) {
					data_size = decode_frame_from_packet(is, is->audio_frame);
				}
				else {
					data_size =
							av_samples_get_buffer_size
							(
									NULL,
									is->audio_st->codec->channels,
									is->audio_frame.nb_samples,
									is->audio_st->codec->sample_fmt,
									1
							);
					memcpy(is->audio_buf, is->audio_frame.data[0], data_size);
				}
			}
			is->audio_pkt_data += len1;
			is->audio_pkt_size -= len1;
			if(data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			pts = is->audio_clock;
			*pts_ptr = pts;
			n = 2 * is->audio_st->codec->channels;
			is->audio_clock += (double)data_size /
					(double)(n * is->audio_st->codec->sample_rate);

			/* We have data, return it and come back for more later */
			return data_size;
		}
		if(pkt->data)
			av_free_packet(pkt);

		if(is->quit) {
			return -1;
		}
		/* next packet */
		if(packet_queue_get(&is->audioq, pkt, 1) < 0) {
			return -1;
		}
		if(pkt->data == flush_pkt.data) {
			avcodec_flush_buffers(is->audio_st->codec);
			continue;
		}
		is->audio_pkt_data = pkt->data;
		is->audio_pkt_size = pkt->size;
		/* if update, update the audio clock w/pts */
		if(pkt->pts != AV_NOPTS_VALUE) {
			is->audio_clock = av_q2d(is->audio_st->time_base)*pkt->pts;
		}
	}
}

void audio_callback(void *userdata, Uint8 *stream, int len) {

	VideoState *is = (VideoState *)userdata;
	int len1, audio_size;
	double pts;
	while(len > 0) {
		if(is->audio_buf_index >= is->audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(is, &pts);
			if(audio_size < 0) {
				/* If error, output silence */
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			}
			else {
				audio_size = synchronize_audio(is, (int16_t *)is->audio_buf,audio_size, pts);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if(len1 > len)
			len1 = len;
		if(!mute) // muting both videos
			memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

void audio_callback_manager(void *userdata, Uint8 *stream, int len) {

	VideoState* is = (VideoState*) userdata;
	if (is->flag_sound == 2) {
		audio_callback(is, stream, len);
		audio_callback(is->is2, stream, len);
	}
	if (is->flag_sound == 1) {
		audio_callback(is->is2, stream, len);
		audio_callback(is, stream, len);
	}
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {

	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(VideoState *is, int delay) {

	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is) {

	SDL_Rect rect;
	VideoPicture *vp;
	//AVPicture pict;
	float aspect_ratio;
	int w, h, x, y;
	//int i;

	vp = &is->pictq[is->pictq_rindex];
	if(vp->bmp) {
		if(is->video_st->codec->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		}
		else {
			aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) *
					is->video_st->codec->width / is->video_st->codec->height;
		}
		if(aspect_ratio <= 0.0) {
			aspect_ratio = (float)is->video_st->codec->width /
					(float)is->video_st->codec->height;
		}
		h = screen->h;
		w = ((int)rint(h * aspect_ratio)) & -3;
		if(w > screen->w) {
			w = screen->w;
			h = ((int)rint(w / aspect_ratio)) & -3;
		}
		x = (screen->w - w) / 2;
		y = (screen->h - h) / 2;

		if(is->is_small) {
			y = 0;
			h = screen->h/2;
		}
		else {
			y = screen->h/2;
			h = screen->h/2;
		}
		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;
		if(is_multi_videos) {
			SDL_DisplayYUVOverlay(vp->bmp, &rect);
		}
		else {
			if(is->flag_sound == 1 && !is->is_small) {
				rect.h = screen->h;
				rect.y=0;

				SDL_DisplayYUVOverlay(vp->bmp, &rect);
			}
			if(is->flag_sound == 2 && is->is_small) {
				rect.h = screen->h;
				SDL_DisplayYUVOverlay(vp->bmp, &rect);
			}
		}
	}
}

void video_refresh_timer(void *userdata) {

	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if(is->video_st) {
		if(is->pictq_size == 0) {
			schedule_refresh(is, 1);
		}
		else {
			vp = &is->pictq[is->pictq_rindex];

			is->video_current_pts = vp->pts;
			is->video_current_pts_time = av_gettime();

			delay = vp->pts - is->frame_last_pts; /* the pts from last time */
			if(delay <= 0 || delay >= 1.0) {
				/* if incorrect delay, use previous one */
				delay = is->frame_last_delay;
			}
			/* save for next time */
			is->frame_last_delay = delay;
			is->frame_last_pts = vp->pts;

			/* update delay to sync to audio if not master source */
			if(is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
				ref_clock = get_master_clock(is);
				diff = vp->pts - ref_clock;

				/* Skip or repeat the frame. Take delay into account
	   			FFPlay still doesn't "know if this is the best guess." */
				sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
				if(fabs(diff) < AV_NOSYNC_THRESHOLD) {
					if(diff <= -sync_threshold) {
						delay = 0;
					}
					else if(diff >= sync_threshold) {
						delay = 2 * delay;
					}
				}
			}
			is->frame_timer += delay;
			/* computer the REAL delay */
			actual_delay = is->frame_timer - (av_gettime() / 1000000.0);
			if(actual_delay < 0.010) {
				/* Really it should skip the picture instead */
				actual_delay = 0.010;
			}
			schedule_refresh(is, (int)(actual_delay * 1000 + 0.5));

			/* show the picture! */
			video_display(is);

			/* update queue for next picture! */
			if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->pictq_rindex = 0;
			}

			SDL_LockMutex(is->pictq_mutex);
			is->pictq_size--;
			SDL_CondSignal(is->pictq_cond);
			SDL_UnlockMutex(is->pictq_mutex);
		}
	}
	else schedule_refresh(is, 100);
}

void alloc_picture(void *userdata) {

	VideoState *is = (VideoState *)userdata;
	VideoPicture *vp;

	vp = &is->pictq[is->pictq_windex];
	if(vp->bmp) {
		// we already have one make another, bigger/smaller
		SDL_FreeYUVOverlay(vp->bmp);
	}
	// Allocate a place to put our YUV image on that screen
	vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,is->video_st->codec->height,SDL_YV12_OVERLAY,screen);
	vp->width = is->video_st->codec->width;
	vp->height = is->video_st->codec->height;

	SDL_LockMutex(is->pictq_mutex);
	vp->allocated = 1;
	SDL_CondSignal(is->pictq_cond);
	SDL_UnlockMutex(is->pictq_mutex);
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts) {

	VideoPicture *vp;
	//int dst_pix_fmt;
	AVPicture pict;

	/* wait until we have space for a new pic */
	SDL_LockMutex(is->pictq_mutex);
	while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
		SDL_CondWait(is->pictq_cond, is->pictq_mutex);
	}
	SDL_UnlockMutex(is->pictq_mutex);

	if(is->quit) return -1;

	// windex is set to 0 initially
	vp = &is->pictq[is->pictq_windex];

	/* allocate or resize the buffer! */
	if(!vp->bmp || vp->width != is->video_st->codec->width || vp->height != is->video_st->codec->height) {
		SDL_Event event;

		vp->allocated = 0;
		/* we have to do it in the main thread */
		event.type = FF_ALLOC_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);

		/* wait until we have a picture allocated */
		SDL_LockMutex(is->pictq_mutex);
		while(!vp->allocated && !is->quit) {
			SDL_CondWait(is->pictq_cond, is->pictq_mutex);
		}
		SDL_UnlockMutex(is->pictq_mutex);
		if(is->quit) {
			return -1;
		}
	}
	/* We have a place to put our picture on the queue */
	/* If we are skipping a frame, do we set this to null but still return vp->allocated = 1? */

	if(vp->bmp) {
		SDL_LockYUVOverlay(vp->bmp);

		//dst_pix_fmt = PIX_FMT_YUV420P;
		/* point pict at the queue */

		pict.data[0] = vp->bmp->pixels[0];
		pict.data[1] = vp->bmp->pixels[2];
		pict.data[2] = vp->bmp->pixels[1];

		pict.linesize[0] = vp->bmp->pitches[0];
		pict.linesize[1] = vp->bmp->pitches[2];
		pict.linesize[2] = vp->bmp->pitches[1];
		// Convert the image into YUV format that SDL uses
		sws_scale
		(
				is->sws_ctx,
				(uint8_t const * const *)pFrame->data,
				pFrame->linesize,
				0,
				is->video_st->codec->height,
				pict.data,
				pict.linesize
		);
		/* black and white is now handled with rgb (toRGB function)
		if(is->color_flag == 1) {
        		memset(vp->bmp->pixels[1], 128, vp->height*vp->width/2);
        		memset(vp->bmp->pixels[2], 128, vp->height*vp->width/2);
    		}
		 */
		if(is->color_flag == 5) {
			memset(vp->bmp->pixels[2], 100, vp->height*vp->width/2);

		}
		SDL_UnlockYUVOverlay(vp->bmp);
		vp->pts = pts;

		/* now we inform our display thread that we have a pic ready */
		if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			is->pictq_windex = 0;
		}
		SDL_LockMutex(is->pictq_mutex);
		is->pictq_size++;
		SDL_UnlockMutex(is->pictq_mutex);
	}
	return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts) {

	double frame_delay;

	if(pts != 0) {
		/* if we have pts, set video clock to it */
		is->video_clock = pts;
	}
	else {
		/* if we aren't given a pts, set it to the clock */
		pts = is->video_clock;
	}
	/* update the video clock */
	frame_delay = av_q2d(is->video_st->codec->time_base);
	/* if we are repeating a frame, adjust clock accordingly */
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;
	if(is_fast) pts/=2;
	return pts;
}

uint64_t global_video_pkt_pts = AV_NOPTS_VALUE;

/* These are called whenever we allocate a frame
 * buffer. We use this to store the global_pts in
 * a frame at the time it is allocated.
 */
int our_get_buffer(struct AVCodecContext *c, AVFrame *pic) {

	int ret = avcodec_default_get_buffer(c, pic);
	uint64_t *pts = av_malloc(sizeof(uint64_t));
	*pts = global_video_pkt_pts;
	pic->opaque = pts;
	return ret;
}

void our_release_buffer(struct AVCodecContext *c, AVFrame *pic) {

	if(pic) av_freep(&pic->opaque);
	avcodec_default_release_buffer(c, pic);
}

int file_exist(char *filename) {

	struct stat buffer;   
	return (!stat(filename, &buffer));
}

void gen_random(char *s, const int len) {

	static const char alphanum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	for (int i = 0; i < len; ++i)
		s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
	s[len] = 0;
}

void savePicture(VideoState* is, AVFrame* pFrame) {

	char name[15];
	FILE *pFile;
	int  y;
	gen_random(name,10);
	strcat(name,".ppm");

	while (file_exist(name)) {
		gen_random(name,10);
		strcat(name,".ppm");
	}

	// Open file
	pFile=fopen(name, "wb");
	if(pFile==NULL) return;

	// Write header
	fprintf(pFile, "P6\n%d %d\n255\n", is->video_st->codec->width, is->video_st->codec->height);

	// Write pixel data
	for(y=0; y<is->video_st->codec->height; y++)
		fwrite(pFrame->data[0]+y*pFrame->linesize[0], 1, is->video_st->codec->width*3, pFile);

	// Close file
	fclose(pFile);
	if (file_exist(name))
		printf("Screenshot %s has been successfully saved!\n",name);
	else
		fprintf(stderr,"Screenshot has been failed to save\n");
}

void toRGB(void *arguments) {

	VideoState *is = (VideoState*)arguments;
	int rgb_1,rgb_2,numBytes, i;
	AVFrame *pFrameRGB = NULL, *pFrame = NULL;
	uint8_t *buffer = NULL;
	struct SwsContext *sws_ctx = NULL, *sws_ctx_2=NULL;

	// Allocate video frame
	pFrameRGB =  av_frame_alloc();
	if(pFrameRGB==NULL) pthread_exit(NULL);
	AVCodecContext *pCodecCtx =  is->video_st->codec;
	// Determine required buffer size and allocate buffer
	numBytes=avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width,pCodecCtx->height);
	buffer=(uint8_t *)av_malloc(numBytes*sizeof(uint8_t));
	avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24,pCodecCtx->width, pCodecCtx->height);

	//context of RGB
	sws_ctx = sws_getContext(pCodecCtx->width,pCodecCtx->height,
			pCodecCtx->pix_fmt,pCodecCtx->width,pCodecCtx->height,PIX_FMT_RGB24, SWS_BILINEAR,NULL,NULL,NULL);

	//context of YUV	    		
	sws_ctx_2 = sws_getContext(pCodecCtx->width, pCodecCtx->height,
			PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height,pCodecCtx->pix_fmt, SWS_BILINEAR, NULL,NULL,NULL);

	for(;;) {
		SDL_LockMutex(is->colorq_mutex);
		while(is->colorq_size == 0 && !is->quit) {
			SDL_CondWait(is->colorq_cond, is->colorq_mutex);
		}
		SDL_UnlockMutex(is->colorq_mutex);

		if(is->quit) {
			av_frame_free(&pFrameRGB);
			return;
		}
		pFrame = is->colorq[is->colorq_rindex];

		if((is->color_flag > 0 && is->color_flag < 5) || (is->save_picture_flag)) {
			if(is->color_flag==2){
				rgb_1 = 1;
				rgb_2 = 2;
			}
			else if (is->color_flag==3) {
				rgb_1 = 0;
				rgb_2 = 2;
			}
			else {
				rgb_1 = 0;
				rgb_2 = 1;
			}
			sws_scale(sws_ctx,(uint8_t const * const *)pFrame->data,
					pFrame->linesize,0,pCodecCtx->height,pFrameRGB->data,pFrameRGB->linesize);

			if (is->color_flag > 1 && is->color_flag < 5) {
				for (i = 0; i < pCodecCtx->height*pCodecCtx->width*3; i+=3) {
					pFrameRGB->data[0][i+rgb_1] = pFrameRGB->data[0][i+rgb_2] = 0;
				}
			}

			if(is->color_flag == 1) {
				for (i = 0; i < pCodecCtx->height*pCodecCtx->width*3; i+=3) {
					pFrameRGB->data[0][i+2] = pFrameRGB->data[0][i+1] = pFrameRGB->data[0][i] =
						(pFrameRGB->data[0][i] + pFrameRGB->data[0][i+1]+pFrameRGB->data[0][i+2])/ 3;
				}
			}

			if (is->save_picture_flag) {
				is->save_picture_flag = 0;
				savePicture(is,pFrameRGB);
			}

			sws_scale(sws_ctx_2, (uint8_t const * const *) pFrameRGB->data,
					pFrameRGB->linesize, 0, pCodecCtx->height, pFrame->data , pFrame->linesize);
		}
		is->curr_pts = synchronize_video(is, pFrame, is->curr_pts);
		if(queue_picture(is, pFrame, is->curr_pts) < 0) break;
		av_frame_free(&pFrame);

		if(++is->colorq_rindex == VIDEO_PICTURE_QUEUE_SIZE)
			is->colorq_rindex = 0;

		SDL_LockMutex(is->colorq_mutex);
		is->colorq_size--;
		SDL_CondSignal(is->colorq_cond);
		SDL_UnlockMutex(is->colorq_mutex);
	}
}

int video_thread(void *arg) {

	VideoState *is = (VideoState *)arg;
	AVPacket pkt1, *packet = &pkt1;
	int frameFinished;
	AVFrame *pFrame;
	double pts;

	int rc;
	pthread_t thread;
	rc = pthread_create(&thread, NULL, (void*)toRGB, is);
	if (rc) {
		fprintf(stderr,"ERROR; return code from pthread_create() is %d\n", rc);
		exit(-1);
	}

	pFrame = av_frame_alloc();
	for(;;) {
		if(packet_queue_get(&is->videoq, packet, 1) < 0) {
			// means we quit getting packets
			break;
		}
		if(packet->data == flush_pkt.data) {
			avcodec_flush_buffers(is->video_st->codec);
			continue;
		}
		pts = 0;

		// Save global pts to be stored in pFrame in first call
		global_video_pkt_pts = packet->pts;
		// Decode video frame
		avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished,packet);
		if(packet->dts == AV_NOPTS_VALUE && pFrame->opaque && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE)
			pts = *(uint64_t *)pFrame->opaque;
		else if(packet->dts != AV_NOPTS_VALUE) pts = packet->dts;
		else pts = 0;
		pts *= av_q2d(is->video_st->time_base);

		// Did we get a video frame?
		if(frameFinished) {
			SDL_LockMutex(is->colorq_mutex);
			while(is->colorq_size >= VIDEO_PICTURE_QUEUE_SIZE && !is->quit) {
				SDL_CondWait(is->colorq_cond, is->colorq_mutex);
			}
			SDL_UnlockMutex(is->colorq_mutex);

			is->curr_pts = pts;
			is->colorq[is->colorq_windex] = av_frame_clone(pFrame);

			if(++is->colorq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
				is->colorq_windex = 0;
			}
			SDL_LockMutex(is->colorq_mutex);
			is->colorq_size++;
			SDL_CondSignal(is->colorq_cond);
			SDL_UnlockMutex(is->colorq_mutex);
		}

		av_free_packet(packet);
	}
	pthread_exit(NULL);
	av_frame_free(&pFrame);
	return 0;
}


int stream_component_open(VideoState *is, int stream_index) {

	AVFormatContext *pFormatCtx = is->pFormatCtx;
	AVCodecContext *codecCtx = NULL;
	AVCodec *codec = NULL;
	AVDictionary *optionsDict = NULL;

	if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams) return -1;
	// Get a pointer to the codec context for the video stream
	codecCtx = pFormatCtx->streams[stream_index]->codec;

	if(codecCtx->codec_type == AVMEDIA_TYPE_AUDIO) {
		if(!is->is_small) {
			// Set audio settings from codec info
			wanted_spec.freq = codecCtx->sample_rate;
			wanted_spec.format = AUDIO_S16SYS;
			wanted_spec.channels = codecCtx->channels;
			wanted_spec.silence = 0;
			wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
			wanted_spec.callback = audio_callback_manager;
			wanted_spec.userdata = is;

				if(SDL_OpenAudio(&wanted_spec, &spec) < 0) {
					fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
					return -1;
				}
		}
		is->audio_hw_buf_size = spec.size;
	}
	codec = avcodec_find_decoder(codecCtx->codec_id);
	if(!codec || (avcodec_open2(codecCtx, codec, &optionsDict) < 0)) {
		fprintf(stderr, "Unsupported codec!\n");
		return -1;
	}

	switch(codecCtx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		is->audioStream = stream_index;
		is->audio_st = pFormatCtx->streams[stream_index];
		is->audio_buf_size = 0;
		is->audio_buf_index = 0;

		/* averaging filter for audio sync */
		is->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
		is->audio_diff_avg_count = 0;
		/* Correct audio only if larger error than this */
		is->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;

		is->sws_ctx_audio = (void*)swr_alloc();
		if (!is->sws_ctx_audio) {
			fprintf(stderr, "Could not allocate resampler context\n");
			return -1;
		}

		memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
		packet_queue_init(&is->audioq);
		SDL_PauseAudio(0);
		break;
	case AVMEDIA_TYPE_VIDEO:
		is->videoStream = stream_index;
		is->video_st = pFormatCtx->streams[stream_index];

		is->frame_timer = (double)av_gettime() / 1000000.0;
		is->frame_last_delay = 40e-3;
		is->video_current_pts_time = av_gettime();

		packet_queue_init(&is->videoq);
		is->video_tid = SDL_CreateThread(video_thread, is);
		is->sws_ctx =
				sws_getContext
				(
						is->video_st->codec->width,
						is->video_st->codec->height,
						is->video_st->codec->pix_fmt,
						is->video_st->codec->width,
						is->video_st->codec->height,
						AV_PIX_FMT_YUV420P,
						SWS_BILINEAR,
						NULL,
						NULL,
						NULL
				);
		codecCtx->get_buffer2 = (void*)our_get_buffer;
		codecCtx->release_buffer = our_release_buffer;
		break;
	default:
		break;
	}
	return 0;
}

int decode_interrupt_cb(void *opaque) {

	return (global_video_state && global_video_state->quit);
}

int decode_thread(void *arg) {

	VideoState *is = (VideoState *)arg;
	AVFormatContext *pFormatCtx = NULL;
	AVPacket pkt1, *packet = &pkt1;

	AVDictionary *io_dict = NULL;
	AVIOInterruptCB callback;

	int video_index = -1 , audio_index = -1 , i;
	is->videoStream= is->audioStream=-1;
	if(!is->is_small)
		global_video_state = is;
	// will interrupt blocking functions if we quit!
	callback.callback = decode_interrupt_cb;
	callback.opaque = is;
	if (avio_open2(&is->io_context, is->filename, 0, &callback, &io_dict)) {
		fprintf(stderr, "Unable to open I/O for %s\n", is->filename);
		return -1;
	}

	// Open video file
	if(avformat_open_input(&pFormatCtx, is->filename, NULL, NULL)!=0)
		return -1; // Couldn't open file

	is->pFormatCtx = pFormatCtx;

	// Retrieve stream information
	if(avformat_find_stream_info(pFormatCtx, NULL)<0)
		return -1; // Couldn't find stream information

	// Dump information about file onto standard error
	av_dump_format(pFormatCtx, 0, is->filename, 0);

	// Find the first video stream
	for(i=0; i<pFormatCtx->nb_streams; i++) {
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO && video_index < 0) 
			video_index=i;
		if(pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_AUDIO && audio_index < 0) 
			audio_index=i;
	}
	if(audio_index >= 0) 
		stream_component_open(is, audio_index);
	if(video_index >= 0)
		stream_component_open(is, video_index);

	if(is->videoStream < 0 || is->audioStream < 0) {
		fprintf(stderr, "%s: could not open codecs\n", is->filename);
		goto fail;
	}

	// main decode loop

	for(;;) {
		if(is->quit) break;
		// seek stuff goes here
		if(is->seek_req) {
			int stream_index= -1;
			int64_t seek_target = is->seek_pos;

			if     (is->videoStream >= 0) stream_index = is->videoStream;
			else if(is->audioStream >= 0) stream_index = is->audioStream;

			if(stream_index>=0)
				seek_target= av_rescale_q(seek_target, AV_TIME_BASE_Q, pFormatCtx->streams[stream_index]->time_base);
			if(av_seek_frame(is->pFormatCtx, stream_index, seek_target, is->seek_flags) < 0)
				fprintf(stderr, "%s: error while seeking\n", is->pFormatCtx->filename);
			else {
				if(is->audioStream >= 0) {
					packet_queue_flush(&is->audioq);
					packet_queue_put(&is->audioq, &flush_pkt);
				}
				if(is->videoStream >= 0) {
					packet_queue_flush(&is->videoq);
					packet_queue_put(&is->videoq, &flush_pkt);
				}
			}
			is->seek_req = 0;
		}

		if(is->audioq.size > MAX_AUDIOQ_SIZE || is->videoq.size > MAX_VIDEOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}
		if(av_read_frame(is->pFormatCtx, packet) < 0) {
			if(is->pFormatCtx->pb->error == 0) {
				SDL_Delay(100); /* no error; wait for user input */
				continue;
			}
			else break;
		}
		// Is this a packet from the video stream?
		if(packet->stream_index == is->videoStream)
			packet_queue_put(&is->videoq, packet);
		else if(packet->stream_index == is->audioStream)
			packet_queue_put(&is->audioq, packet);
		else
			av_free_packet(packet);
	}
	/* all done - wait for it */
	while(!is->quit)
		SDL_Delay(100);
	fail:
	{
		SDL_Event event;
		event.type = FF_QUIT_EVENT;
		event.user.data1 = is;
		SDL_PushEvent(&event);
	}
	return 0;
}

void stream_seek(VideoState *is, int64_t pos, int rel) {

	if(!is->seek_req) {
		is->seek_pos = pos;
		is->seek_flags = rel < 0 ? AVSEEK_FLAG_BACKWARD : 0;
		is->seek_req = 1;
	}
}

int main(int argc, char *argv[]) {

	SDL_Event       event;
	//double          pts;
	VideoState      *is;
	int width = 640, height = 480;
	is = av_mallocz(sizeof(VideoState));
	is->is2 = av_mallocz(sizeof(VideoState));
	is->is2->is_small = 1;
	is->flag_sound = 1;
	if(argc < 3) {
		fprintf(stderr, "You should insert 2 filenames\n");
		exit(1);
	}
	if(argc < 5)
		printf("The size will be default (640 x 480)\n");
		
	else {
		width = strtol(argv[3],NULL,10);
		height = strtol(argv[4],NULL,10);
	}
	if (width == 0 || height == 0) {
		width = 640;
		height = 480;
	}
	printf("Initializing %s on %d x %d\n",argv[1],width,height);
	// Register all formats and codecs
	av_register_all();

	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}

	// Make a screen to put our video
#ifndef __DARWIN__
	screen = SDL_SetVideoMode(width, height, 0, 0);
#else
	screen = SDL_SetVideoMode(width, height, 24, 0);
#endif
	if(!screen) {
		fprintf(stderr, "SDL: could not set video mode - exiting\n");
		exit(1);
	}

	av_strlcpy(is->filename, argv[1], 1024);
	av_strlcpy(is->is2->filename, argv[2], 1024);

	is->pictq_mutex = SDL_CreateMutex();
	is->pictq_cond = SDL_CreateCond();
	is->colorq_mutex = SDL_CreateMutex();
	is->colorq_cond = SDL_CreateCond();
	schedule_refresh(is,40);

	is->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	is->parse_tid = SDL_CreateThread(decode_thread, is);
	if(!is->parse_tid) {
		av_free(is);
		return -1;
	}

	is->is2->pictq_mutex = SDL_CreateMutex();
	is->is2->pictq_cond = SDL_CreateCond();
	is->is2->colorq_mutex = SDL_CreateMutex();
	is->is2->colorq_cond = SDL_CreateCond();
	schedule_refresh(is->is2,40);

	is->is2->av_sync_type = DEFAULT_AV_SYNC_TYPE;
	is->is2->parse_tid = SDL_CreateThread(decode_thread, is->is2);
	if(!is->is2->parse_tid) {
		av_free(is->is2);
		return -1;
	}

	av_init_packet(&flush_pkt);
	flush_pkt.data = (unsigned char *)"FLUSH";
	for(;;) {
		double incr, pos;

		SDL_WaitEvent(&event);
		switch(event.type) {
		case SDL_KEYDOWN:
			switch(event.key.keysym.sym) {		//Here we defines the keys (the events)
			case SDLK_LEFT:
				incr = -10.0;
				goto do_seek;
			case SDLK_RIGHT:
				incr = 10.0;
				goto do_seek;
			case SDLK_UP:
				incr = 60.0;
				goto do_seek;
			case SDLK_DOWN:
				incr = -60.0;
				goto do_seek;
				do_seek:
				if(global_video_state) {
					pos = get_master_clock(global_video_state);
					pos += incr;
					stream_seek(global_video_state, (int64_t)(pos * AV_TIME_BASE), incr);
				}
				break;
			// black & white event	
			case SDLK_w:
			if (is->color_flag == 1) is->color_flag = is->is2->color_flag = 0;
			else is->color_flag = is->is2->color_flag = 1;
			break;
			// red screen event
			case SDLK_r:
				if (is->color_flag == 2) is->color_flag = is->is2->color_flag = 0;
				else is->color_flag = is->is2->color_flag = 2;
				break;
			// green screen event
			case SDLK_g:
				if (is->color_flag == 3) is->color_flag = is->is2->color_flag = 0;
				else is->color_flag = is->is2->color_flag = 3;
				break;
			// blue screen event
			case SDLK_b:
				if (is->color_flag == 4) is->color_flag = is->is2->color_flag = 0;
				else is->color_flag = is->is2->color_flag = 4;
				break;
			// instant filter layer screen event	
			case SDLK_h:
				if (is->color_flag == 5) is->color_flag = is->is2->color_flag = 0;
				else is->color_flag = is->is2->color_flag = 5;
				break;	
			// clear colors
			case SDLK_c:					//clear all colors
				is->color_flag = is->is2->color_flag = 0;
				break;
			// take screen shot
			case SDLK_x:
				if(is->flag_sound == 1)
					is->save_picture_flag = 1;
				if(is->flag_sound == 2)
					is->is2->save_picture_flag = 1;
				break;
			// play primary audio
			case SDLK_1:
				if(is->flag_sound != 1) {
					is->flag_sound = 1;
					is->is2->flag_sound = 1;
					wanted_spec.freq = is->pFormatCtx->streams[is->audioStream]->codec->sample_rate;
					wanted_spec.channels = is->pFormatCtx->streams[is->audioStream]->codec->channels;
					event.user.data1 = global_video_state = is;
				}
				break;
			// play secondary audio
			case SDLK_2:
				if(is->flag_sound != 2) {
					is->flag_sound = 2;
					is->is2->flag_sound = 2;
					wanted_spec.freq = is->is2->pFormatCtx->streams[is->is2->audioStream]->codec->sample_rate;
					wanted_spec.channels = is->is2->pFormatCtx->streams[is->is2->audioStream]->codec->channels;
					event.user.data1 = global_video_state = is->is2;
				}
				break;
			// mute both audios
			case SDLK_3:
				if(mute != 1) mute = 1;
				else mute = 0;
				break;
			// one full screened video
			case SDLK_o:
				if(is_multi_videos != 0)
					is_multi_videos = 0;
				break;
			// multiple videos
			case SDLK_m:
				if(is_multi_videos != 1)
					is_multi_videos = 1;
				break; 
			//fast forward the 2 videos	
			case SDLK_f:
				if(is_fast != 1) is_fast = 1;
				else is_fast = 0;
				break;		
			// quit video player
			case SDLK_q:
				is->quit = 1;
				break;

			default:
				break;
			}
			break;
			case FF_QUIT_EVENT:
			case SDL_QUIT:
				is->quit = 1;
				/*
				 * If the video has finished playing, then both the picture and
				 * audio queues are waiting for more data.  Make them stop
				 * waiting and terminate normally.
				 */
				SDL_CondSignal(is->audioq.cond);
				SDL_CondSignal(is->videoq.cond);
				SDL_Quit();
				exit(0);
				break;
			case FF_ALLOC_EVENT:
				alloc_picture(event.user.data1);
				break;
			case FF_REFRESH_EVENT:
				video_refresh_timer(event.user.data1);
				break;
			default:
				break;
		}
	}
	return 0;
}
