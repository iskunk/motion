/***********************************************************
 *  In the top section are the functions that are used
 *  when processing the RTSP camera feed.  Since these functions
 *  are internal to the RTSP module, and many require FFmpeg
 *  structures in their declarations, they are within the
 *  HAVE_FFMPEG block that eliminates them entirely when
 *  FFmpeg is not present.
 *
 *  The functions:
 *      netcam_setup_rtsp
 *      netcam_connect_rtsp
 *      netcam_shutdown_rtsp
 *      netcam_next_rtsp
 *  are called from netcam.c therefore must be defined even
 *  if FFmpeg is not present.  They must also not have FFmpeg
 *  structures in the declarations.  Simple error
 *  messages are raised if called when no FFmpeg is found.
 *
 ***********************************************************/

#include "rotate.h"    /* already includes motion.h */
#include "netcam_rtsp.h"

#ifdef HAVE_FFMPEG

#include "ffmpeg.h"

static int netcam_rtsp_resize(netcam_context_ptr netcam);
static int netcam_rtsp_open_sws(netcam_context_ptr netcam);

/**
 * netcam_check_pixfmt
 *
 * Determine whether pix_format is YUV420P
 */
static int netcam_check_pixfmt(netcam_context_ptr netcam){
    int retcd;

    retcd = -1;

    if ((netcam->rtsp->codec_context->pix_fmt == MY_PIX_FMT_YUV420P) ||
        (netcam->rtsp->codec_context->pix_fmt == MY_PIX_FMT_YUVJ420P)) retcd = 0;

    return retcd;

}
/**
 * netcam_rtsp_null_context
 *
 * Null all the context
 */
static void netcam_rtsp_null_context(netcam_context_ptr netcam){

    netcam->rtsp->swsctx         = NULL;
    netcam->rtsp->swsframe_in    = NULL;
    netcam->rtsp->swsframe_out   = NULL;
    netcam->rtsp->frame          = NULL;
    netcam->rtsp->codec_context  = NULL;
    netcam->rtsp->format_context = NULL;
    netcam->rtsp->resize_buffer  = NULL;

    netcam->rtsp->active = 0;
}
/**
 * netcam_rtsp_close_context
 *
 * Close all the context that could be open
 */
static void netcam_rtsp_close_context(netcam_context_ptr netcam){

    if (netcam->rtsp->swsctx       != NULL) sws_freeContext(netcam->rtsp->swsctx);
    if (netcam->rtsp->swsframe_in  != NULL) my_frame_free(netcam->rtsp->swsframe_in);
    if (netcam->rtsp->swsframe_out != NULL) my_frame_free(netcam->rtsp->swsframe_out);
    if (netcam->rtsp->frame        != NULL) my_frame_free(netcam->rtsp->frame);
    if (netcam->rtsp->codec_context    != NULL) my_avcodec_close(netcam->rtsp->codec_context);
    if (netcam->rtsp->format_context   != NULL) avformat_close_input(&netcam->rtsp->format_context);
    if (netcam->rtsp->resize_buffer    != NULL) av_free(netcam->rtsp->resize_buffer);

    netcam_rtsp_null_context(netcam);
}

static int rtsp_decode_video(AVPacket *packet, AVFrame *frame, AVCodecContext *ctx_codec){

#if (LIBAVFORMAT_VERSION_MAJOR >= 58) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR >= 41))
    int retcd;
    char errstr[128];

    if (packet) {
        retcd = avcodec_send_packet(ctx_codec, packet);
        assert(retcd != AVERROR(EAGAIN));
        if (retcd < 0 && retcd != AVERROR_EOF){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error sending packet to codec: %s", errstr);
            return -1;
        }
    }

    retcd = avcodec_receive_frame(ctx_codec, frame);

    /* Ensure that we get a frame from every "key" packet
     */
    assert(!packet || retcd != AVERROR(EAGAIN) || !(packet->flags & AV_PKT_FLAG_KEY));

    if (retcd == AVERROR(EAGAIN)) return 0;
    if (retcd < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error receiving frame from codec: %s", errstr);
        return -1;
    }

    /* Ensure that "have an I-frame" corresponds exactly with
     * "have a packet, and its 'key' flag is set"
     */
    assert((frame->pict_type == AV_PICTURE_TYPE_I) ==
           (packet && packet->flags & AV_PKT_FLAG_KEY));

    return 1;

#else

    AVPacket empty_packet;
    int retcd;
    int check = 0;
    char errstr[128];

    if (!packet) {
        av_init_packet(&empty_packet);
        empty_packet.data = NULL;
        empty_packet.size = 0;
        packet = &empty_packet;
    }

    retcd = avcodec_decode_video2(ctx_codec, frame, &check, packet);
    if (retcd < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error decoding packet: %s",errstr);
        return -1;
    }
    if (check == 0 || retcd == 0) return 0;
    return 1;

#endif

}
/**
 * rtsp_decode_packet
 *
 * This routine takes in the packet from the read and decodes it into
 * the frame.  It then takes the frame and copies it into the netcam
 * buffer
 *
 * Parameters:
 *      packet    The packet that was read from av_read, or NULL
 *      buffer    The buffer that is the final destination
 *      frame     The frame into which we decode the packet
 *
 *
 * Returns:
 *      Error      Negative value
 *      No result  0(zero)
 *      Success    The size of the frame decoded
 */
