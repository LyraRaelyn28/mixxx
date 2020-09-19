#include "sources/soundsourceffmpeg.h"

#include <mutex>

#include "util/logger.h"
#include "util/sample.h"

namespace mixxx {

namespace {

// FFmpeg constants

constexpr AVSampleFormat kavSampleFormat = AV_SAMPLE_FMT_FLT;

constexpr uint64_t kavChannelLayoutUndefined = 0;

constexpr int64_t kavStreamDefaultStartTime = 0;

// "AAC Audio - Encoder Delay and Synchronization: The 2112 Sample Assumption"
// https://developer.apple.com/library/ios/technotes/tn2258/_index.html
// "It must also be assumed that without an explicit value, the playback
// system will trim 2112 samples from the AAC decoder output when starting
// playback from any point in the bitsream."
// See also: https://developer.apple.com/library/archive/documentation/QuickTime/QTFF/QTFFAppenG/QTFFAppenG.html
constexpr int64_t kavStreamDecoderDelayAAC = 2112;

// Use 0-based sample frame indexing
constexpr SINT kMinFrameIndex = 0;

constexpr SINT kSamplesPerMP3Frame = 1152;

// A stream packet may produce multiple stream frames when decoded.
// Buffering more than a few codec frames with samples in advance
// should be unlikely.
// NOTE(2019-09-08, uklotzde): This is just a best guess. If the
// number of 4 is too low it would only result in some extra loop
// iterations, because the same packet needs to fed multiple
// times into the decoder.
constexpr SINT kMaxDecodedFramesPerPacket = 4;

const Logger kLogger("SoundSourceFFmpeg");

std::once_flag initFFmpegLibFlag;

// FFmpeg API Changes:
// https://github.com/FFmpeg/FFmpeg/blob/master/doc/APIchanges

// This function must be called once during startup.
void initFFmpegLib() {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
#endif
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 10, 100)
    avcodec_register_all();
#endif
}

inline int64_t getStreamChannelLayout(const AVStream& avStream) {
    auto channel_layout = avStream.codecpar->channel_layout;
    if (channel_layout == kavChannelLayoutUndefined) {
        // Workaround: FFmpeg sometimes fails to determine the channel
        // layout, e.g. for a mono WAV files with a single channel!
        channel_layout = av_get_default_channel_layout(avStream.codecpar->channels);
        kLogger.info()
                << "Unknown channel layout -> using default layout"
                << channel_layout
                << "for"
                << avStream.codecpar->channels
                << "channel(s)";
    }
    return channel_layout;
}

inline int64_t getStreamStartTime(const AVStream& avStream) {
    auto start_time = avStream.start_time;
    if (start_time == AV_NOPTS_VALUE) {
        // This case is not unlikely, e.g. happens when decoding WAV files.
        switch (avStream.codecpar->codec_id) {
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_AAC_LATM: {
            // Account for the expected decoder delay instead of simply
            // using the default start time.
            // Not all M4A files encode the start_time correctly, e.g.
            // the test file cover-test-itunes-12.7.0-aac.m4a has a valid
            // start_time of 0. Unfortunately, this special case is cannot
            // detected and compensated.
            start_time = math_max(kavStreamDefaultStartTime, kavStreamDecoderDelayAAC);
            break;
        }
        default:
            start_time = kavStreamDefaultStartTime;
        }
#if VERBOSE_DEBUG_LOG
        kLogger.debug()
                << "Unknown start time -> using default value"
                << start_time;
#endif
    }
    DEBUG_ASSERT(start_time != AV_NOPTS_VALUE);
    return start_time;
}

inline int64_t getStreamEndTime(const AVStream& avStream) {
    // The "duration" contains actually the end time of the
    // stream.
    VERIFY_OR_DEBUG_ASSERT(getStreamStartTime(avStream) <= avStream.duration) {
        // assume that the stream is empty
        return getStreamStartTime(avStream);
    }
    return avStream.duration;
}

inline SINT convertStreamTimeToFrameIndex(const AVStream& avStream, int64_t pts) {
    // getStreamStartTime(avStream) -> 1st audible frame at kMinFrameIndex
    return kMinFrameIndex +
            av_rescale_q(
                    pts - getStreamStartTime(avStream),
                    avStream.time_base,
                    (AVRational){1, avStream.codecpar->sample_rate});
}

inline int64_t convertFrameIndexToStreamTime(const AVStream& avStream, SINT frameIndex) {
    // Inverse mapping of convertStreamTimeToFrameIndex()
    return getStreamStartTime(avStream) +
            av_rescale_q(
                    frameIndex - kMinFrameIndex,
                    (AVRational){1, avStream.codecpar->sample_rate},
                    avStream.time_base);
}

IndexRange getStreamFrameIndexRange(const AVStream& avStream) {
    const auto frameIndexRange = IndexRange::between(
            convertStreamTimeToFrameIndex(avStream, getStreamStartTime(avStream)),
            convertStreamTimeToFrameIndex(avStream, getStreamEndTime(avStream)));
    DEBUG_ASSERT(frameIndexRange.orientation() != IndexRange::Orientation::Backward);
    return frameIndexRange;
}

SINT getStreamSeekPrerollFrameCount(const AVStream& avStream) {
    // Stream might not provide an appropriate value that is
    // sufficient for sample accurate decoding
    const SINT defaultSeekPrerollFrameCount =
            avStream.codecpar->seek_preroll;
    DEBUG_ASSERT(defaultSeekPrerollFrameCount >= 0);
    switch (avStream.codecpar->codec_id) {
    case AV_CODEC_ID_MP3:
    case AV_CODEC_ID_MP3ON4: {
        // In the worst case up to 29 MP3 frames need to be prerolled
        // for accurate seeking:
        // http://www.mars.org/mailman/public/mad-dev/2002-May/000634.html
        // But that would require to (re-)decode many frames after each seek
        // operation, which increases the chance that dropouts may occur.
        // As a compromise we will preroll only 9 instead of 29 frames.
        // Those 9 frames should at least drain the bit reservoir.
        //
        // NOTE(2019-09-08): Executing the decoding test with various VBR/CBR
        // MP3 files always produced exact results with only 9 preroll frames.
        // Thus increasing this number is not required and would increase
        // the risk for drop outs when jumping to a new position within
        // the file. Audible drop outs are considered more harmful than
        // slight deviations from the exact signal!
        DEBUG_ASSERT(avStream.codecpar->channels <= 2);
        const SINT mp3SeekPrerollFrameCount =
                9 * (kSamplesPerMP3Frame / avStream.codecpar->channels);
        return math_max(mp3SeekPrerollFrameCount, defaultSeekPrerollFrameCount);
    }
    case AV_CODEC_ID_AAC:
    case AV_CODEC_ID_AAC_LATM: {
        const SINT aacSeekPrerollFrameCount = kavStreamDecoderDelayAAC;
        return math_max(aacSeekPrerollFrameCount, defaultSeekPrerollFrameCount);
    }
    default:
        return defaultSeekPrerollFrameCount;
    }
}

QString formatErrorMessage(int errnum) {
    char errbuf[256];
    if (av_strerror(errnum, errbuf, sizeof(errbuf) / sizeof(errbuf[0]) == 0)) {
        return QString("%1 (%2)").arg(errbuf, errnum);
    } else {
        return QString("No description for error code (%1) found").arg(errnum);
    }
}

#if VERBOSE_DEBUG_LOG
inline void avTrace(const char* preamble, const AVPacket& avPacket) {
    kLogger.debug()
            << preamble
            << "{ stream_index" << avPacket.stream_index
            << "| pos" << avPacket.pos
            << "| size" << avPacket.size
            << "| dst" << avPacket.dts
            << "| pts" << avPacket.pts
            << "| duration" << avPacket.duration
            << '}';
}

inline void avTrace(const char* preamble, const AVFrame& avFrame) {
    kLogger.debug()
            << preamble
            << "{ channels" << avFrame.channels
            << "| channel_layout" << avFrame.channel_layout
            << "| format" << avFrame.format
            << "| sample_rate" << avFrame.sample_rate
            << "| pkt_dts" << avFrame.pkt_dts
            << "| pkt_duration" << avFrame.pkt_duration
            << "| pts" << avFrame.pts
            << "| nb_samples" << avFrame.nb_samples
            << '}';
}
#endif // VERBOSE_DEBUG_LOG

AVFormatContext* openInputFile(
        const QString& fileName) {
    // Will be allocated implicitly when opening the input file
    AVFormatContext* pavInputFormatContext = nullptr;

    // Open input file and allocate/initialize AVFormatContext
    const int avformat_open_input_result =
            avformat_open_input(
                    &pavInputFormatContext, fileName.toLocal8Bit().constData(), nullptr, nullptr);
    if (avformat_open_input_result != 0) {
        DEBUG_ASSERT(avformat_open_input_result < 0);
        kLogger.warning()
                << "avformat_open_input() failed:"
                << formatErrorMessage(avformat_open_input_result).toLocal8Bit().constData();
        DEBUG_ASSERT(pavInputFormatContext == nullptr);
    }
    return pavInputFormatContext;
}

bool openDecodingContext(
        AVCodecContext* pavCodecContext) {
    DEBUG_ASSERT(pavCodecContext != nullptr);

    const int avcodec_open2_result = avcodec_open2(pavCodecContext, pavCodecContext->codec, nullptr);
    if (avcodec_open2_result != 0) {
        DEBUG_ASSERT(avcodec_open2_result < 0);
        kLogger.warning()
                << "avcodec_open2() failed:"
                << formatErrorMessage(avcodec_open2_result).toLocal8Bit().constData();
        return false;
    }
    return true;
}

} // anonymous namespace

