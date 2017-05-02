#ifndef _INCLUDE_FFMPEG_H_
#define _INCLUDE_FFMPEG_H_

#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <stdint.h>

enum TIMELAPSE_TYPE {
    TIMELAPSE_NONE,         /* No timelapse, regular processing */
    TIMELAPSE_APPEND,       /* Use append version of timelapse */
    TIMELAPSE_NEW           /* Use create new file version of timelapse */
};

#ifdef HAVE_FFMPEG

#include <errno.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavdevice/avdevice.h>

#if (LIBAVFORMAT_VERSION_MAJOR >= 56)
#define MY_PIX_FMT_YUV420P   AV_PIX_FMT_YUV420P
#define MY_PIX_FMT_YUVJ420P  AV_PIX_FMT_YUVJ420P
#define MyPixelFormat AVPixelFormat
#else  //Old ffmpeg pixel formats
#define MY_PIX_FMT_YUV420P   PIX_FMT_YUV420P
#define MY_PIX_FMT_YUVJ420P  PIX_FMT_YUVJ420P
#define MyPixelFormat PixelFormat
#endif  //Libavformat >= 56

#endif // HAVE_FFMPEG

struct ffmpeg {
#ifdef HAVE_FFMPEG
    AVFormatContext *oc;
    AVStream *video_st;
    AVCodecContext *ctx_codec;
    AVCodec *codec;
    AVPacket pkt;
    AVFrame *picture;       /* contains default image pointers */
    AVDictionary *opts;
    AVFormatContext *rtsp_format_context;
    enum AVCodecID passthru_codec_id;
    AVRational passthru_time_base;
#endif
    int width;
    int height;
    enum TIMELAPSE_TYPE tlapse;
    int fps;
    int bps;
    char *filename;
    int vbr;
    const char *codec_name;
    int64_t last_pts;
    int test_mode;
    int gop_cnt;
    struct timeval start_time;
    int passthru_started;
    int passthru_frames_lost;
    int64_t passthru_ts_offset;
    int64_t passthru_last_serial;
};


#ifdef HAVE_FFMPEG

AVFrame *my_frame_alloc(void);
void my_frame_free(AVFrame *frame);
void my_packet_unref(AVPacket *pkt);
void my_avcodec_close(AVCodecContext *codec_context);
int my_image_get_buffer_size(enum MyPixelFormat pix_fmt, int width, int height);
int my_image_copy_to_buffer(AVFrame *frame,uint8_t *buffer_ptr,enum MyPixelFormat pix_fmt,int width,int height,int dest_size);
int my_image_fill_arrays(AVFrame *frame,uint8_t *buffer_ptr,enum MyPixelFormat pix_fmt,int width,int height);

struct packet_buff *ffmpeg_packet_buffer_new(void);
void ffmpeg_packet_buffer_add(struct packet_buff *buffer, AVPacket *pkt);
void ffmpeg_packet_buffer_copy(struct packet_buff *dest, struct packet_buff *src);
void ffmpeg_packet_buffer_move(struct packet_buff *dest, struct packet_buff *src);
void ffmpeg_packet_buffer_clear(struct packet_buff *buffer);
void ffmpeg_packet_buffer_unref(struct packet_buff *buffer);
int  ffmpeg_packet_buffer_count(struct packet_buff *buffer);
void ffmpeg_packet_buffer_free(struct packet_buff *buffer);

#endif /* HAVE_FFMPEG */

void ffmpeg_global_init(void);
void ffmpeg_global_deinit(void);
void ffmpeg_avcodec_log(void *, int, const char *, va_list);

int ffmpeg_open(struct ffmpeg *ffmpeg);
int ffmpeg_put_image(struct ffmpeg *ffmpeg, unsigned char *image, const struct timeval *tv1);
int ffmpeg_put_packets(struct ffmpeg *ffmpeg, struct packet_buff *buffer);
void ffmpeg_close(struct ffmpeg *ffmpeg);

#endif /* _INCLUDE_FFMPEG_H_ */
