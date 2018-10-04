#include <unistd.h>
#include <iostream>
#include <vector>
// #include <chrono>

// #include <opencv2/highgui.hpp>
// #include <opencv2/video.hpp>
#include "clipp.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

using namespace clipp;
// using namespace std::chrono;

/* cv::VideoCapture get_device(int camID, double width, double height)
{
  cv::VideoCapture cam(camID);
  if (!cam.isOpened())
  {
    std::cout << "Failed to open video capture device!" << std::endl;
    exit(1);
  }

  cam.set(cv::CAP_PROP_FRAME_WIDTH, width);
  cam.set(cv::CAP_PROP_FRAME_HEIGHT, height);

  return cam;
} */

void initialize_avformat_context(AVFormatContext *&fctx, const char *format_name)
{
  int ret = avformat_alloc_output_context2(&fctx, nullptr, format_name, nullptr);
  if (ret < 0)
  {
    std::cout << "Could not allocate output format context!" << std::endl;
    exit(1);
  }
}

void initialize_io_context(AVFormatContext *&fctx, const char *output)
{
  if (!(fctx->oformat->flags & AVFMT_NOFILE))
  {
    int ret = avio_open2(&fctx->pb, output, AVIO_FLAG_WRITE, nullptr, nullptr);
    if (ret < 0)
    {
      std::cout << "Could not open output IO context!" << std::endl;
      exit(1);
    }
  }
}