void SoundSourceFFmpeg::InputAVFormatContextPtr::take(
        AVFormatContext** ppavInputFormatContext) {
    DEBUG_ASSERT(ppavInputFormatContext != nullptr);
    if (m_pavInputFormatContext != *ppavInputFormatContext) {
        close();
        m_pavInputFormatContext = *ppavInputFormatContext;
        *ppavInputFormatContext = nullptr;
    }
}

void SoundSourceFFmpeg::InputAVFormatContextPtr::close() {
    if (m_pavInputFormatContext != nullptr) {
        avformat_close_input(&m_pavInputFormatContext);
        DEBUG_ASSERT(m_pavInputFormatContext == nullptr);
    }
}

//static
SoundSourceFFmpeg::AVCodecContextPtr
SoundSourceFFmpeg::AVCodecContextPtr::alloc(
        const AVCodec* codec) {
    AVCodecContextPtr context(avcodec_alloc_context3(codec));
    if (!context) {
        kLogger.warning()
                << "avcodec_alloc_context3() failed for codec"
                << codec->name;
    }
    return context;
}

void SoundSourceFFmpeg::AVCodecContextPtr::take(AVCodecContext** ppavCodecContext) {
    DEBUG_ASSERT(ppavCodecContext != nullptr);
    if (m_pavCodecContext != *ppavCodecContext) {
        close();
        m_pavCodecContext = *ppavCodecContext;
        *ppavCodecContext = nullptr;
    }
}

void SoundSourceFFmpeg::AVCodecContextPtr::close() {
    if (m_pavCodecContext != nullptr) {
        avcodec_free_context(&m_pavCodecContext);
        m_pavCodecContext = nullptr;
    }
}

void SoundSourceFFmpeg::SwrContextPtr::take(
        SwrContext** ppSwrContext) {
    DEBUG_ASSERT(m_pSwrContext != nullptr);
    if (m_pSwrContext != *ppSwrContext) {
        close();
        m_pSwrContext = *ppSwrContext;
        *ppSwrContext = nullptr;
    }
}

void SoundSourceFFmpeg::SwrContextPtr::close() {
    if (m_pSwrContext != nullptr) {
        swr_free(&m_pSwrContext);
        DEBUG_ASSERT(m_pSwrContext == nullptr);
    }
}

SoundSourceProviderFFmpeg::SoundSourceProviderFFmpeg() {
    std::call_once(initFFmpegLibFlag, initFFmpegLib);
}

