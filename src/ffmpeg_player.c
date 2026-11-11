#include "ffmpeg_player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_error_text(char *error, size_t error_size,
                           const char *format, ...)
{
    va_list args;

    if (error == NULL || error_size == 0)
        return;

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

#ifdef HAVE_FFMPEG

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <pulse/error.h>
#include <pulse/simple.h>

#define OUTPUT_RATE 48000
#define OUTPUT_CHANNELS 2
#define OUTPUT_BUFFER_BYTES 19200

typedef enum {
    DECODE_CONTINUE,
    DECODE_STOP,
    DECODE_SEEK,
    DECODE_EOF,
    DECODE_ERROR
} DecodeResult;

struct FfmpegPlayer {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool thread_started;
    bool stop;
    bool paused;
    bool idle;
    bool running;
    bool seek_pending;
    bool fatal;
    double seek_target;
    double time_pos;
    double duration;
    double volume;
    char error[256];
    pa_simple *stream;
    AVFormatContext *format;
    AVCodecContext *codec;
    SwrContext *swr;
    int stream_index;
};

static void set_player_error(FfmpegPlayer *player, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    vsnprintf(player->error, sizeof(player->error), format, args);
    va_end(args);
}

static void set_av_error(FfmpegPlayer *player, const char *action, int code)
{
    char message[AV_ERROR_MAX_STRING_SIZE];

    if (av_strerror(code, message, sizeof(message)) != 0)
        snprintf(message, sizeof(message), "错误码 %d", code);
    set_player_error(player, "%s：%s", action, message);
}

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    size_t length;

    if (destination_size == 0)
        return;
    length = strlen(source);
    if (length >= destination_size)
        length = destination_size - 1;
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void close_decoder(FfmpegPlayer *player)
{
    swr_free(&player->swr);
    avcodec_free_context(&player->codec);
    if (player->format != NULL)
        avformat_close_input(&player->format);
    player->stream_index = -1;
}

static void stop_thread(FfmpegPlayer *player)
{
    if (player->thread_started) {
        pthread_mutex_lock(&player->mutex);
        player->stop = true;
        player->paused = false;
        pthread_cond_broadcast(&player->cond);
        pthread_mutex_unlock(&player->mutex);

        pthread_join(player->thread, NULL);
        player->thread_started = false;
    }

    close_decoder(player);

    if (player->stream != NULL) {
        int pulse_error = 0;

        (void)pa_simple_flush(player->stream, &pulse_error);
    }

    pthread_mutex_lock(&player->mutex);
    player->stop = false;
    player->paused = false;
    player->seek_pending = false;
    player->idle = true;
    player->time_pos = 0.0;
    player->duration = 0.0;
    pthread_mutex_unlock(&player->mutex);
}

static DecodeResult wait_for_resume(FfmpegPlayer *player)
{
    DecodeResult result = DECODE_CONTINUE;

    pthread_mutex_lock(&player->mutex);
    if (player->paused && !player->stop && !player->seek_pending) {
        pthread_mutex_unlock(&player->mutex);
        if (player->stream != NULL) {
            int pulse_error = 0;

            (void)pa_simple_flush(player->stream, &pulse_error);
        }
        pthread_mutex_lock(&player->mutex);
        while (player->paused && !player->stop && !player->seek_pending)
            pthread_cond_wait(&player->cond, &player->mutex);
    }

    if (player->stop)
        result = DECODE_STOP;
    else if (player->seek_pending)
        result = DECODE_SEEK;

    pthread_mutex_unlock(&player->mutex);
    return result;
}

static void perform_seek(FfmpegPlayer *player, double target)
{
    AVStream *stream = player->format->streams[player->stream_index];
    int64_t timestamp;
    int result;

    if (target < 0.0)
        target = 0.0;
    if (player->duration > 0.0 && target > player->duration)
        target = player->duration;

    timestamp = av_rescale_q((int64_t)(target * AV_TIME_BASE),
                             AV_TIME_BASE_Q, stream->time_base);
    result = avformat_seek_file(player->format, player->stream_index,
                                INT64_MIN, timestamp, INT64_MAX, 0);
    if (result < 0) {
        pthread_mutex_lock(&player->mutex);
        set_av_error(player, "跳转失败", result);
        pthread_mutex_unlock(&player->mutex);
        return;
    }

    avcodec_flush_buffers(player->codec);
    (void)swr_close(player->swr);
    (void)swr_init(player->swr);
    if (player->stream != NULL) {
        int pulse_error = 0;

        (void)pa_simple_flush(player->stream, &pulse_error);
    }

    pthread_mutex_lock(&player->mutex);
    player->time_pos = target;
    pthread_mutex_unlock(&player->mutex);
}

static DecodeResult write_frame(FfmpegPlayer *player, AVFrame *frame)
{
    int output_samples = swr_get_out_samples(player->swr, frame->nb_samples);
    uint8_t *buffer = NULL;
    int converted;
    double volume;
    int result;

    if (output_samples <= 0)
        return DECODE_CONTINUE;

    result = av_samples_alloc(&buffer, NULL, OUTPUT_CHANNELS, output_samples,
                              AV_SAMPLE_FMT_S16, 0);
    if (result < 0) {
        pthread_mutex_lock(&player->mutex);
        set_av_error(player, "无法分配音频缓冲区", result);
        pthread_mutex_unlock(&player->mutex);
        return DECODE_ERROR;
    }

    converted = swr_convert(player->swr, &buffer, output_samples,
                            (const uint8_t **)frame->extended_data,
                            frame->nb_samples);
    if (converted < 0) {
        av_freep(&buffer);
        pthread_mutex_lock(&player->mutex);
        set_av_error(player, "音频重采样失败", converted);
        pthread_mutex_unlock(&player->mutex);
        return DECODE_ERROR;
    }

    if (converted > 0) {
        int sample_count = converted * OUTPUT_CHANNELS;
        int i;

        pthread_mutex_lock(&player->mutex);
        volume = player->volume;
        pthread_mutex_unlock(&player->mutex);

        if (volume != 1.0) {
            int16_t *samples = (int16_t *)buffer;

            for (i = 0; i < sample_count; i++) {
                int value = (int)(samples[i] * volume);

                if (value > INT16_MAX)
                    value = INT16_MAX;
                if (value < INT16_MIN)
                    value = INT16_MIN;
                samples[i] = (int16_t)value;
            }
        }

        if (pa_simple_write(player->stream, buffer,
                            (size_t)sample_count * sizeof(int16_t),
                            &result) < 0) {
            pthread_mutex_lock(&player->mutex);
            set_player_error(player, "写入 PulseAudio 失败：%s",
                             pa_strerror(result));
            pthread_mutex_unlock(&player->mutex);
            av_freep(&buffer);
            return DECODE_ERROR;
        }

        pthread_mutex_lock(&player->mutex);
        player->time_pos += (double)converted / (double)OUTPUT_RATE;
        if (player->duration > 0.0 && player->time_pos > player->duration)
            player->time_pos = player->duration;
        pthread_mutex_unlock(&player->mutex);
    }

    av_freep(&buffer);
    return DECODE_CONTINUE;
}

static DecodeResult process_frames(FfmpegPlayer *player, AVFrame *frame)
{
    for (;;) {
        DecodeResult state = wait_for_resume(player);
        int result;

        if (state != DECODE_CONTINUE)
            return state;

        result = avcodec_receive_frame(player->codec, frame);
        if (result == AVERROR(EAGAIN))
            return DECODE_CONTINUE;
        if (result == AVERROR_EOF)
            return DECODE_EOF;
        if (result < 0) {
            pthread_mutex_lock(&player->mutex);
            set_av_error(player, "音频解码失败", result);
            pthread_mutex_unlock(&player->mutex);
            return DECODE_ERROR;
        }

        state = write_frame(player, frame);
        av_frame_unref(frame);
        if (state != DECODE_CONTINUE)
            return state;
    }
}

static DecodeResult send_packet(FfmpegPlayer *player, AVPacket *packet,
                                AVFrame *frame, int *send_result)
{
    for (;;) {
        DecodeResult state;
        int result = avcodec_send_packet(player->codec, packet);

        if (result != AVERROR(EAGAIN)) {
            *send_result = result;
            return DECODE_CONTINUE;
        }

        state = process_frames(player, frame);
        if (state != DECODE_CONTINUE)
            return state;
    }
}

static void *decode_thread(void *argument)
{
    FfmpegPlayer *player = argument;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    bool reached_eof = false;
    DecodeResult state = DECODE_CONTINUE;

    if (packet == NULL || frame == NULL) {
        pthread_mutex_lock(&player->mutex);
        set_player_error(player, "内存不足");
        pthread_mutex_unlock(&player->mutex);
        state = DECODE_ERROR;
        goto done;
    }

    while (!reached_eof && state == DECODE_CONTINUE) {
        int result;

        state = wait_for_resume(player);
        if (state == DECODE_SEEK) {
            double target;

            pthread_mutex_lock(&player->mutex);
            target = player->seek_target;
            player->seek_pending = false;
            pthread_mutex_unlock(&player->mutex);
            perform_seek(player, target);
            state = DECODE_CONTINUE;
            continue;
        }
        if (state != DECODE_CONTINUE)
            break;

        result = av_read_frame(player->format, packet);
        if (result == AVERROR_EOF) {
            reached_eof = true;
            break;
        }
        if (result < 0) {
            pthread_mutex_lock(&player->mutex);
            set_av_error(player, "读取音频数据失败", result);
            pthread_mutex_unlock(&player->mutex);
            state = DECODE_ERROR;
            break;
        }
        if (packet->stream_index != player->stream_index) {
            av_packet_unref(packet);
            continue;
        }

        state = send_packet(player, packet, frame, &result);
        av_packet_unref(packet);
        if (state == DECODE_SEEK) {
            state = DECODE_CONTINUE;
            continue;
        }
        if (state != DECODE_CONTINUE)
            break;
        if (result < 0 && result != AVERROR_EOF) {
            pthread_mutex_lock(&player->mutex);
            set_av_error(player, "发送音频数据失败", result);
            pthread_mutex_unlock(&player->mutex);
            state = DECODE_ERROR;
            break;
        }

        state = process_frames(player, frame);
        if (state == DECODE_SEEK) {
            state = DECODE_CONTINUE;
            continue;
        }
        if (state == DECODE_EOF) {
            reached_eof = true;
            break;
        }
        if (state != DECODE_CONTINUE)
            break;
    }

    if (state == DECODE_CONTINUE || state == DECODE_EOF) {
        int result = avcodec_send_packet(player->codec, NULL);

        if (result >= 0 || result == AVERROR_EOF) {
            DecodeResult flush_state = process_frames(player, frame);

            if (flush_state == DECODE_ERROR)
                state = DECODE_ERROR;
        }

        if (state != DECODE_ERROR && player->stream != NULL) {
            int pulse_error = 0;

            (void)pa_simple_drain(player->stream, &pulse_error);
        }

        pthread_mutex_lock(&player->mutex);
        player->idle = true;
        pthread_mutex_unlock(&player->mutex);
    }

    if (state == DECODE_ERROR) {
        pthread_mutex_lock(&player->mutex);
        player->fatal = true;
        player->running = false;
        player->idle = true;
        pthread_mutex_unlock(&player->mutex);
    }

done:
    av_frame_free(&frame);
    av_packet_free(&packet);
    return NULL;
}

static int open_decoder(FfmpegPlayer *player, const char *path)
{
    const AVCodec *decoder;
    AVChannelLayout input_layout = {0};
    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
    AVStream *stream;
    int result;

    result = avformat_open_input(&player->format, path, NULL, NULL);
    if (result < 0) {
        set_av_error(player, "无法打开音频文件", result);
        return -1;
    }

    result = avformat_find_stream_info(player->format, NULL);
    if (result < 0) {
        set_av_error(player, "无法读取音频信息", result);
        close_decoder(player);
        return -1;
    }

    result = av_find_best_stream(player->format, AVMEDIA_TYPE_AUDIO,
                                 -1, -1, &decoder, 0);
    if (result < 0) {
        set_player_error(player, "文件中没有可播放的音频流");
        close_decoder(player);
        return -1;
    }
    player->stream_index = result;
    stream = player->format->streams[player->stream_index];

    player->codec = avcodec_alloc_context3(decoder);
    if (player->codec == NULL) {
        set_player_error(player, "内存不足");
        close_decoder(player);
        return -1;
    }

    result = avcodec_parameters_to_context(player->codec, stream->codecpar);
    if (result < 0) {
        set_av_error(player, "无法初始化音频解码器", result);
        close_decoder(player);
        return -1;
    }

    result = avcodec_open2(player->codec, decoder, NULL);
    if (result < 0) {
        set_av_error(player, "无法打开音频解码器", result);
        close_decoder(player);
        return -1;
    }

    if (player->codec->ch_layout.nb_channels > 0)
        (void)av_channel_layout_copy(&input_layout,
                                     &player->codec->ch_layout);
    else
        av_channel_layout_default(&input_layout, OUTPUT_CHANNELS);

    result = swr_alloc_set_opts2(&player->swr, &output_layout,
                                 AV_SAMPLE_FMT_S16, OUTPUT_RATE,
                                 &input_layout, player->codec->sample_fmt,
                                 player->codec->sample_rate, 0, NULL);
    av_channel_layout_uninit(&input_layout);
    if (result < 0 || player->swr == NULL) {
        set_player_error(player, "无法初始化音频重采样器");
        close_decoder(player);
        return -1;
    }

    result = swr_init(player->swr);
    if (result < 0) {
        set_av_error(player, "无法配置音频重采样器", result);
        close_decoder(player);
        return -1;
    }

    if (player->format->duration != AV_NOPTS_VALUE)
        player->duration = (double)player->format->duration / AV_TIME_BASE;
    else if (stream->duration != AV_NOPTS_VALUE)
        player->duration = (double)stream->duration *
                           av_q2d(stream->time_base);
    else
        player->duration = 0.0;

    return 0;
}

bool ffmpeg_player_compiled(void)
{
    return true;
}

FfmpegPlayer *ffmpeg_player_create(char *error, size_t error_size)
{
    FfmpegPlayer *player = calloc(1, sizeof(*player));
    pa_sample_spec sample_spec;
    pa_buffer_attr buffer_attr;
    int pulse_error = 0;

    if (error != NULL && error_size > 0)
        error[0] = '\0';

    if (player == NULL) {
        set_error_text(error, error_size, "内存不足");
        return NULL;
    }

    if (pthread_mutex_init(&player->mutex, NULL) != 0) {
        set_error_text(error, error_size, "无法初始化线程锁");
        free(player);
        return NULL;
    }
    if (pthread_cond_init(&player->cond, NULL) != 0) {
        set_error_text(error, error_size, "无法初始化线程条件变量");
        pthread_mutex_destroy(&player->mutex);
        free(player);
        return NULL;
    }

    player->volume = 80.0;
    player->running = true;
    player->idle = true;
    player->stream_index = -1;

    sample_spec.format = PA_SAMPLE_S16NE;
    sample_spec.rate = OUTPUT_RATE;
    sample_spec.channels = OUTPUT_CHANNELS;

    buffer_attr.maxlength = (uint32_t)-1;
    buffer_attr.tlength = OUTPUT_BUFFER_BYTES;
    buffer_attr.prebuf = (uint32_t)-1;
    buffer_attr.minreq = (uint32_t)-1;
    buffer_attr.fragsize = (uint32_t)-1;

    player->stream = pa_simple_new(NULL, "musicplayertui",
                                   PA_STREAM_PLAYBACK, NULL, "music",
                                   &sample_spec, NULL, &buffer_attr,
                                   &pulse_error);
    if (player->stream == NULL) {
        set_error_text(error, error_size, "无法连接 PulseAudio：%s",
                       pa_strerror(pulse_error));
        pthread_cond_destroy(&player->cond);
        pthread_mutex_destroy(&player->mutex);
        free(player);
        return NULL;
    }

    return player;
}

void ffmpeg_player_destroy(FfmpegPlayer *player)
{
    if (player == NULL)
        return;

    stop_thread(player);
    if (player->stream != NULL)
        pa_simple_free(player->stream);
    pthread_cond_destroy(&player->cond);
    pthread_mutex_destroy(&player->mutex);
    free(player);
}

int ffmpeg_player_load(FfmpegPlayer *player, const char *path)
{
    if (player == NULL)
        return -1;

    stop_thread(player);

    pthread_mutex_lock(&player->mutex);
    player->error[0] = '\0';
    player->fatal = false;
    player->running = true;
    player->idle = true;
    pthread_mutex_unlock(&player->mutex);

    if (open_decoder(player, path) != 0) {
        close_decoder(player);
        return -1;
    }

    pthread_mutex_lock(&player->mutex);
    player->idle = false;
    player->paused = false;
    player->stop = false;
    player->seek_pending = false;
    player->time_pos = 0.0;
    pthread_mutex_unlock(&player->mutex);

    player->thread_started = true;
    if (pthread_create(&player->thread, NULL, decode_thread, player) != 0) {
        player->thread_started = false;
        close_decoder(player);
        pthread_mutex_lock(&player->mutex);
        set_player_error(player, "无法创建解码线程：%s", strerror(errno));
        player->idle = true;
        pthread_mutex_unlock(&player->mutex);
        return -1;
    }

    return 0;
}

int ffmpeg_player_set_pause(FfmpegPlayer *player, bool pause)
{
    if (player == NULL)
        return -1;

    pthread_mutex_lock(&player->mutex);
    player->paused = pause;
    if (!pause)
        pthread_cond_broadcast(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

int ffmpeg_player_seek_relative(FfmpegPlayer *player, double seconds)
{
    double target;

    if (player == NULL)
        return -1;

    pthread_mutex_lock(&player->mutex);
    target = player->time_pos + seconds;
    if (target < 0.0)
        target = 0.0;
    if (player->duration > 0.0 && target > player->duration)
        target = player->duration;
    player->seek_target = target;
    player->seek_pending = true;
    pthread_cond_broadcast(&player->cond);
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

int ffmpeg_player_set_volume(FfmpegPlayer *player, double volume)
{
    if (player == NULL)
        return -1;

    if (volume < 0.0)
        volume = 0.0;
    if (volume > 130.0)
        volume = 130.0;

    pthread_mutex_lock(&player->mutex);
    player->volume = volume;
    pthread_mutex_unlock(&player->mutex);
    return 0;
}

int ffmpeg_player_stop(FfmpegPlayer *player)
{
    if (player == NULL)
        return -1;

    stop_thread(player);
    return 0;
}

int ffmpeg_player_poll(FfmpegPlayer *player, FfmpegPlayerStatus *status)
{
    bool fatal;

    if (player == NULL || status == NULL)
        return -1;

    memset(status, 0, sizeof(*status));

    pthread_mutex_lock(&player->mutex);
    status->running = player->running;
    status->idle = player->idle;
    status->paused = player->paused;
    status->time_pos = player->time_pos;
    status->duration = player->duration;
    status->volume = player->volume;
    copy_text(status->error, sizeof(status->error), player->error);
    fatal = player->fatal;
    pthread_mutex_unlock(&player->mutex);

    return fatal ? -1 : 0;
}

bool ffmpeg_player_is_running(FfmpegPlayer *player)
{
    bool running;

    if (player == NULL)
        return false;

    pthread_mutex_lock(&player->mutex);
    running = player->running && !player->fatal;
    pthread_mutex_unlock(&player->mutex);
    return running;
}

const char *ffmpeg_player_error(const FfmpegPlayer *player)
{
    if (player == NULL)
        return "FFmpeg 播放器未初始化";
    return player->error;
}

#else

bool ffmpeg_player_compiled(void)
{
    return false;
}

FfmpegPlayer *ffmpeg_player_create(char *error, size_t error_size)
{
    set_error_text(error, error_size,
                   "编译时未启用 FFmpeg 后端（缺少 libav* 或 PulseAudio 开发库）");
    return NULL;
}

void ffmpeg_player_destroy(FfmpegPlayer *player)
{
    (void)player;
}

int ffmpeg_player_load(FfmpegPlayer *player, const char *path)
{
    (void)player;
    (void)path;
    return -1;
}

int ffmpeg_player_set_pause(FfmpegPlayer *player, bool pause)
{
    (void)player;
    (void)pause;
    return -1;
}

int ffmpeg_player_seek_relative(FfmpegPlayer *player, double seconds)
{
    (void)player;
    (void)seconds;
    return -1;
}

int ffmpeg_player_set_volume(FfmpegPlayer *player, double volume)
{
    (void)player;
    (void)volume;
    return -1;
}

int ffmpeg_player_stop(FfmpegPlayer *player)
{
    (void)player;
    return -1;
}

int ffmpeg_player_poll(FfmpegPlayer *player, FfmpegPlayerStatus *status)
{
    (void)player;
    if (status != NULL)
        memset(status, 0, sizeof(*status));
    return -1;
}

bool ffmpeg_player_is_running(FfmpegPlayer *player)
{
    (void)player;
    return false;
}

const char *ffmpeg_player_error(const FfmpegPlayer *player)
{
    (void)player;
    return "FFmpeg 后端未编译";
}

#endif