static int rtsp_decode_packet(AVPacket *packet, netcam_buff_ptr buffer, AVFrame *frame, AVCodecContext *ctx_codec){

    int frame_size;
    int retcd;

    retcd = rtsp_decode_video(packet, frame, ctx_codec);
    if (retcd <= 0) return retcd;

    frame_size = my_image_get_buffer_size(ctx_codec->pix_fmt, ctx_codec->width, ctx_codec->height);

    netcam_check_buffsize(buffer, frame_size);

    retcd = my_image_copy_to_buffer(frame, (uint8_t *)buffer->ptr,ctx_codec->pix_fmt,ctx_codec->width,ctx_codec->height, frame_size);
    if (retcd < 0) {
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error decoding video packet: Copying to buffer");
        return -1;
    }

    buffer->used = frame_size;

    return frame_size;
}

/**
 * netcam_open_codec
 *
 * This routine opens the codec context for the indicated stream
 *
 * Parameters:
 *      stream_idx  The index of the stream that was found as "best"
 *      fmt_ctx     The format context that was created upon opening the stream
 *      type        The type of media type (This is a constant)
 *
 *
 * Returns:
 *      Failure    Error code from FFmpeg (Negative number)
 *      Success    0(Zero)
 */
static int netcam_open_codec(netcam_context_ptr netcam){

    int retcd;
    char errstr[128];
    AVStream *st;
    AVCodec *decoder = NULL;

    retcd = av_find_best_stream(netcam->rtsp->format_context, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Could not find stream in input!: %s",errstr);
        return (retcd < 0) ? retcd : -1;
    }
    netcam->rtsp->video_stream_index = retcd;
    st = netcam->rtsp->format_context->streams[netcam->rtsp->video_stream_index];

#if (LIBAVFORMAT_VERSION_MAJOR >= 58) || ((LIBAVFORMAT_VERSION_MAJOR == 57) && (LIBAVFORMAT_VERSION_MINOR >= 41))
    decoder = avcodec_find_decoder(st->codecpar->codec_id);
    if (decoder == NULL) {
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to find codec!");
        return -1;
    }

    netcam->rtsp->codec_context = avcodec_alloc_context3(decoder);
    if (netcam->rtsp->codec_context == NULL) {
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to allocate decoder!");
        return -1;
    }

    if ((retcd = avcodec_parameters_to_context(netcam->rtsp->codec_context, st->codecpar)) < 0) {
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to copy decoder parameters!: %s", errstr);
        return -1;
    }

#else

    netcam->rtsp->codec_context = st->codec;
    decoder = avcodec_find_decoder(netcam->rtsp->codec_context->codec_id);
    if (decoder == NULL) {
         MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to find codec!");
         return -1;
     }

#endif

    /* Force low delay in passthru mode, because we need to be able
     * to tell exactly which packets yield a given frame
     *
     * FIXME: set only for passthru mode
     */
    netcam->rtsp->codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;

    retcd = avcodec_open2(netcam->rtsp->codec_context, decoder, NULL);
    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        av_strerror(retcd, errstr, sizeof(errstr));
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to open codec!: %s", errstr);
        return (retcd < 0) ? retcd : -1;
    }

    return 0;
}

/**
* rtsp_new_context
*
*      Create a new RTSP context structure.
*
* Parameters
*
*       None
*
* Returns:     Pointer to the newly-created structure, NULL if error.
*
*/
struct rtsp_context *rtsp_new_context(void){
    struct rtsp_context *ret;

    /* Note that mymalloc will exit on any problem. */
    ret = mymalloc(sizeof(struct rtsp_context));

    memset(ret, 0, sizeof(struct rtsp_context));