QStringList SoundSourceProviderFFmpeg::getSupportedFileExtensions() const {
    QStringList list;

    // Collect all supported formats (whitelist)
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    AVInputFormat* pavInputFormat = nullptr;
    while ((pavInputFormat = av_iformat_next(pavInputFormat))) {
#else
    const AVInputFormat* pavInputFormat = nullptr;
    void* iInputFormat = 0;
    while ((pavInputFormat = av_demuxer_iterate(&iInputFormat))) {
#endif
        if (pavInputFormat->flags | AVFMT_SEEK_TO_PTS) {
            ///////////////////////////////////////////////////////////
            // Whitelist of tested codecs (including variants)
            ///////////////////////////////////////////////////////////
            if (!strcmp(pavInputFormat->name, "aac")) {
                list.append("aac");
                continue;
            } else if (!strcmp(pavInputFormat->name, "aiff")) {
                list.append("aif");
                list.append("aiff");
                continue;
            } else if (!strcmp(pavInputFormat->name, "mp3")) {
                list.append("mp3");
                continue;
            } else if (!strcmp(pavInputFormat->name, "mp4")) {
                list.append("mp4");
                continue;
            } else if (!strcmp(pavInputFormat->name, "m4v")) {
                list.append("m4v");
                continue;
            } else if (!strcmp(pavInputFormat->name, "mov,mp4,m4a,3gp,3g2,mj2")) {
                list.append("mov");
                list.append("mp4");
                list.append("m4a");
                list.append("3gp");
                list.append("3g2");
                list.append("mj2");
                continue;
            } else if (!strcmp(pavInputFormat->name, "opus") || !strcmp(pavInputFormat->name, "libopus")) {
                list.append("opus");
                continue;
            } else if (!strcmp(pavInputFormat->name, "wav")) {
                list.append("wav");
                continue;
            } else if (!strcmp(pavInputFormat->name, "wv")) {
                list.append("wv");
                continue;
                ///////////////////////////////////////////////////////////
                // Codecs with failing tests
                ///////////////////////////////////////////////////////////
                /*
            } else if (!strcmp(pavInputFormat->name, "flac")) {
                // FFmpeg failure causes test failure:
                // [flac @ 0x2ef2060] read_timestamp() failed in the middle
                // SoundSourceFFmpeg - av_seek_frame() failed: Operation not permitted
                list.append("flac");
                continue;
            } else if (!strcmp(pavInputFormat->name, "ogg")) {
                // Test failures that might be caused by FFmpeg bug:
                // https://trac.ffmpeg.org/ticket/3825
                list.append("ogg");
                continue;
            } else if (!strcmp(pavInputFormat->name, "wma") ||
                    !strcmp(pavInputFormat->name, "xwma")) {
                list.append("wma");
                continue;
            */
                ///////////////////////////////////////////////////////////
                // Untested codecs
                ///////////////////////////////////////////////////////////
                /*
            } else if (!strcmp(pavInputFormat->name, "ac3")) {
                list.append("ac3");
                continue;
            } else if (!strcmp(pavInputFormat->name, "caf")) {
                list.append("caf");
                continue;
            } else if (!strcmp(pavInputFormat->name, "mpc")) {
                list.append("mpc");
                continue;
            } else if (!strcmp(pavInputFormat->name, "mpeg")) {
                list.append("mpeg");
                continue;
            } else if (!strcmp(pavInputFormat->name, "tak")) {
                list.append("tak");
                continue;
            } else if (!strcmp(pavInputFormat->name, "tta")) {
                list.append("tta");
                continue;
            */
            }
        }
        kLogger.info()
                << "Disabling untested input format:"
                << pavInputFormat->name;
        continue;
    }

    return list;
}

SoundSourceFFmpeg::SoundSourceFFmpeg(const QUrl& url)
        : SoundSource(url),
          m_pavStream(nullptr),
          m_pavDecodedFrame(nullptr),
          m_pavResampledFrame(nullptr),
          m_seekPrerollFrameCount(0) {
}

SoundSourceFFmpeg::~SoundSourceFFmpeg() {
    close();
}

SoundSource::OpenResult SoundSourceFFmpeg::tryOpen(
        OpenMode /*mode*/,
        const OpenParams& params) {
    // Open input
    {
        AVFormatContext* pavInputFormatContext =
                openInputFile(getLocalFileName());
        if (pavInputFormatContext == nullptr) {
            kLogger.warning()
                    << "Failed to open input file"
                    << getLocalFileName();
            return OpenResult::Failed;
        }
        m_pavInputFormatContext.take(&pavInputFormatContext);
    }

    // Retrieve stream information
    const int avformat_find_stream_info_result =
            avformat_find_stream_info(m_pavInputFormatContext, nullptr);
    if (avformat_find_stream_info_result != 0) {
        DEBUG_ASSERT(avformat_find_stream_info_result < 0);
        kLogger.warning()
                << "avformat_find_stream_info() failed:"
                << formatErrorMessage(avformat_find_stream_info_result).toLocal8Bit().constData();
        return OpenResult::Failed;
    }

    // Find the best stream
    AVCodec* pDecoder = nullptr;
    const int av_find_best_stream_result = av_find_best_stream(
            m_pavInputFormatContext,
            AVMEDIA_TYPE_AUDIO,
            /*wanted_stream_nb*/ -1,
            /*related_stream*/ -1,
            &pDecoder,
            /*flags*/ 0);
    if (av_find_best_stream_result < 0) {
        switch (av_find_best_stream_result) {
        case AVERROR_STREAM_NOT_FOUND:
            kLogger.warning()
                    << "av_find_best_stream() failed to find an audio stream";
            break;
        case AVERROR_DECODER_NOT_FOUND:
            kLogger.warning()
                    << "av_find_best_stream() failed to find a decoder for any audio stream";
            break;
        default:
            kLogger.warning()
                    << "av_find_best_stream() failed:"
                    << formatErrorMessage(av_find_best_stream_result).toLocal8Bit().constData();
        }
        return SoundSource::OpenResult::Aborted;
    }
    DEBUG_ASSERT(pDecoder);

    // Select audio stream for decoding
    AVStream* pavStream = m_pavInputFormatContext->streams[av_find_best_stream_result];
    DEBUG_ASSERT(pavStream != nullptr);
    DEBUG_ASSERT(pavStream->index == av_find_best_stream_result);

    // Allocate decoding context
    AVCodecContextPtr pavCodecContext = AVCodecContextPtr::alloc(pDecoder);
    if (!pavCodecContext) {
        return SoundSource::OpenResult::Aborted;
    }

    // Configure decoding context
    const int avcodec_parameters_to_context_result =
            avcodec_parameters_to_context(pavCodecContext, pavStream->codecpar);
    if (avcodec_parameters_to_context_result != 0) {
        DEBUG_ASSERT(avcodec_parameters_to_context_result < 0);
        kLogger.warning()
                << "avcodec_parameters_to_context() failed:"
                << formatErrorMessage(avcodec_parameters_to_context_result).toLocal8Bit().constData();
        return SoundSource::OpenResult::Aborted;
    }

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 18, 100)
    // Align the time base of the context with that of the selected stream
    av_codec_set_pkt_timebase(pavCodecContext, pavStream->time_base);
#endif

    // Request output format
    pavCodecContext->request_sample_fmt = kavSampleFormat;
    if (params.getSignalInfo().getChannelCount().isValid()) {
        // A dedicated number of channels for the output signal
        // has been requested. Forward this to FFmpeg to avoid
        // manual resampling or post-processing after decoding.
        pavCodecContext->request_channel_layout =
                av_get_default_channel_layout(params.getSignalInfo().getChannelCount());
    }

    // Open decoding context
    if (!openDecodingContext(pavCodecContext)) {
        // early exit on any error
        return SoundSource::OpenResult::Failed;
    }

    // Initialize members
    m_pavCodecContext = std::move(pavCodecContext);
    m_pavStream = pavStream;

    if (kLogger.debugEnabled()) {
        kLogger.debug()
                << "Opened stream for decoding"
                << "{ index" << m_pavStream->index
                << "| id" << m_pavStream->id
                << "| codec_type" << m_pavStream->codecpar->codec_type
                << "| codec_id" << m_pavStream->codecpar->codec_id
                << "| channels" << m_pavStream->codecpar->channels
                << "| channel_layout" << m_pavStream->codecpar->channel_layout
                << "| channel_layout (fixed)" << getStreamChannelLayout(*m_pavStream)
                << "| format" << m_pavStream->codecpar->format
                << "| sample_rate" << m_pavStream->codecpar->sample_rate
                << "| bit_rate" << m_pavStream->codecpar->bit_rate
                << "| frame_size" << m_pavStream->codecpar->frame_size
                << "| initial_padding" << m_pavStream->codecpar->initial_padding
                << "| trailing_padding" << m_pavStream->codecpar->trailing_padding
                << "| seek_preroll" << m_pavStream->codecpar->seek_preroll
                << "| start_time" << m_pavStream->start_time
                << "| duration" << m_pavStream->duration
                << "| nb_frames" << m_pavStream->nb_frames
                << "| time_base" << m_pavStream->time_base.num << '/' << m_pavStream->time_base.den
                << '}';
    }

    audio::ChannelCount channelCount;
    audio::SampleRate sampleRate;
    if (!initResampling(&channelCount, &sampleRate)) {
        return OpenResult::Failed;
    }
    if (!initChannelCountOnce(channelCount)) {
        kLogger.warning()
                << "Failed to initialize number of channels"
                << channelCount;
        return OpenResult::Aborted;
    }
    if (!initSampleRateOnce(sampleRate)) {
        kLogger.warning()
                << "Failed to initialize sample rate"
                << sampleRate;
        return OpenResult::Aborted;
    }

    const auto streamBitrate =
            audio::Bitrate(m_pavStream->codecpar->bit_rate / 1000); // kbps
    if (streamBitrate.isValid() && !initBitrateOnce(streamBitrate)) {
        kLogger.warning()
                << "Failed to initialize bitrate"
                << streamBitrate;
        return OpenResult::Failed;
    }

    if (m_pavStream->duration == AV_NOPTS_VALUE) {
        // Streams with unknown or unlimited duration are
        // not (yet) supported.
        kLogger.warning()
                << "Unknown or unlimited stream duration";
        return OpenResult::Failed;
    }
    const auto streamFrameIndexRange =
            getStreamFrameIndexRange(*m_pavStream);
    VERIFY_OR_DEBUG_ASSERT(streamFrameIndexRange.start() <= streamFrameIndexRange.end()) {
        kLogger.warning()
                << "Stream with unsupported or invalid frame index range"
                << streamFrameIndexRange;
        return OpenResult::Failed;
    }

    // Decoding MP3/AAC files manually into WAV using the ffmpeg CLI and
    // comparing the audio data revealed that we need to map the nominal
    // range of the stream onto our internal range starting at kMinFrameIndex.
    // See also the discussion regarding cue point shift/offset:
    // https://mixxx.zulipchat.com/#narrow/stream/109171-development/topic/Cue.20shift.2Foffset
    const auto frameIndexRange = IndexRange::forward(
            kMinFrameIndex,
            streamFrameIndexRange.length());
    if (!initFrameIndexRangeOnce(frameIndexRange)) {
        kLogger.warning()
                << "Failed to initialize frame index range"
                << frameIndexRange;
        return OpenResult::Failed;
    }

    DEBUG_ASSERT(!m_pavDecodedFrame);
    m_pavDecodedFrame = av_frame_alloc();

    // FFmpeg does not provide sample-accurate decoding after random seeks
    // in the stream out of the box. Depending on the actual codec we need
    // to account for this and start decoding before the target position.
    m_seekPrerollFrameCount = getStreamSeekPrerollFrameCount(*m_pavStream);
#if VERBOSE_DEBUG_LOG
    kLogger.debug() << "Seek preroll frame count:" << m_seekPrerollFrameCount;
#endif

    m_frameBuffer.reinit(
            getSignalInfo(),
            // A stream packet may produce multiple stream frames when decoded
            kMaxDecodedFramesPerPacket);

    return OpenResult::Succeeded;
}