void set_video_codec_params(AVFormatContext *&fctx, AVCodecContext *&codec_ctx, double width, double height, int fps, int bitrate)
{
  const AVRational dst_fps = {fps, 1};

  codec_ctx->codec_tag = 0;
  codec_ctx->codec_id = AV_CODEC_ID_H264;
  codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
  codec_ctx->width = width;
  codec_ctx->height = height;
  codec_ctx->gop_size = 12;
  codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
  codec_ctx->framerate = dst_fps;
  codec_ctx->time_base = av_inv_q(dst_fps);
  codec_ctx->bit_rate = bitrate;
  if (fctx->oformat->flags & AVFMT_GLOBALHEADER) {
    codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
}

void initialize_video_codec_stream(AVStream *&stream, AVCodecContext *&codec_ctx, AVCodec *&codec, std::string codec_profile)
{
  int ret = avcodec_parameters_from_context(stream->codecpar, codec_ctx);
  if (ret < 0)
  {
    std::cout << "Could not initialize stream codec parameters!" << std::endl;
    exit(1);
  }

  AVDictionary *codec_options = nullptr;
  av_dict_set(&codec_options, "profile", codec_profile.c_str(), 0);
  // av_dict_set(&codec_options, "preset", "superfast", 0);
  // av_dict_set(&codec_options, "tune", "zerolatency", 0);

  // open video encoder
  ret = avcodec_open2(codec_ctx, codec, &codec_options);
  if (ret < 0)
  {
    std::cout << "Could not open video encoder!" << std::endl;
    exit(1);
  }
}

SwsContext *initialize_sample_scaler(AVCodecContext *codec_ctx, double width, double height)
{
  SwsContext *swsctx = sws_getContext(width, height, AV_PIX_FMT_BGR24, width, height, codec_ctx->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
  if (!swsctx)
  {
    std::cout << "Could not initialize sample scaler!" << std::endl;
    exit(1);
  }

  return swsctx;
}

AVFrame *allocate_frame_buffer(AVCodecContext *codec_ctx, double width, double height)
{
  AVFrame *frame = av_frame_alloc();

  // std::vector<uint8_t> framebuf(av_image_get_buffer_size(codec_ctx->pix_fmt, width, height, 1));
  int size = av_image_get_buffer_size(codec_ctx->pix_fmt, width, height, 1);
  uint8_t *framebuf = (uint8_t*)av_malloc(size);
  av_image_fill_arrays(frame->data, frame->linesize, framebuf, codec_ctx->pix_fmt, width, height, 1);
  frame->width = width;
  frame->height = height;
  frame->format = static_cast<int>(codec_ctx->pix_fmt);

  return frame;
}

/* check that a given sample format is supported by the encoder */
static int check_sample_fmt(AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const enum AVSampleFormat *p = codec->sample_fmts;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* just pick the highest supported samplerate */
static int select_sample_rate(AVCodec *codec)
{
    const int *p;
    int best_samplerate = 0;

    if (!codec->supported_samplerates)
        return 44100;

    p = codec->supported_samplerates;
    while (*p) {
        best_samplerate = FFMAX(*p, best_samplerate);
        p++;
    }
    return best_samplerate;
}

/* select layout with the highest channel count */
static int select_channel_layout(AVCodec *codec)
{
    const uint64_t *p;
    uint64_t best_ch_layout = 0;
    int best_nb_channels   = 0;

    if (!codec->channel_layouts)
        return AV_CH_LAYOUT_MONO;

    p = codec->channel_layouts;
    while (*p) {
        int nb_channels = av_get_channel_layout_nb_channels(*p);

        if (nb_channels > best_nb_channels) {
            best_ch_layout    = *p;
            best_nb_channels = nb_channels;
        }
        p++;
    }
    return best_ch_layout;
}

void stream_av(double width, double height, int fps, int camID, int bitrate, std::string codec_profile, std::string server)
{
  const int64_t startTime = av_gettime();
  // std::cout << "stream video " << width << " " << height << std::endl;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
  av_register_all();
#endif
  avformat_network_init();

  const char *output = server.c_str();
  int ret;
  std::vector<uint8_t> imgbuf;
  // std::vector<uint8_t> samplebuf;
  float **samplebuf;

  // format
  AVCodec *audio_codec, *video_codec;
  AVStream *audio_st, *video_st;
  AVCodecContext *audio_cc, *video_cc;
  AVFormatContext *oc = nullptr;

  initialize_avformat_context(oc, "flv");
  initialize_io_context(oc, output);

  // video
  {
    video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);

    oc->video_codec = video_codec;
    oc->video_codec_id = AV_CODEC_ID_H264;

    video_st = avformat_new_stream(oc, video_codec);
    video_cc = avcodec_alloc_context3(video_codec);
    // video_cc = video_st->codec;

    set_video_codec_params(oc, video_cc, width, height, fps, bitrate);
    initialize_video_codec_stream(video_st, video_cc, video_codec, codec_profile);

    video_st->codecpar->extradata = video_cc->extradata;
    video_st->codecpar->extradata_size = video_cc->extradata_size;

    imgbuf.resize((int)height * (int)width * 3 + 16);
  }
  auto *swsctx = initialize_sample_scaler(video_cc, width, height);
  AVFrame *video_frame = allocate_frame_buffer(video_cc, width, height);
  video_frame->pts = 0;
  AVFrame *audio_frame = av_frame_alloc();
  audio_frame->pts = 0;

  // audio
  {
    // audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    audio_codec = avcodec_find_encoder(AV_CODEC_ID_MP3);
    if (!audio_codec) {
      fprintf(stderr, "Could not find codec\n");
      exit(1);
    }

    oc->audio_codec = audio_codec;
    oc->audio_codec_id = audio_codec->id;

    audio_st = avformat_new_stream(oc, audio_codec);

    if (!audio_st) {
      fprintf(stderr, "Could not allocate stream\n");
      exit(1);
    }
    /* // audio_st->id = oc->nb_streams-1;
    // audio_cc = audio_st->codec;
    audio_cc = avcodec_alloc_context3(audio_codec);

    // put sample parameters
    audio_cc->bit_rate = 64000;

    // check that the encoder supports s16 pcm input
    audio_cc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    if (!check_sample_fmt(audio_codec, audio_cc->sample_fmt)) {
        fprintf(stderr, "Encoder does not support sample format %s",
                av_get_sample_fmt_name(audio_cc->sample_fmt));
        exit(1);
    }
    audio_cc->profile = FF_PROFILE_AAC_HE_V2;

    // select other audio parameters supported by the encoder
    audio_cc->sample_rate    = select_sample_rate(audio_codec);
    audio_cc->channel_layout = select_channel_layout(audio_codec);
    audio_cc->channels       = av_get_channel_layout_nb_channels(audio_cc->channel_layout);
    if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
      audio_cc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    } */

    audio_cc = avcodec_alloc_context3(audio_codec);
    audio_cc->bit_rate = 64000;
    audio_cc->sample_rate = select_sample_rate(audio_codec);
    // audio_cc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    audio_cc->sample_fmt = AV_SAMPLE_FMT_S16;
    audio_cc->channel_layout = AV_CH_LAYOUT_MONO;
    audio_cc->channels = av_get_channel_layout_nb_channels(audio_cc->channel_layout);

    // std::cout << "check sample rate " << audio_cc->sample_rate << " " << audio_cc->bit_rate << std::endl;

    ret = avcodec_parameters_from_context(audio_st->codecpar, audio_cc);
    if (ret < 0) {
      std::cout << "Error sending frame to audio codec context! " << ret << std::endl;
      exit(1);
    }
    /* audio_st->codecpar->bit_rate = audio_cc->bit_rate;
    audio_st->codecpar->sample_rate = audio_cc->sample_rate;
    audio_st->codecpar->channels = audio_cc->channels;
    audio_st->codecpar->channel_layout = audio_cc->channel_layout;
    audio_st->codecpar->codec_id = AV_CODEC_ID_AAC;
    audio_st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio_st->codecpar->format = AV_SAMPLE_FMT_FLTP;
    audio_st->codecpar->frame_size = audio_cc->frame_size;
    audio_st->codecpar->block_align = audio_cc->block_align;
    audio_st->codecpar->initial_padding = audio_cc->initial_padding; */
    audio_st->codecpar->extradata = audio_cc->extradata;
    audio_st->codecpar->extradata_size = audio_cc->extradata_size;

    // AVDictionary *codec_options = nullptr;
    // av_dict_set(&codec_options, "profile", "lol", 0);
    // av_dict_set(&codec_options, "preset", "superfast", 0);
    // av_dict_set(&codec_options, "tune", "zerolatency", 0);
    ret = avcodec_open2(audio_cc, audio_codec, NULL);
    if (ret < 0) {
      fprintf(stderr, "Could not open audio codec\n");
      exit(1);
    }

    audio_frame->sample_rate     = audio_cc->sample_rate;
    audio_frame->nb_samples     = audio_cc->frame_size;
    audio_frame->format         = audio_cc->sample_fmt;
    audio_frame->channel_layout = audio_cc->channel_layout;

    int buffer_size = av_samples_get_buffer_size(NULL, audio_cc->channels, audio_cc->frame_size, audio_cc->sample_fmt, 0);
    av_frame_get_buffer(audio_frame, buffer_size / audio_cc->channels);

    /* int buffer_size = av_samples_get_buffer_size(NULL, audio_cc->channels, audio_cc->frame_size, audio_cc->sample_fmt, 0);
    if (buffer_size < 0) {
        fprintf(stderr, "Could not get sample buffer size\n");
        exit(1);
    }
    samplebuf.resize(buffer_size); */
    /* std::cout << "alloc sample buf 1" << std::endl;
    int src_samples_linesize;
    ret = av_samples_alloc_array_and_samples((uint8_t***)&samplebuf, &src_samples_linesize, audio_cc->channels, audio_frame->nb_samples, AV_SAMPLE_FMT_FLTP, 0);
    if (ret < 0) {
      std::cout << "Could not alloc audio frame! " << ret << std::endl;
      exit(1);
    }
    std::cout << "alloc sample buf 2" << std::endl; */
    /* // setup the data pointers in the AVFrame
    ret = avcodec_fill_audio_frame(audio_frame, audio_cc->channels, audio_cc->sample_fmt, (const uint8_t*)samplebuf.data(), buffer_size, 0);
    if (ret < 0) {
      std::cout << "Could not fill audio frame! " << ret << std::endl;
      exit(1);
    } */
  }

  av_dump_format(oc, 0, output, 1);

  ret = avformat_write_header(oc, nullptr);
  if (ret < 0) {
    std::cout << "Could not write header!" << std::endl;
    exit(1);
  }

  int vts = 0;
  int ats = 0;
  bool end_of_stream = false;

  do
  {
    /* Compute current audio and video time. */
    double audio_time = audio_frame->pts * av_q2d(audio_cc->time_base);
    double video_time = video_frame->pts * av_q2d(video_cc->time_base);
    /* write interleaved audio and video frames */
    // std::cout << "check time " << audio_time << " " << video_time << "\n";
    if (audio_time < video_time) {
      // std::cout << "audio" << "\n";

      // AVRational timebase = { 1, audio_cc->sample_rate };
      // audio_frame->pts += av_rescale_q(audio_cc->frame_size, timebase, audio_st->time_base);
      // audio_frame->pts += av_rescale_q(1, audio_cc->time_base, audio_st->time_base);
      // audio_frame->pts += av_rescale_q(audio_cc->frame_size, audio_cc->time_base, audio_st->time_base);

      // std::cout << "receive audio pkt 0 " << audio_frame->pts << " " << video_frame->pts << " " << (video_frame->pts - audio_frame->pts) << std::endl;
      // if (audio_frame->pts > video_frame->pts) {
      // std::cout << "receive audio pkt 1" << std::endl;

      av_frame_make_writable(audio_frame);
      /* for (int i = 0; i < samplebuf.size(); i++) {
      // for (int i = 0; i < 1000; i++) {
        samplebuf[i] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
      } */
      audio_frame->pts = av_rescale_q(ats * audio_frame->nb_samples, AVRational{1, audio_cc->sample_rate}, audio_cc->time_base);
      // audio_frame->pts += av_rescale_q(audio_frame->nb_samples, AVRational{1, audio_cc->sample_rate}, audio_cc->time_base);
      // audio_frame->pts = av_rescale_q(ats * audio_frame->nb_samples, AVRational{1, audio_cc->sample_rate}, audio_st->time_base);
      // audio_frame->pts = av_rescale_q(ats, AVRational{1, 1}, audio_cc->time_base);
      // audio_frame->pts = ats;
      // audio_frame->pkt_pts = audio_frame->pts;
      // audio_frame->pkt_dts = audio_frame->pts;
      // ats += audio_cc->frame_size;
      ats++;

      // std::cout << "receive audio pkt 0 " << audio_frame->pts << " += " << av_rescale_q(audio_frame->nb_samples, timebase, audio_st->time_base) << " " << timebase.num << "/" << timebase.den << " " << audio_st->time_base.num << "/" << audio_st->time_base.den << std::endl;

      ret = avcodec_send_frame(audio_cc, audio_frame);
      // int got_packet;
      // ret = avcodec_encode_audio2(audio_codec, &pkt, audio_frame, &got_packet);
      if (ret < 0) {
        fprintf(stderr, "Error encoding audio frame\n");
        exit(1);
      }

      // std::cout << "receive audio pkt 1" << std::endl;

      /* int got_output;
      ret = avcodec_encode_audio2(audio_cc, &pkt, audio_frame, &got_output);
      if (got_output) {
        ret = av_interleaved_write_frame(oc, &pkt);
        if (ret < 0)
        {
          std::cout << "Error writing interleaved audio frame! " << ret << std::endl;
          exit(1);
        }

        std::cout << "receive audio pkt 4" << std::endl;

        av_packet_unref(&pkt);
      } */

      for (;;) {
        AVPacket pkt = {0}; // data and size must be 0;
        av_init_packet(&pkt);

        ret = avcodec_receive_packet(audio_cc, &pkt);
        if (ret == -EAGAIN) {
          break;
        } else if (ret < 0)  {
          std::cout << "Error receiving packet from audio codec context! " << ret << std::endl;
          exit(1);
        }

        av_packet_rescale_ts(&pkt, audio_cc->time_base, audio_st->time_base);
        // av_packet_rescale_ts(&pkt, AVRational{1, audio_cc->sample_rate}, audio_st->time_base);
        pkt.stream_index = audio_st->index;

        // std::cout << "send audio packet " << audio_st->index << " " << (pkt.pts * av_q2d(audio_st->time_base)) << " " << (pkt.dts * av_q2d(audio_st->time_base)) << "\n";
        // std::cout << "got audio pkt pts 2 " << " " << pkt.pts << " " << pkt.dts << " " << pkt.duration << " " << pkt.pos << " " << audio_frame->pts << std::endl;

        ret = av_interleaved_write_frame(oc, &pkt);
        if (ret < 0)
        {
          std::cout << "Error writing interleaved audio frame! " << ret << std::endl;
          exit(1);
        }

        // std::cout << "receive audio pkt 3" << std::endl;

        av_packet_unref(&pkt);
      }
    } else {
      // std::cout << "video" << "\n";

      av_frame_make_writable(video_frame);
      for (int i = 0; i < 1000; i++) {
      // for (int i = 0; i < 1000; i++) {
        imgbuf[rand() % imgbuf.size()] = (uint8_t)(rand() & 0xFF);
      }
      uint8_t *data[] = {imgbuf.data()};
      const int stride[] = {(int)width};
      sws_scale(swsctx, data, stride, 0, height, video_frame->data, video_frame->linesize);
      video_frame->pts = av_rescale_q(vts, AVRational{1, fps}, video_cc->time_base);
      // video_frame->pkt_pts = video_frame->pts;
      // video_frame->pkt_dts = video_frame->pts;
      vts++;

      ret = avcodec_send_frame(video_cc, video_frame);
      if (ret < 0)
      {
        // std::cout << "Error sending frame to codec context!" << std::endl;
        exit(1);
      }

      // std::cout << "receive video pkt 1" << std::endl;

      for (;;) {
        AVPacket pkt = {0};
        av_init_packet(&pkt);

        ret = avcodec_receive_packet(video_cc, &pkt);
        if (ret == -EAGAIN) {
          break;
        } else if (ret < 0) {
          std::cout << "Error receiving packet from video codec context! " << ret << std::endl;
          exit(1);
        }

        // int64_t pts = pkt.pts;
        // pkt.duration = 1;
        av_packet_rescale_ts(&pkt, video_cc->time_base, video_st->time_base);
        pkt.stream_index = video_st->index;

        // std::cout << "send video packet " << video_st->index << " " << vts << " " << video_frame->pts << " " << (video_frame->pts * av_q2d(video_cc->time_base)) << "\n";

        // std::cout << "send video packet " << (pkt.pts * av_q2d(video_st->time_base)) << "\n";
        // std::cout << "got video pts " << vts << " " << pts << " " << pkt.pts << " " << videoPacketPts << "\n";

        ret = av_interleaved_write_frame(oc, &pkt);
        if (ret < 0) {
          std::cout << "Error writing interleaved video frame! " << ret << std::endl;
          exit(1);
        }

        // std::cout << "receive video pkt 3" << std::endl;

        av_packet_unref(&pkt);
      }
    }

    const int64_t nowTime = av_gettime() - startTime;
    int64_t videoPacketPts = av_rescale_q(video_frame->pts, video_cc->time_base, AVRational{1, AV_TIME_BASE});
    if (nowTime < videoPacketPts) {
      // std::cout << "sleep " << (videoPacketPts - nowTime) << "\n";
      av_usleep(videoPacketPts - nowTime);
    } else {
      // std::cout << "no sleep " << "\n";
    }
  } while (!end_of_stream);

  av_write_trailer(oc);

  av_frame_free(&video_frame);
  // av_frame_free(&audio_frame);
  avcodec_close(video_cc);
  avcodec_close(audio_cc);
  avio_close(oc->pb);
  avformat_free_context(oc);
}

int main(int argc, char *argv[]) {
  int cameraID = 0, fps = 20, width = 1280, height = 1024, bitrate = 3500 * 1024;
  std::string h264profile = "high444";
  std::string outputServer = "rtmp://127.0.0.1:1935/live/lol";
  bool dump_log = false;

  auto cli = (
    (option("-c", "--camera") & value("camera", cameraID)) % "camera ID (default: 0)",
    (option("-o", "--output") & value("output", outputServer)) % "output RTMP server (default: rtmp://127.0.0.1:1935/live/lol)",
    (option("-f", "--fps") & value("fps", fps)) % "frames-per-second (default: 30)",
    (option("-w", "--width") & value("width", width)) % "video width (default: 800)",
    (option("-h", "--height") & value("height", height)) % "video height (default: 640)",
    (option("-b", "--bitrate") & value("bitrate", bitrate)) % "stream bitrate in kb/s (default: 300000)",
    (option("-p", "--profile") & value("profile", h264profile)) % "H264 codec profile (baseline | high | high10 | high422 | high444 | main) (default: high444)",
    (option("-l", "--log") & value("log", dump_log)) % "print debug output (default: false)"
  );

  if (!parse(argc, argv, cli))
  {
    std::cout << make_man_page(cli, argv[0]) << std::endl;
    return 1;
  }

  if (dump_log)
  {
    av_log_set_level(AV_LOG_TRACE);
  }

  stream_av(width, height, fps, cameraID, bitrate, h264profile, outputServer);

  return 0;
}