    return ret;
}
/**
* netcam_interrupt_rtsp
*
*    This function is called during the FFmpeg blocking functions.
*    These include the opening of the format context as well as the
*    reading of the packets from the stream.  Since this is called
*    during all blocking functions, the process uses the readingframe
*    flag to determine whether to timeout the process.
*
* Parameters
*
*       ctx   We pass in the netcam context to use it to look for the
*             readingframe flag as well as the time that we started
*             the read attempt.
*
* Returns:
*       Failure    -1(which triggers an interrupt)
*       Success     0(zero which indicates to let process continue)
*
*/
static int netcam_interrupt_rtsp(void *ctx){
    netcam_context_ptr netcam = (netcam_context_ptr)ctx;
    struct rtsp_context *rtsp = netcam->rtsp;

    if (netcam->finish) {
        /* netcam_cleanup() wants us to stop */
        if (rtsp) rtsp->interrupted = 1;
        return 1;
    }

    if (rtsp->status == RTSP_CONNECTED) {
        return 0;
    } else if (rtsp->status == RTSP_READINGIMAGE) {
        struct timeval interrupttime;
        if (gettimeofday(&interrupttime, NULL) < 0) {
            MOTION_LOG(WRN, TYPE_NETCAM, SHOW_ERRNO, "%s: get interrupt time failed");
        }
        if ((interrupttime.tv_sec - rtsp->startreadtime.tv_sec ) > 10){
            MOTION_LOG(WRN, TYPE_NETCAM, NO_ERRNO, "%s: Camera timed out for %s", rtsp->netcam_url);
            rtsp->interrupted = 1;
            return 1;
        } else{
            return 0;
        }
    } else {
        /* This is for NOTCONNECTED and RECONNECTING status.  We give these
         * options more time because all the ffmpeg calls that are inside the
         * rtsp_connect function will use the same start time.  Otherwise we
         * would need to reset the time before each call to a ffmpeg function.
        */
        struct timeval interrupttime;
        if (gettimeofday(&interrupttime, NULL) < 0) {
            MOTION_LOG(WRN, TYPE_NETCAM, SHOW_ERRNO, "%s: get interrupt time failed");
        }
        if ((interrupttime.tv_sec - rtsp->startreadtime.tv_sec ) > 30){
            MOTION_LOG(WRN, TYPE_NETCAM, NO_ERRNO, "%s: Camera timed out for %s", rtsp->netcam_url);
            rtsp->interrupted = 1;
            return 1;
        } else{
            return 0;
        }
    }

    //should not be possible to get here
    return 0;
}
/**
* netcam_read_rtsp_image
*
*    This function reads the packet from the camera.
*    It is called extensively so only absolutely essential
*    functions and allocations are performed.
*
* Parameters
*
*       netcam  The netcam context to read from
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
int netcam_read_rtsp_image(netcam_context_ptr netcam){
    struct timeval     curtime;
    netcam_buff_ptr    buffer;
    AVPacket           packet;
    int                size_decoded;
    int                video_packet_count = 0;
    int64_t            last_video_dts = 0;

    assert(netcam->rtsp->codec_context != NULL);

    /* Point to our working buffer. */
    buffer = netcam->receiving;
    buffer->used = 0;
    buffer->packet_serial = netcam->rtsp->cur_packet_serial;
    buffer->packet_count = 0;
    buffer->pts = AV_NOPTS_VALUE;

    assert(ffmpeg_packet_buffer_count(buffer->frame_packets) == 0);

    av_init_packet(&packet);
    packet.data = NULL;
    packet.size = 0;

    if (gettimeofday(&curtime, NULL) < 0) {
        MOTION_LOG(ERR, TYPE_NETCAM, SHOW_ERRNO, "%s: gettimeofday");
    }
    netcam->rtsp->startreadtime = curtime;
    netcam->rtsp->interrupted = 0;
    netcam->rtsp->status = RTSP_READINGIMAGE;

    /* First, check whether the codec has any frames ready to go
     * before we feed it new packets
     */
    size_decoded = rtsp_decode_packet(NULL, buffer, netcam->rtsp->frame, netcam->rtsp->codec_context);

    while (size_decoded == 0 && av_read_frame(netcam->rtsp->format_context, &packet) >= 0) {
        buffer->packet_count++;

        /* Re-purpose the .pos field to store a packet serial number
         */
        packet.pos = netcam->rtsp->cur_packet_serial++;
#if 1
fprintf(stderr, "packet: n=%d dts=%09ld pts=%09ld dur=%05d ser=%06ld [%c] IN\n",
  packet.stream_index,
  packet.dts,
  packet.pts,
  (int)packet.duration,
  packet.pos,
  packet.flags & AV_PKT_FLAG_KEY ? 'K' : ' '
);
#endif

        /* This seems to occur for the first packet received after
         * reconnecting to a camera
         */
        if (packet.dts == AV_NOPTS_VALUE && packet.pts == AV_NOPTS_VALUE) {
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Packet has invalid timestamps, setting to zero");
            packet.dts = packet.pts = 0;
        }

        /* Some cameras will occasionally send a packet with an
         * out-of-order decompression timestamp. We'll let this slide
         * if it's in the first few packets, but otherwise we bail out.
         * Correcting these timestamps on-the-fly is too hard to do here
         * and camera manufacturers really ought to know better :p
         */
        if (packet.dts <= netcam->rtsp->last_dts[packet.stream_index]) {
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Received packet with non-increasing dts!");
            if (packet.pos >= 10)
                break;
        }
        netcam->rtsp->last_dts[packet.stream_index] = packet.dts;

        if (packet.stream_index == netcam->rtsp->video_stream_index) {
            video_packet_count++;
            last_video_dts = packet.dts;
            size_decoded = rtsp_decode_packet(&packet, buffer, netcam->rtsp->frame, netcam->rtsp->codec_context);
        }

        ffmpeg_packet_buffer_add(buffer->frame_packets, &packet);

        av_init_packet(&packet);
        packet.data = NULL;
        packet.size = 0;
#if 0 /* iskunk: not freeing the packet here when in passthru mode */
        my_packet_unref(&packet);
#endif
    }
    netcam->rtsp->status = RTSP_CONNECTED;

    if ((size_decoded <= 0) || (netcam->rtsp->interrupted == 1)) {
        // something went wrong, end of stream? interrupted?
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    /* Codec delay is undesirable in passthru mode, because it
     * hampers our ability to correlate packets and frames
     */
    assert(video_packet_count == 0 ||
           netcam->rtsp->frame->pkt_dts == last_video_dts);

    buffer->pts = (netcam->rtsp->frame->pts != AV_NOPTS_VALUE) ?
        netcam->rtsp->frame->pts : netcam->rtsp->frame->pkt_dts;
#if 1
assert(netcam->rtsp->frame->repeat_pict == 0);
fprintf(stderr, " frame: pts=%09ld pkt_dts=%09ld count=%d type=%c [%c]\n",
  netcam->rtsp->frame->pts,
  netcam->rtsp->frame->pkt_dts,
  buffer->packet_count,
  av_get_picture_type_char(netcam->rtsp->frame->pict_type),
  netcam->rtsp->frame->key_frame ? 'K' : ' ');
#endif

    /* Add placeholder packet for subtitle overlay
     */
#if 0
    av_init_packet(&packet);
    packet.pos = -1;
    ffmpeg_packet_buffer_add(buffer->frame_packets, &packet);
#endif

    if ((netcam->width  != (unsigned)netcam->rtsp->codec_context->width) ||
        (netcam->height != (unsigned)netcam->rtsp->codec_context->height) ||
        (netcam_check_pixfmt(netcam) != 0) ){
        if (netcam_rtsp_resize(netcam) < 0)
          return -1;
    }

    netcam_image_read_complete(netcam);

    return 0;
}
/**
* netcam_rtsp_resize_ntc
*
*    This function notifies the user of the need to transcode
*    the netcam image which uses a lot of CPU resources
*
* Parameters
*
*       netcam  The netcam context to read from
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
static int netcam_rtsp_resize_ntc(netcam_context_ptr netcam){

    int diff_dims, diff_pixfmt;

    assert(netcam->rtsp->codec_context != NULL);

    diff_dims =
        netcam->width  != (unsigned)netcam->rtsp->codec_context->width ||
        netcam->height != (unsigned)netcam->rtsp->codec_context->height;

    diff_pixfmt = netcam_check_pixfmt(netcam) != 0;

    if (diff_dims || diff_pixfmt) {
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: ");
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: ****************************************************************");
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: The network camera is sending pictures in a different");
    }

    if (diff_dims) {
        if (diff_pixfmt) {
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: size than specified in the config and also a ");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: different picture format.  The picture is being");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: transcoded to YUV420P and into the size requested");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: in the config file.  If possible change netcam to");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: be in YUV420P format and the size requested in the");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: config to possibly lower CPU usage.");
        } else {
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: size than specified in the configuration file.");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: The picture is being transcoded into the size ");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: requested in the configuration.  If possible change");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: netcam or configuration to indicate the same size");
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: to possibly lower CPU usage.");
        }

        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Netcam: %d x %d => Config: %d x %d"
        ,netcam->rtsp->codec_context->width,netcam->rtsp->codec_context->height
        ,netcam->width,netcam->height);
    } else if (diff_pixfmt) {
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: format than YUV420P.  The image sent is being ");
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: trancoded to YUV420P.  If possible change netcam ");
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: picture format to YUV420P to possibly lower CPU usage.");
    }

    if (diff_dims || diff_pixfmt) {
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: ****************************************************************");
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: ");
    }

    return 0;
}
/**
* netcam_rtsp_open_context
*
*    This function opens the format context for the camera.
*
* Parameters
*
*       netcam  The netcam context to read from
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
static int netcam_rtsp_open_context(netcam_context_ptr netcam){

    int  retcd;
    char errstr[128];
    char optsize[10], optfmt[8], optfps[5];
    int i;

    if (netcam->rtsp->path == NULL) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Null path passed to connect");
        }
        return -1;
    }

    netcam->rtsp->cur_packet_serial = 0;

    // open the network connection
    AVDictionary *opts = 0;
    netcam->rtsp->format_context = avformat_alloc_context();
    netcam->rtsp->format_context->interrupt_callback.callback = netcam_interrupt_rtsp;
    netcam->rtsp->format_context->interrupt_callback.opaque = netcam;

    netcam->rtsp->interrupted = 0;
    if (gettimeofday(&netcam->rtsp->startreadtime, NULL) < 0) {
        MOTION_LOG(ERR, TYPE_NETCAM, SHOW_ERRNO, "%s: gettimeofday");
    }

    if (strncmp(netcam->rtsp->path, "http", 4) == 0 ){
        netcam->rtsp->format_context->iformat = av_find_input_format("mjpeg");
    } else if (strncmp(netcam->rtsp->path, "rtsp", 4) == 0 ){
        if (netcam->cnt->conf.rtsp_uses_tcp) {
            av_dict_set(&opts, "rtsp_transport", "tcp", 0);
            if (netcam->rtsp->status == RTSP_NOTCONNECTED)
                MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Using tcp transport");
        } else {
            av_dict_set(&opts, "rtsp_transport", "udp", 0);
            av_dict_set(&opts, "max_delay", "500000", 0);  //100000 is the default
            if (netcam->rtsp->status == RTSP_NOTCONNECTED)
                MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Using udp transport");
        }
    } else {
        netcam->rtsp->format_context->iformat = av_find_input_format("video4linux2");

        if (netcam->cnt->conf.v4l2_palette == 8) {
            sprintf(optfmt, "%s","mjpeg");
            av_dict_set(&opts, "input_format", optfmt, 0);
        } else if (netcam->cnt->conf.v4l2_palette == 21){
            sprintf(optfmt, "%s","h264");
            av_dict_set(&opts, "input_format", optfmt, 0);
        } else{
            sprintf(optfmt, "%s","default");
        }

        sprintf(optfps, "%d",netcam->cnt->conf.frame_limit);
        av_dict_set(&opts, "framerate", optfps, 0);

        sprintf(optsize, "%dx%d",netcam->cnt->conf.width,netcam->cnt->conf.height);
        av_dict_set(&opts, "video_size", optsize, 0);

        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: v4l2 input_format %s",optfmt);
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: v4l2 framerate %s", optfps);
            MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: v4l2 video_size %s", optsize);
        }
     }

    retcd = avformat_open_input(&netcam->rtsp->format_context, netcam->rtsp->path, NULL, &opts);
    av_dict_free(&opts);
    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to open input(%s): %s", netcam->rtsp->netcam_url, errstr);
        }
        //The format context gets freed upon any error from open_input.
        if (retcd == 0)
            netcam_rtsp_close_context(netcam);
        return (retcd < 0) ? retcd : -1;
    }

    // fill out stream information
    retcd = avformat_find_stream_info(netcam->rtsp->format_context, NULL);
    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        if (retcd < 0 && netcam->rtsp->status == RTSP_NOTCONNECTED) {
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to find stream info: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    assert(netcam->rtsp->format_context->nb_streams <= MAX_STREAMS);
    for (i = 0; i < netcam->rtsp->format_context->nb_streams; i++)
        netcam->rtsp->last_dts[i] = INT64_MIN;

    /* there is no way to set the avcodec thread names, but they inherit
     * our thread name - so temporarily change our thread name to the
     * desired name */
    {
        char newtname[16];
        char curtname[16] = "unknown";
#if (!defined(BSD) || defined(__APPLE__))
        pthread_getname_np(pthread_self(), curtname, sizeof(curtname));
#endif
        snprintf(newtname, sizeof(newtname), "av%d%s%s",
                 netcam->cnt->threadnr,
                 netcam->cnt->conf.camera_name ? ":" : "",
                 netcam->cnt->conf.camera_name ? netcam->cnt->conf.camera_name : "");
        MOTION_PTHREAD_SETNAME(newtname);

    retcd = netcam_open_codec(netcam);

        MOTION_PTHREAD_SETNAME(curtname);
    }

    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to open codec context: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    /* Save information needed for passthru mode
     */
    if (netcam->rtsp->rtsp_info == NULL)
        netcam->rtsp->rtsp_info = ffmpeg_rtsp_info_new(netcam->rtsp->format_context, netcam->rtsp->video_stream_index);

    assert(netcam->rtsp->codec_context != NULL);

    if (netcam->rtsp->codec_context->width <= 0 ||
        netcam->rtsp->codec_context->height <= 0)
    {
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Camera image size is invalid");
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    if (netcam->rtsp->codec_context->pix_fmt == AV_PIX_FMT_NONE) {
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Camera pixel format is undetermined");
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    netcam->rtsp->frame = my_frame_alloc();
    if (netcam->rtsp->frame == NULL) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to allocate frame.  Fatal error.  Check FFmpeg/Libav configuration");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    if (netcam_rtsp_open_sws(netcam) < 0) return -1;

#if 0 /* XXX: don't do this or else we lose a frame */
    /*
     *  Validate that the previous steps opened the camera
     */
    retcd = netcam_read_rtsp_image(netcam);
    if ((retcd < 0) || (netcam->rtsp->interrupted == 1)){
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Failed to read first image");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }
#endif /* 0 */

    netcam->rtsp->active = 1;

    return 0;

}
/**
* netcam_rtsp_open_sws
*
*    This function opens the rescaling context components.
*
* Parameters
*
*       netcam  The netcam context to read from
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
static int netcam_rtsp_open_sws(netcam_context_ptr netcam){

    netcam->width  = ((netcam->cnt->conf.width / 8) * 8);
    netcam->height = ((netcam->cnt->conf.height / 8) * 8);


    netcam->rtsp->swsframe_in = my_frame_alloc();
    if (netcam->rtsp->swsframe_in == NULL) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to allocate frame.  Fatal error.  Check FFmpeg/Libav configuration");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    netcam->rtsp->swsframe_out = my_frame_alloc();
    if (netcam->rtsp->swsframe_out == NULL) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to allocate frame.  Fatal error.  Check FFmpeg/Libav configuration");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    /*
     *  The scaling context is used to change dimensions to config file and
     *  also if the format sent by the camera is not YUV420.
     */
    netcam->rtsp->swsctx = sws_getContext(
         netcam->rtsp->codec_context->width
        ,netcam->rtsp->codec_context->height
        ,netcam->rtsp->codec_context->pix_fmt
        ,netcam->width
        ,netcam->height
        ,MY_PIX_FMT_YUV420P
        ,SWS_BICUBIC,NULL,NULL,NULL);
    if (netcam->rtsp->swsctx == NULL) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to allocate scaling context.  Fatal error.  Check FFmpeg/Libav configuration");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    netcam->rtsp->swsframe_size = my_image_get_buffer_size(
            MY_PIX_FMT_YUV420P
            ,netcam->width
            ,netcam->height);
    if (netcam->rtsp->swsframe_size <= 0) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error determining size of frame out");
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    /* the image buffers must be big enough to hold the final frame after resizing */
    netcam_check_buffsize(netcam->receiving, netcam->rtsp->swsframe_size);
    netcam_check_buffsize(netcam->latest, netcam->rtsp->swsframe_size);

    return 0;

}
/**
* netcam_rtsp_resize
*
*    This function reencodes the image to yuv420p with the desired size
*
* Parameters
*
*       netcam  The netcam context to read from
*       image   The destination image.
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
static int netcam_rtsp_resize(netcam_context_ptr netcam){

    int      retcd;
    char     errstr[128];

    retcd=my_image_fill_arrays(
        netcam->rtsp->swsframe_in
        ,(uint8_t*)netcam->receiving->ptr
        ,netcam->rtsp->codec_context->pix_fmt
        ,netcam->rtsp->codec_context->width
        ,netcam->rtsp->codec_context->height);
    if (retcd < 0) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error allocating picture in: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    /* XXX: could we use cnt->imgs.common_buffer instead? */
    if (netcam->rtsp->resize_buffer == NULL)
        netcam->rtsp->resize_buffer = (uint8_t *)av_malloc(netcam->rtsp->swsframe_size * sizeof(uint8_t));

    retcd=my_image_fill_arrays(
        netcam->rtsp->swsframe_out
        ,netcam->rtsp->resize_buffer
        ,MY_PIX_FMT_YUV420P
        ,netcam->width
        ,netcam->height);
    if (retcd < 0) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error allocating picture out: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    retcd = sws_scale(
        netcam->rtsp->swsctx
        ,(const uint8_t* const *)netcam->rtsp->swsframe_in->data
        ,netcam->rtsp->swsframe_in->linesize
        ,0
        ,netcam->rtsp->codec_context->height
        ,netcam->rtsp->swsframe_out->data
        ,netcam->rtsp->swsframe_out->linesize);
    if (retcd < 0) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error resizing/reformatting: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }

    retcd=my_image_copy_to_buffer(
         netcam->rtsp->swsframe_out
        ,(uint8_t *)netcam->receiving->ptr
        ,MY_PIX_FMT_YUV420P
        ,netcam->width
        ,netcam->height
        ,netcam->rtsp->swsframe_size);
    if (retcd < 0) {
        if (netcam->rtsp->status == RTSP_NOTCONNECTED){
            av_strerror(retcd, errstr, sizeof(errstr));
            MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: Error putting frame into output buffer: %s", errstr);
        }
        netcam_rtsp_close_context(netcam);
        return -1;
    }
    netcam->receiving->used = netcam->rtsp->swsframe_size;

    return 0;

}
/*********************************************************
 *  This ends the section of functions that rely upon FFmpeg
 ***********************************************************/