bool SoundSourceFFmpeg::initResampling(
        audio::ChannelCount* pResampledChannelCount,
        audio::SampleRate* pResampledSampleRate) {
    const auto avStreamChannelLayout =
            getStreamChannelLayout(*m_pavStream);
    const auto streamChannelCount =
            audio::ChannelCount(m_pavStream->codecpar->channels);
    // NOTE(uklotzde, 2017-09-26): Resampling to a different number of
    // channels like upsampling a mono to stereo signal breaks various
    // tests in the EngineBufferE2ETest suite!! SoundSource decoding tests
    // are unaffected, because there we always compare two signals produced
    // by the same decoder instead of a decoded with a reference signal. As
    // a workaround we decode the stream's channels as is and let Mixxx decide
    // how to handle this later.
    const auto resampledChannelCount =
            /*config.getSignalInfo().getChannelCount().isValid() ? config.getSignalInfo().getChannelCount() :*/ streamChannelCount;
    const auto avResampledChannelLayout =
            av_get_default_channel_layout(resampledChannelCount);
    const auto avStreamSampleFormat =
            m_pavCodecContext->sample_fmt;
    const auto avResampledSampleFormat =
            kavSampleFormat;
    // NOTE(uklotzde): We prefer not to change adjust sample rate here, because
    // all the frame calculations while decoding use the frame information
    // from the underlying stream! We only need resampling for up-/downsampling
    // the channels and to transform the decoded audio data into the sample
    // format that is used by Mixxx.
    const auto streamSampleRate =
            audio::SampleRate(m_pavStream->codecpar->sample_rate);
    const auto resampledSampleRate = streamSampleRate;
    if ((resampledChannelCount != streamChannelCount) ||
            (avResampledChannelLayout != avStreamChannelLayout) ||
            (avResampledSampleFormat != avStreamSampleFormat)) {
#if VERBOSE_DEBUG_LOG
        kLogger.debug()
                << "Decoded stream needs to be resampled"
                << ": channel count =" << resampledChannelCount
                << "| channel layout =" << avResampledChannelLayout
                << "| sample format =" << av_get_sample_fmt_name(avResampledSampleFormat);
#endif
        m_pSwrContext = SwrContextPtr(swr_alloc_set_opts(
                nullptr,
                avResampledChannelLayout,
                avResampledSampleFormat,
                resampledSampleRate,
                avStreamChannelLayout,
                avStreamSampleFormat,
                streamSampleRate,
                0,
                nullptr));
        if (!m_pSwrContext) {
            kLogger.warning()
                    << "Failed to allocate resampling context";
            return false;
        }
        const auto swr_init_result =
                swr_init(m_pSwrContext);
        if (swr_init_result < 0) {
            kLogger.warning()
                    << "swr_init() failed:"
                    << formatErrorMessage(swr_init_result).toLocal8Bit().constData();
            return false;
        }
        DEBUG_ASSERT(!m_pavResampledFrame);
        m_pavResampledFrame = av_frame_alloc();
    }
    // Finish initialization
    m_avStreamChannelLayout = avStreamChannelLayout;
    m_avResampledChannelLayout = avResampledChannelLayout;
    // Write output parameters
    DEBUG_ASSERT(pResampledChannelCount);
    *pResampledChannelCount = resampledChannelCount;
    DEBUG_ASSERT(pResampledSampleRate);
    *pResampledSampleRate = resampledSampleRate;
    return true;
}

