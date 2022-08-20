#include "AudioDecoder.h"
#include "../utils/Logger.h"

AudioDecoder::AudioDecoder(int index, AVFormatContext *ftx): BaseDecoder(index, ftx) {
    LOGI("AudioDecoder")
}

AudioDecoder::~AudioDecoder() {
    LOGI("~AudioDecoder")
}

bool AudioDecoder::prepare() {
    AVCodecParameters *params = mFtx->streams[getStreamIndex()]->codecpar;

    mAudioCodec = avcodec_find_decoder(params->codec_id);
    if (mAudioCodec == nullptr) {
        std::string msg = "[audio] not find audio decoder";
        if (mErrorMsgListener) {
            mErrorMsgListener(-1000, msg);
        }
        return false;
    }

    // init codec context
    mAudioCodecContext = avcodec_alloc_context3(mAudioCodec);
    if (!mAudioCodecContext) {
        std::string msg = "[audio] codec context alloc failed";
        if (mErrorMsgListener) {
            mErrorMsgListener(-2000, msg);
        }
        return false;
    }
    avcodec_parameters_to_context(mAudioCodecContext, params);

    // open codec
    int ret = avcodec_open2(mAudioCodecContext, mAudioCodec, nullptr);
    if (ret != 0) {
        std::string msg = "[audio] codec open failed";
        if (mErrorMsgListener) {
            mErrorMsgListener(-3000, msg);
        }
        return false;
    }

    mAvFrame = av_frame_alloc();

    mSwrContext = swr_alloc_set_opts(
            nullptr,
            AV_CH_LAYOUT_STEREO,
            AV_SAMPLE_FMT_S16,
            44100,

            (int64_t)mAudioCodecContext->channel_layout,
            mAudioCodecContext->sample_fmt,
            mAudioCodecContext->sample_rate,
            0,
            nullptr
            );
    ret = swr_init(mSwrContext);

    LOGI("[audio] prepare, sample rate: %d, channels: %d, channel_layout: %" PRId64 ", fmt: %d, swr_init: %d",
         mAudioCodecContext->sample_rate, mAudioCodecContext->channels, mAudioCodecContext->channel_layout, mAudioCodecContext->sample_fmt, ret);

    mStartTime = -1;

    return ret == 0;
}

int AudioDecoder::decode(AVPacket *avPacket) {
    int res = avcodec_send_packet(mAudioCodecContext, avPacket);
    LOGI("[audio] avcodec_send_packet...pts: %" PRId64 ", dts: %" PRId64 ", res: %d", avPacket->pts, avPacket->dts, res)

    res = avcodec_receive_frame(mAudioCodecContext, mAvFrame);
    if (res != 0) {
        LOGE("[audio] avcodec_receive_frame err: %d", res)
        av_frame_unref(mAvFrame);
        return res;
    }
    auto pts = mAvFrame->pts * av_q2d(mFtx->streams[getStreamIndex()]->time_base) * 1000;
    LOGI("[audio] avcodec_receive_frame...pts: %" PRId64 ", time: %f, best_effort_timestamp: %" PRId64, mAvFrame->pts, pts, mAvFrame->best_effort_timestamp)

    int out_nb = (int) av_rescale_rnd(mAvFrame->nb_samples, 44100, mAvFrame->sample_rate, AV_ROUND_UP);
    int out_channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
    int size = av_samples_get_buffer_size(nullptr, out_channels, out_nb, AV_SAMPLE_FMT_S16, 1);
    if (mAudioBuffer == nullptr) {
        LOGI("audio frame, out_channels: %d, out_nb: %d, size: %d, ", out_channels, out_nb, size)
        mAudioBuffer = (uint8_t *) av_malloc(size);
    }

    int nb = swr_convert(
            mSwrContext,
            &mAudioBuffer,
            size / out_channels,
            (const uint8_t**)mAvFrame->data,
            mAvFrame->nb_samples
            );

    mDataSize = 0;
    if (nb > 0) {
        mDataSize = nb * out_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
        if (mOnFrameArrivedListener != nullptr) {
            mOnFrameArrivedListener(mAvFrame);
        }
    }
    LOGI("swr_convert, dataSize: %d, nb: %d, out_channels: %d", mDataSize, nb, out_channels)

    return 0;
}

void AudioDecoder::release() {
    if (mAudioBuffer != nullptr) {
        av_free(mAudioBuffer);
        mAudioBuffer = nullptr;
        LOGI("[audio] buffer...release")
    }

    if (mAvFrame != nullptr) {
        av_frame_free(&mAvFrame);
        av_freep(&mAvFrame);
        LOGI("[audio] av frame...release")
    }

    if (mSwrContext != nullptr) {
        swr_free(&mSwrContext);
        mSwrContext = nullptr;
        LOGI("[audio] sws context...release")
    }

    if (mAudioCodecContext != nullptr) {
        avcodec_close(mAudioCodecContext);
        avcodec_free_context(&mAudioCodecContext);
        mAudioCodecContext = nullptr;
        LOGI("[audio] codec...release")
    }
}

void AudioDecoder::avSync(AVFrame *frame) {
    int64_t pts = frame->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) {
        pts = 0;
        LOGE("AV_NOPTS_VALUE")
    }
    // s -> us
    pts = pts * (int64_t)(av_q2d(mTimeBase) * 1000 * 1000);
    int64_t elapsedTime;
    if (mStartTime < 0) {
        mStartTime = av_gettime();
        elapsedTime = 0;
    } else {
        elapsedTime = av_gettime() - mStartTime;
    }
    int64_t diff = pts - elapsedTime;
    diff = FFMIN(diff, DELAY_THRESHOLD);
    LOGI("[audio] avSync, pts: %" PRId64 ", elapsedTime: %" PRId64 " diff: %" PRId64 "ms", pts, elapsedTime, (diff / 1000))
    if (diff > 0) {
        av_usleep(diff);
    }
}