#endif /* End HAVE_FFMPEG */

/**
* netcam_connect_rtsp
*
*    This function initiates the connection to the rtsp camera.
*
* Parameters
*
*       netcam  The netcam context to open.
*
* Returns:
*       Failure    -1
*       Success     0(zero)
*
*/
int netcam_connect_rtsp(netcam_context_ptr netcam){
#ifdef HAVE_FFMPEG

    if (netcam_rtsp_open_context(netcam) < 0) return -1;

    if (netcam_rtsp_resize_ntc(netcam) < 0 ) return -1;

#if 0 /* XXX: don't do this or else we lose a frame */
    if (netcam_read_rtsp_image(netcam) < 0) return -1;
#endif /* 0 */

    /* Ensure that both of these are empty
     */
    ffmpeg_packet_buffer_unref(netcam->latest->frame_packets);
    ffmpeg_packet_buffer_unref(netcam->receiving->frame_packets);

    netcam->rtsp->status = RTSP_CONNECTED;

    MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO, "%s: Camera connected");

    return 0;

#else  /* No FFmpeg/Libav */
    if (netcam)
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: FFmpeg/Libav not found on computer.  No RTSP support");
    return -1;
#endif /* End #ifdef HAVE_FFMPEG */
}

/**
* netcam_shutdown_rtsp
*
*    This function closes and frees all the items for rtsp
*
* Parameters
*
*       netcam  The netcam context to free.
*
* Returns:
*       Failure    nothing
*       Success    nothing
*
*/
void netcam_shutdown_rtsp(netcam_context_ptr netcam){
#ifdef HAVE_FFMPEG

    if (netcam->rtsp->status == RTSP_CONNECTED ||
        netcam->rtsp->status == RTSP_READINGIMAGE) {
        MOTION_LOG(NTC, TYPE_NETCAM, NO_ERRNO,"%s: netcam shut down");
    }

    netcam_rtsp_close_context(netcam);

    free(netcam->rtsp->path);
    free(netcam->rtsp->user);
    free(netcam->rtsp->pass);

    /* Note: Motion main-loop will free netcam->rtsp->rtsp_info */

    free(netcam->rtsp);
    netcam->rtsp = NULL;

#else  /* No FFmpeg/Libav */
    /* Stop compiler warnings */
    if (netcam)
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: FFmpeg/Libav not found on computer.  No RTSP support");
#endif /* End #ifdef HAVE_FFMPEG */
}