void SoundSourceFFmpeg::close() {
    av_frame_free(&m_pavResampledFrame);
    DEBUG_ASSERT(!m_pavResampledFrame);
    av_frame_free(&m_pavDecodedFrame);
    DEBUG_ASSERT(!m_pavDecodedFrame);
    m_pSwrContext.close();
    m_pavCodecContext.close();
    m_pavInputFormatContext.close();
}

namespace {
SINT readNextPacket(
        AVFormatContext* pavFormatContext,
        AVStream* pavStream,
        AVPacket* pavPacket,
        SINT flushFrameIndex) {
    while (true) {
        const auto av_read_frame_result =
                av_read_frame(
                        pavFormatContext,
                        pavPacket);
        if (av_read_frame_result < 0) {
            if (av_read_frame_result == AVERROR_EOF) {
                // Enter drain mode: Flush the decoder with a final empty packet
                kLogger.debug()
                        << "EOF: Entering drain mode";
                pavPacket->stream_index = pavStream->index;
                pavPacket->data = nullptr;
                pavPacket->size = 0;
                return flushFrameIndex;
            } else {
                kLogger.warning()
                        << "av_read_frame() failed:"
                        << formatErrorMessage(av_read_frame_result).toLocal8Bit().constData();
                return ReadAheadFrameBuffer::kInvalidFrameIndex;
            }
        }
#if VERBOSE_DEBUG_LOG
        avTrace("Packet read from stream", *pavPacket);
#endif
        DEBUG_ASSERT(pavPacket->data);
        DEBUG_ASSERT(pavPacket->size > 0);
        if (pavPacket->stream_index == pavStream->index) {
            // Found a packet for the stream
            break;
        } else {
            av_packet_unref(pavPacket);
        }
    }
    DEBUG_ASSERT(pavPacket->stream_index == pavStream->index);
    return (pavPacket->pts != AV_NOPTS_VALUE)
            ? convertStreamTimeToFrameIndex(*pavStream, pavPacket->pts)
            : ReadAheadFrameBuffer::kUnknownFrameIndex;
}
} // namespace

bool SoundSourceFFmpeg::adjustCurrentPosition(SINT startIndex) {
    DEBUG_ASSERT(frameIndexRange().containsIndex(startIndex));

    if (m_frameBuffer.isReady()) {
        if (m_frameBuffer.trySeekToFirstFrame(startIndex)) {
            // Nothing to do
            return true;
        }
        m_frameBuffer.discardAllBufferedFrames();
    }

    // Need to seek to a valid start position before reading
    auto seekFrameIndex =
            math_max(kMinFrameIndex, startIndex - m_seekPrerollFrameCount);
    // Seek to codec frame boundaries if the frame size is fixed and known
    if (m_pavStream->codecpar->frame_size > 0) {
        seekFrameIndex -=
                (seekFrameIndex - kMinFrameIndex) % m_pavCodecContext->frame_size;
    }
    DEBUG_ASSERT(seekFrameIndex >= kMinFrameIndex);
    DEBUG_ASSERT(seekFrameIndex <= startIndex);

    if (!m_frameBuffer.isValid() ||
            (m_frameBuffer.firstFrame() > startIndex) ||
            (m_frameBuffer.firstFrame() < seekFrameIndex)) {
        // Flush internal decoder state
        avcodec_flush_buffers(m_pavCodecContext);
        // Seek to new position
        const int64_t seekTimestamp =
                convertFrameIndexToStreamTime(*m_pavStream, seekFrameIndex);
        int av_seek_frame_result = av_seek_frame(m_pavInputFormatContext,
                m_pavStream->index,
                seekTimestamp,
                AVSEEK_FLAG_BACKWARD);
        if (av_seek_frame_result < 0) {
            // Unrecoverable seek error: Invalidate the current position and abort
            kLogger.warning()
                    << "av_seek_frame() failed:"
                    << formatErrorMessage(av_seek_frame_result)
                               .toLocal8Bit()
                               .constData();
            m_frameBuffer.invalidate();
            return false;
        }
    }

    // The current position remains unknown until actually reading data
    // from the stream
    m_frameBuffer.reset();

    return true;
}

bool SoundSourceFFmpeg::consumeNextAVPacket(
        AVPacket* pavPacket, AVPacket** ppavNextPacket) {
    DEBUG_ASSERT(pavPacket);
    DEBUG_ASSERT(ppavNextPacket);
    if (!*ppavNextPacket) {
        // Read next packet from stream
        const SINT packetFrameIndex = readNextPacket(m_pavInputFormatContext,
                m_pavStream,
                pavPacket,
                m_frameBuffer.firstFrame());
        if (packetFrameIndex == ReadAheadFrameBuffer::kInvalidFrameIndex) {
            // Invalidate current position and abort reading
            m_frameBuffer.invalidate();
            return false;
        }
        *ppavNextPacket = pavPacket;
    }
    auto pavNextPacket = *ppavNextPacket;

    // Consume raw packet data
#if VERBOSE_DEBUG_LOG
    avTrace("Sending packet to decoder", *pavNextPacket);
#endif
    const auto avcodec_send_packet_result =
            avcodec_send_packet(m_pavCodecContext, pavNextPacket);
    if (avcodec_send_packet_result == 0) {
        // Packet has been consumed completely
#if VERBOSE_DEBUG_LOG
        kLogger.debug() << "Packet has been consumed by decoder";
#endif
        // Release ownership on packet
        av_packet_unref(pavNextPacket);
        *ppavNextPacket = nullptr;
    } else {
        // Packet has not been consumed or only partially
        if (avcodec_send_packet_result == AVERROR(EAGAIN)) {
            // Keep and resend this packet to the decoder during the next round
#if VERBOSE_DEBUG_LOG
            kLogger.debug() << "Packet needs to be sent again to decoder";
#endif
        } else {
            kLogger.warning()
                    << "avcodec_send_packet() failed:"
                    << formatErrorMessage(avcodec_send_packet_result)
                               .toLocal8Bit()
                               .constData();
            // Release ownership on packet
            av_packet_unref(pavNextPacket);
            *ppavNextPacket = nullptr;
            // Invalidate current position and abort reading
            m_frameBuffer.invalidate();
            return false;
        }
    }
    return true;
}