/**
* netcam_setup_rtsp
*
*    This function sets up all the necessary items for the
*    rtsp camera.
*
* Parameters
*
*       netcam  The netcam context to free.
*       url     The URL of the camera
*
* Returns:
*       Failure    -1
*       Success    0(zero)
*
*/
int netcam_setup_rtsp(netcam_context_ptr netcam, struct url_t *url){
#ifdef HAVE_FFMPEG

  struct context *cnt = netcam->cnt;
  const char *ptr;
  char *path;
  int ret = -1;

  netcam->caps.streaming = NCS_RTSP;

  netcam->rtsp = rtsp_new_context();

  netcam_rtsp_null_context(netcam);

  if (netcam->rtsp == NULL) {
    MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: unable to create rtsp context");
    netcam_shutdown_rtsp(netcam);
    return -1;
  }

  /*
   * Allocate space for a working string to contain the path.
   * The extra 5 is for "://", ":" and string terminator.
   */

  // force port to a sane value
  if (netcam->connect_port > 65535) {
    netcam->connect_port = 65535;
  } else if (netcam->connect_port < 0) {
    netcam->connect_port = 0;
  }

    if (cnt->conf.netcam_userpass != NULL) {
        ptr = cnt->conf.netcam_userpass;
    } else {
        ptr = url->userpass;  /* Don't set this one NULL, gets freed. */
    }

    if (ptr != NULL) {
        char *cptr;
        if ((cptr = strchr(ptr, ':')) == NULL) {
            netcam->rtsp->user = mystrdup(ptr);
        } else {
            netcam->rtsp->user = mymalloc((cptr - ptr)+2);  //+2 for string terminator
            memcpy(netcam->rtsp->user, ptr,(cptr - ptr));
            netcam->rtsp->pass = mystrdup(cptr + 1);
        }
    }

    /*
     *  Need a method to query the path and
     *  determine the authentication type
     */
    if (strcmp(url->service, "v4l2") == 0) {
        path = mymalloc(strlen(url->path));
        sprintf(path, "%s", url->path);
    } else if ((netcam->rtsp->user != NULL) && (netcam->rtsp->pass != NULL)) {
        path = mymalloc(strlen(url->service) + strlen(netcam->connect_host)
              + 5 + strlen(url->path) + 5
              + strlen(netcam->rtsp->user) + strlen(netcam->rtsp->pass) + 4 );
        sprintf(path, "%s://%s:%s@%s:%d%s",
                url->service,netcam->rtsp->user,netcam->rtsp->pass,
                netcam->connect_host, netcam->connect_port, url->path);
    } else {
        path = mymalloc(strlen(url->service) + strlen(netcam->connect_host)
              + 5 + strlen(url->path) + 5);
        sprintf(path, "%s://%s:%d%s", url->service,
            netcam->connect_host, netcam->connect_port, url->path);
    }
    netcam->rtsp->path = path;

    netcam_url_free(url);

    /*
     * Keep a pointer to the original URL for logging purposes
     * (we don't want to put passwords into the log)
     */
    netcam->rtsp->netcam_url = cnt->conf.netcam_url;

    /*
     * Now we need to set some flags
     */
    netcam->rtsp->status = RTSP_NOTCONNECTED;

    /*
     * Warn and fix dimensions as needed.
     */
    if (netcam->cnt->conf.width % 8) {
        MOTION_LOG(CRT, TYPE_NETCAM, NO_ERRNO, "%s: Image width (%d) requested is not modulo 8.", netcam->cnt->conf.width);
        netcam->cnt->conf.width = netcam->cnt->conf.width - (netcam->cnt->conf.width % 8) + 8;
        MOTION_LOG(CRT, TYPE_NETCAM, NO_ERRNO, "%s: Adjusting width to next higher multiple of 8 (%d).", netcam->cnt->conf.width);
    }
    if (netcam->cnt->conf.height % 8) {
        MOTION_LOG(CRT, TYPE_NETCAM, NO_ERRNO, "%s: Image height (%d) requested is not modulo 8.", netcam->cnt->conf.height);
        netcam->cnt->conf.height = netcam->cnt->conf.height - (netcam->cnt->conf.height % 8) + 8;
        MOTION_LOG(CRT, TYPE_NETCAM, NO_ERRNO, "%s: Adjusting height to next higher multiple of 8 (%d).", netcam->cnt->conf.height);
    }

    /*
     * The RTSP context should be all ready to attempt a connection with
     * the server, so we try ....
     */
    ret = netcam_connect_rtsp(netcam);
    if (ret < 0){
        return ret;
    }

    netcam->get_image = netcam_read_rtsp_image;

  return 0;

#else  /* No FFmpeg/Libav */
    /* Stop compiler warnings */
    if ((url) || (netcam))
        MOTION_LOG(ERR, TYPE_NETCAM, NO_ERRNO, "%s: FFmpeg/Libav not found on computer.  No RTSP support");
    return -1;
#endif /* End #ifdef HAVE_FFMPEG */
}

/**
* netcam_next_rtsp
*
*    This function moves the picture to the image buffer.
*    If the picture is not in the correct format for size
*    it will put it into the requested format
*
* Parameters
*
*       netcam  The netcam context to free.
*       url     The URL of the camera
*
* Returns:
*       Failure    -1
*       Success    0(zero)
*
*/
int netcam_next_rtsp(unsigned char *image , netcam_context_ptr netcam){
    /* This function is running from the motion_loop thread - generally the
     * rest of the functions in this file are running from the
     * netcam_handler_loop thread - this means you generally cannot access
     * or call anything else without taking care of thread safety.
     * The netcam mutex *only* protects netcam->latest, it cannot be
     * used to safely call other netcam functions. */

    pthread_mutex_lock(&netcam->mutex);

    while (!netcam->get_picture) {
        struct timespec waittime;
        waittime.tv_sec  = time(NULL) + 2;
        waittime.tv_nsec = 0;
        if (pthread_cond_timedwait(&netcam->pic_ready, &netcam->mutex, &waittime) == 0)
            break;
        if (netcam->cnt->finish)
            goto done;
    }

    netcam->get_picture = 0;

    memcpy(image, netcam->latest->ptr, netcam->latest->used);

#ifdef HAVE_FFMPEG

    ffmpeg_packet_buffer_move(netcam->cnt->recent_packets, netcam->new_packets);
    if (netcam->cnt->rtsp_info == NULL)
        netcam->cnt->rtsp_info = netcam->rtsp->rtsp_info;

    assert(image == netcam->cnt->current_image->image);
    netcam->cnt->current_image->packet_serial = netcam->latest->packet_serial;
    netcam->cnt->current_image->packet_count  = netcam->latest->packet_count;

    if (netcam->latest->packet_serial < netcam->cnt->last_packet_serial) {
        /*
         * Stream has restarted! Finish writing any open movie file
         */
        if (netcam->cnt->ffmpeg_output)
            netcam->cnt->makemovie = 1;
        /*
         * Ensure that we read in (and discard) a full GOP before we start
         * passthru recording, as some cameras are known to send bogus
         * packet timestamps early in the stream after reconnecting. (It's
         * not enough just to discard the first few packets, because we
         * still need to be able to reach back for a "key" packet)
         */
        netcam->cnt->startup_frames = MAXVAL(netcam->cnt->init_startup_frames, 100);
    }

    netcam->cnt->last_packet_serial = netcam->latest->packet_serial;
    netcam->cnt->last_pts           = netcam->latest->pts;

#endif /* HAVE_FFMPEG */

    done:

    pthread_mutex_unlock(&netcam->mutex);

    if (netcam->cnt->rotate_data.degrees > 0)
        /* Rotate as specified */
        rotate_map(netcam->cnt, image);

    return 0;
}