const CSAMPLE* SoundSourceFFmpeg::resampleDecodedAVFrame() {
    if (m_pSwrContext) {
        // Decoded frame must be resampled before reading
        m_pavResampledFrame->channel_layout = m_avResampledChannelLayout;
        m_pavResampledFrame->sample_rate = getSignalInfo().getSampleRate();
        m_pavResampledFrame->format = kavSampleFormat;
        if (m_pavDecodedFrame->channel_layout == kavChannelLayoutUndefined) {
            // Sometimes the channel layout is undefined.
            m_pavDecodedFrame->channel_layout = m_avStreamChannelLayout;
        }
#if VERBOSE_DEBUG_LOG
        avTrace("Resampling decoded frame", *m_pavDecodedFrame);
#endif
        const auto swr_convert_frame_result = swr_convert_frame(
                m_pSwrContext, m_pavResampledFrame, m_pavDecodedFrame);
        if (swr_convert_frame_result != 0) {
            kLogger.warning()
                    << "swr_convert_frame() failed:"
                    << formatErrorMessage(swr_convert_frame_result)
                               .toLocal8Bit()
                               .constData();
            // Discard decoded frame and abort after unrecoverable error
            av_frame_unref(m_pavDecodedFrame);
            return nullptr;
        }
#if VERBOSE_DEBUG_LOG
        avTrace("Received resampled frame", *m_pavResampledFrame);
#endif
        DEBUG_ASSERT(m_pavDecodedFrame->pts = m_pavResampledFrame->pts);
        DEBUG_ASSERT(m_pavDecodedFrame->nb_samples =
                             m_pavResampledFrame->nb_samples);
        return reinterpret_cast<const CSAMPLE*>(
                m_pavResampledFrame->extended_data[0]);
    } else {
        return reinterpret_cast<const CSAMPLE*>(
                m_pavDecodedFrame->extended_data[0]);
    }
}

ReadableSampleFrames SoundSourceFFmpeg::readSampleFramesClamped(
        WritableSampleFrames writableSampleFrames) {
    DEBUG_ASSERT(m_frameBuffer.signalInfo() == getSignalInfo());
    const SINT readableStartIndex =
            writableSampleFrames.frameIndexRange().start();
    const CSAMPLE* readableData = writableSampleFrames.writableData();

    // Consume all buffered sample data before decoding any new data
    writableSampleFrames = m_frameBuffer.consumeBufferedFrames(writableSampleFrames);

    // Skip decoding if all data has been read
    auto writableFrameRange = writableSampleFrames.frameIndexRange();
    DEBUG_ASSERT(writableFrameRange <= frameIndexRange());
    if (writableFrameRange.empty()) {
        auto readableRange = IndexRange::between(
                readableStartIndex, writableFrameRange.start());
        DEBUG_ASSERT(readableRange.orientation() != IndexRange::Orientation::Backward);
        const auto readableSampleCount =
                getSignalInfo().frames2samples(readableRange.length());
        return ReadableSampleFrames(readableRange,
                SampleBuffer::ReadableSlice(readableData, readableSampleCount));
    }

    // Adjust the current position
    if (!adjustCurrentPosition(writableFrameRange.start())) {
        // Abort reading on seek errors
        return ReadableSampleFrames();
    }

    // Start decoding into the output buffer from the current position
    CSAMPLE* pOutputSampleBuffer = writableSampleFrames.writableData();

    AVPacket avPacket;
    av_init_packet(&avPacket);
    avPacket.data = nullptr;
    avPacket.size = 0;
    AVPacket* pavNextPacket = nullptr;
    auto readFrameIndex = m_frameBuffer.firstFrame();
    while (m_frameBuffer.isValid() &&                         // no decoding error occurred
            (pavNextPacket || !writableFrameRange.empty()) && // not yet finished
            consumeNextAVPacket(&avPacket, &pavNextPacket)) { // next packet consumed
        int avcodec_receive_frame_result;
        // One or more AV packets are required for decoding the next
        // AV frame.
        do {
#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "writableFrameRange" << writableFrameRange
                    << "m_frameBuffer.bufferedFrameRange()"
                    << m_frameBuffer.bufferedFrameRange();
#endif

            DEBUG_ASSERT(writableFrameRange.empty() || m_frameBuffer.isEmpty());

            // Decode next frame
            IndexRange decodedFrameRange;
            avcodec_receive_frame_result =
                    avcodec_receive_frame(m_pavCodecContext, m_pavDecodedFrame);
            if (avcodec_receive_frame_result == 0) {
#if VERBOSE_DEBUG_LOG
                avTrace("Received decoded frame", *m_pavDecodedFrame);
#endif
                DEBUG_ASSERT(m_pavDecodedFrame->pts != AV_NOPTS_VALUE);
                const auto decodedFrameCount = m_pavDecodedFrame->nb_samples;
                DEBUG_ASSERT(decodedFrameCount > 0);
                const auto streamFrameIndex =
                        convertStreamTimeToFrameIndex(
                                *m_pavStream, m_pavDecodedFrame->pts);
                decodedFrameRange = IndexRange::forward(
                        streamFrameIndex,
                        decodedFrameCount);
                if (readFrameIndex == ReadAheadFrameBuffer::kUnknownFrameIndex) {
                    readFrameIndex = decodedFrameRange.start();
                }
            } else if (avcodec_receive_frame_result == AVERROR(EAGAIN)) {
#if VERBOSE_DEBUG_LOG
                kLogger.debug()
                        << "No more frames available until decoder is fed with "
                           "more packets from stream";
#endif
                DEBUG_ASSERT(!pavNextPacket);
                break;
            } else if (avcodec_receive_frame_result == AVERROR_EOF) {
                DEBUG_ASSERT(!pavNextPacket);
                if (readFrameIndex != ReadAheadFrameBuffer::kUnknownFrameIndex) {
                    DEBUG_ASSERT(m_frameBuffer.isEmpty());
                    // Due to the lead-in with a start_time > 0 some encoded
                    // files are shorter then actually reported. This may vary
                    // depending on the encoder version, because sometimes the
                    // lead-in is included in the stream's duration and sometimes
                    // not. Short periods of silence at the end of a track are
                    // acceptable in favor of a consistent handling of the lead-in,
                    // because they may affect only the position of the outro end
                    // point and not any other position markers!
                    kLogger.debug()
                            << "Stream ends at sample frame"
                            << readFrameIndex
                            << "instead of"
                            << frameIndexRange().end()
                            << "-> padding with silence";
                    if (!writableFrameRange.empty()) {
                        DEBUG_ASSERT(readFrameIndex < writableFrameRange.end());
                        const auto clearSampleCount =
                                getSignalInfo().frames2samples(
                                        writableFrameRange.length());
                        if (pOutputSampleBuffer) {
                            SampleUtil::clear(
                                    pOutputSampleBuffer, clearSampleCount);
                            pOutputSampleBuffer += clearSampleCount;
                        }
                        writableFrameRange.shrinkFront(writableFrameRange.length());
                    }
                }
                // Invalidate current position and abort reading
                m_frameBuffer.invalidate();
                break;
            } else {
                kLogger.warning()
                        << "avcodec_receive_frame() failed:"
                        << formatErrorMessage(avcodec_receive_frame_result)
                                   .toLocal8Bit()
                                   .constData();
                // Invalidate current position and abort reading
                m_frameBuffer.invalidate();
                break;
            }

#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "After receiving decoded sample data:"
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "decodedFrameRange" << decodedFrameRange
                    << "writableFrameRange" << writableFrameRange;
#endif
            DEBUG_ASSERT(readFrameIndex != ReadAheadFrameBuffer::kInvalidFrameIndex);
            DEBUG_ASSERT(readFrameIndex != ReadAheadFrameBuffer::kUnknownFrameIndex);
            DEBUG_ASSERT(!decodedFrameRange.empty());

            if (decodedFrameRange.start() < readFrameIndex) {
                // The next frame starts BEFORE the current position
                const auto overlapRange = IndexRange::between(
                        decodedFrameRange.start(), readFrameIndex);
                // NOTE(2019-02-08, uklotzde): Overlapping frames at the
                // beginning of an audio stream before the first readable
                // sample frame at kMinFrameIndex are expected. For example
                // this happens when decoding 320kbps MP3 files where
                // decoding starts at position -1105 and the first 1105
                // decoded samples need to be skipped.
                if (readFrameIndex > kMinFrameIndex) {
                    kLogger.warning()
                            << "Overlapping sample frames in the stream:"
                            << overlapRange;
                }
                const auto consumedRange = IndexRange::between(
                        writableSampleFrames.frameIndexRange().start(),
                        // We might still be decoding samples in preroll mode, i.e.
                        // readFrameIndex < writableSampleFrames.frameIndexRange().start()
                        math_max(readFrameIndex,
                                writableSampleFrames.frameIndexRange()
                                        .start()));
                auto rewindRange = intersect(overlapRange, consumedRange);
                if (!rewindRange.empty()) {
                    DEBUG_ASSERT(rewindRange.end() == readFrameIndex);
                    kLogger.warning()
                            << "Rewinding current position:"
                            << readFrameIndex
                            << "->"
                            << rewindRange.start();
                    // Rewind internally buffered samples first...
                    const auto rewindFrameCount =
                            m_frameBuffer.discardLastBufferedFrames(rewindRange.length());
                    rewindRange.shrinkBack(rewindFrameCount);
                    // ...then rewind remaining samples from the output buffer
                    if (pOutputSampleBuffer) {
                        pOutputSampleBuffer -= getSignalInfo().frames2samples(
                                rewindRange.length());
                    }
                    writableFrameRange = IndexRange::between(
                            rewindRange.start(), writableFrameRange.end());
                    DEBUG_ASSERT(writableFrameRange.orientation() !=
                            IndexRange::Orientation::Backward);
                }
                // Adjust read position
                readFrameIndex = decodedFrameRange.start();
            }

#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "Before resampling:"
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "decodedFrameRange" << decodedFrameRange
                    << "writableFrameRange" << writableFrameRange;
#endif

            const CSAMPLE* pDecodedSampleData = resampleDecodedAVFrame();
            if (!pDecodedSampleData) {
                // Invalidate current position and abort reading after unrecoverable error
                m_frameBuffer.invalidate();
                break;
            }

            //                 readFrameIndex
            //                       |
            //                       v
            //      | missing frames | skipped frames |<- decodedFrameRange ->|
            //      ^
            //      |
            // writableFrameRange.start()

            // -= 1st step =-
            // Advance writableFrameRange.start() towards readFrameIndex
            // if behind and fill with missing sample frames with silence
            if (writableFrameRange.start() < readFrameIndex) {
                const auto missingFrameRange =
                        IndexRange::between(
                                writableFrameRange.start(),
                                math_min(readFrameIndex, writableFrameRange.end()));
                DEBUG_ASSERT(missingFrameRange.orientation() !=
                        IndexRange::Orientation::Backward);
                kLogger.warning()
                        << "Generating silence for missing sample data"
                        << missingFrameRange;
                const auto clearFrameCount =
                        missingFrameRange.length();
                const auto clearSampleCount =
                        getSignalInfo().frames2samples(missingFrameRange.length());
                if (pOutputSampleBuffer) {
                    SampleUtil::clear(
                            pOutputSampleBuffer,
                            clearSampleCount);
                    pOutputSampleBuffer += clearSampleCount;
                }
                writableFrameRange.shrinkFront(clearFrameCount);
            }
            DEBUG_ASSERT(writableFrameRange.empty() ||
                    writableFrameRange.start() >= readFrameIndex);

            // -= 2nd step =-
            // Check for skipped sample data and log a message.
            // Nothing to do here, because the skipped samples will be discarded
            // during the following two steps. How to actually handle these range
            // of unavailable samples depends on the relative position of
            // writableFrameRange.
            DEBUG_ASSERT(readFrameIndex <= decodedFrameRange.start());
            const auto skippedFrameRange =
                    IndexRange::between(
                            readFrameIndex,
                            decodedFrameRange.start());
            if (!skippedFrameRange.empty()) {
                // The decoder has skipped some sample data that needs to
                // be filled with silence to continue decoding! This is supposed
                // to occur only at the beginning of a stream for the very first
                // decoded frame with a lead-in due to start_time > 0. But not all
                // encoded streams seem to account for this by correctly setting
                // the start_time property.
                // NOTE: Decoding might even start at a negative position for the
                // first frame of the file, i.e. outside of the track's valid range!
                // Consequently isValidFrameIndex(readFrameIndex) might return false.
                // This is expected behavior and will be compensated during 'preskip'
                // (see below).
                if (readFrameIndex <= frameIndexRange().start()) {
                    kLogger.debug()
                            << "Generating silence for skipped sample data"
                            << skippedFrameRange
                            << "at the start of the audio stream";
                } else {
                    kLogger.warning()
                            << "Generating silence for skipped sample data"
                            << skippedFrameRange;
                }
            }

#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "Before discarding excessive sample data:"
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "decodedFrameRange" << decodedFrameRange
                    << "writableFrameRange" << writableFrameRange;
#endif

            // -= 3rd step =-
            // Discard both skipped and decoded frames that do not overlap
            // with writableFrameRange, i.e. that precede writableFrameRange.
            if (writableFrameRange.start() > readFrameIndex) {
                //                 readFrameIndex
                //                       |
                //                       v
                //      | missing frames | skipped frames |<- decodedFrameRange ->|
                //                                   ^               ^                 ^
                //                                   |...            |...              |....
                //                         writableFrameRange.start()
                const auto excessiveFrameRange =
                        IndexRange::between(
                                decodedFrameRange.start(),
                                math_min(
                                        writableFrameRange.start(),
                                        decodedFrameRange.end()));
                if (excessiveFrameRange.orientation() == IndexRange::Orientation::Forward) {
#if VERBOSE_DEBUG_LOG
                    kLogger.debug()
                            << "Discarding excessive sample data:"
                            << excessiveFrameRange;
#endif
                    const auto excessiveFrameCount = excessiveFrameRange.length();
                    pDecodedSampleData += getSignalInfo().frames2samples(
                            excessiveFrameCount);
                    decodedFrameRange.shrinkFront(
                            excessiveFrameCount);
                }
                // Reset readFrameIndex beyond both skippedFrameRange
                // and excessiveFrameRange
                DEBUG_ASSERT(readFrameIndex <= excessiveFrameRange.end());
                readFrameIndex = excessiveFrameRange.end();
                if (decodedFrameRange.empty()) {
                    // Skip the remaining loop body
                    m_frameBuffer.reset(readFrameIndex);
                    continue;
                }
            }

#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "Before consuming skipped and decoded sample data:"
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "decodedFrameRange" << decodedFrameRange
                    << "writableFrameRange" << writableFrameRange;
#endif

            // -= 4th step =-
            // Consume all sample data from both skipped and decoded
            // ranges that overlap with writableFrameRange, i.e. that
            // are supposed to be consumed.
            DEBUG_ASSERT(readFrameIndex <= decodedFrameRange.start());
            if (!writableFrameRange.empty()) {
                const auto skippableFrameRange =
                        IndexRange::between(
                                writableFrameRange.start(),
                                math_min(decodedFrameRange.start(), writableFrameRange.end()));
                if (skippableFrameRange.orientation() == IndexRange::Orientation::Forward) {
                    // Fill the gap of skipped frames until the first available
                    // decoded frame with silence
#if VERBOSE_DEBUG_LOG
                    kLogger.debug()
                            << "Consuming skipped sample data by generating silence:"
                            << skippableFrameRange;
#endif
                    const auto clearFrameCount =
                            skippableFrameRange.length();
                    const auto clearSampleCount =
                            getSignalInfo().frames2samples(clearFrameCount);
                    if (pOutputSampleBuffer) {
                        SampleUtil::clear(
                                pOutputSampleBuffer,
                                clearSampleCount);
                        pOutputSampleBuffer += clearSampleCount;
                    }
                    writableFrameRange.shrinkFront(clearFrameCount);
                    readFrameIndex += clearFrameCount;
                }
            }
            DEBUG_ASSERT(writableFrameRange.empty() ||
                    readFrameIndex == decodedFrameRange.start());
            readFrameIndex = decodedFrameRange.start();
            if (!writableFrameRange.empty()) {
                DEBUG_ASSERT(writableFrameRange.start() == decodedFrameRange.start());
                const auto copyableFrameRange =
                        IndexRange::between(
                                readFrameIndex,
                                math_min(decodedFrameRange.end(), writableFrameRange.end()));
                if (copyableFrameRange.orientation() == IndexRange::Orientation::Forward) {
                    // Copy the decoded samples into the output buffer
#if VERBOSE_DEBUG_LOG
                    kLogger.debug()
                            << "Consuming decoded sample data:"
                            << copyableFrameRange;
#endif
                    const auto copyFrameCount =
                            copyableFrameRange.length();
                    const auto copySampleCount =
                            getSignalInfo().frames2samples(copyFrameCount);
                    if (pOutputSampleBuffer) {
                        SampleUtil::copy(
                                pOutputSampleBuffer,
                                pDecodedSampleData,
                                copySampleCount);
                        pOutputSampleBuffer += copySampleCount;
                    }
                    pDecodedSampleData += copySampleCount;
                    decodedFrameRange.shrinkFront(copyFrameCount);
                    writableFrameRange.shrinkFront(copyFrameCount);
                    readFrameIndex += copyFrameCount;
                }
            }

            // Store the current stream position before buffering
            // the remaining sample data
            m_frameBuffer.reset(readFrameIndex);

#if VERBOSE_DEBUG_LOG
            kLogger.debug()
                    << "Before buffering skipped and decoded sample data:"
                    << "m_frameBuffer.firstFrame()" << m_frameBuffer.firstFrame()
                    << "readFrameIndex" << readFrameIndex
                    << "decodedFrameRange" << decodedFrameRange
                    << "writableFrameRange" << writableFrameRange;
#endif

            // Buffer remaining unread sample data
            const auto unbufferedSampleFrames = m_frameBuffer.bufferFrames(
                    ReadAheadFrameBuffer::BufferingMode::FillGapWithSilence,
                    ReadableSampleFrames(
                            decodedFrameRange,
                            SampleBuffer::ReadableSlice(
                                    pDecodedSampleData,
                                    getSignalInfo().frames2samples(decodedFrameRange.length()))));
            Q_UNUSED(unbufferedSampleFrames) // only used for assertion
            DEBUG_ASSERT(unbufferedSampleFrames.frameLength() == 0);
            if (m_frameBuffer.bufferedFrameRange().end() > frameIndexRange().end()) {
                // NOTE(2019-09-08, uklotzde): For some files (MP3 VBR, Lavf AAC)
                // FFmpeg may decode a few more samples than expected! Simply discard
                // those trailing samples, because we are not prepared to adjust the
                // duration of the stream later.
                const auto overflowFrameCount =
                        m_frameBuffer.bufferedFrameRange().end() - frameIndexRange().end();
                kLogger.info()
                        << "Discarding"
                        << overflowFrameCount
                        << "sample frames at the end of the audio stream";
                m_frameBuffer.discardLastBufferedFrames(overflowFrameCount);
            }

            // Housekeeping before next decoding iteration
            av_frame_unref(m_pavDecodedFrame);
            av_frame_unref(m_pavResampledFrame);
        } while (avcodec_receive_frame_result == 0 &&
                m_frameBuffer.isValid());
    }
    DEBUG_ASSERT(!pavNextPacket);

    auto readableRange =
            IndexRange::between(
                    readableStartIndex,
                    writableFrameRange.start());
    return ReadableSampleFrames(
            readableRange,
            SampleBuffer::ReadableSlice(
                    readableData,
                    getSignalInfo().frames2samples(readableRange.length())));
}

QString SoundSourceProviderFFmpeg::getName() const {
    return "FFmpeg";
}

SoundSourceProviderPriority SoundSourceProviderFFmpeg::getPriorityHint(
        const QString& /*supportedFileExtension*/) const {
    // TODO: Increase priority to HIGHER if FFmpeg should be used as the
    // default decoder instead of other SoundSources?
    // Currently it is only used as a fallback after all other SoundSources
    // failed to open a file or are otherwise unavailable.
    return SoundSourceProviderPriority::LOWEST;
}

} // namespace mixxx
