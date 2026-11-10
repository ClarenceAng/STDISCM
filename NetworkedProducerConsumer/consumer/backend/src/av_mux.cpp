#include "av_mux.h"

#include <cstdint>
#include <exception>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/pixfmt.h>
}

using namespace std;

namespace {

struct Input {
    AVFormatContext* context = nullptr;
    int streamIndex = -1;
    uint64_t packetsWithoutTime = 0;  // can't be placed in the output, so they are left out and counted

    ~Input() {
        if (context != nullptr) avformat_close_input(&context);
    }

    AVStream* stream() const { return context->streams[streamIndex]; }
};

struct Output {
    AVFormatContext* context = nullptr;

    ~Output() {
        if (context == nullptr) return;
        // pb is only still open here when the mux already failed and its file is being discarded,
        // so a failure closing it changes nothing
        if (context->pb != nullptr) avio_closep(&context->pb);
        avformat_free_context(context);
    }
};

struct Packet {
    AVPacket* packet = av_packet_alloc();

    ~Packet() { av_packet_free(&packet); }
};

string errorText(int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, text, sizeof(text));
    return text;
}

void check(int result, const string& action) {
    if (result < 0) throw runtime_error(action + ": " + errorText(result));
}

void addNote(string& notes, const string& text) {
    if (!notes.empty()) notes += "; ";
    notes += text;
}

// false only means the file has no stream of that type, anything else that goes wrong throws
bool openBestStream(Input& input, const string& path, AVMediaType type) {
    check(avformat_open_input(&input.context, path.c_str(), nullptr, nullptr), "could not open " + path);
    check(avformat_find_stream_info(input.context, nullptr), "could not read the streams of " + path);

    const int index = av_find_best_stream(input.context, type, -1, -1, nullptr, 0);
    if (index == AVERROR_STREAM_NOT_FOUND) return false;
    check(index, "could not choose a stream of " + path);
    input.streamIndex = index;
    return true;
}

// audio codecs that are both allowed in an MP4 and played by browsers
bool browserPlaysAudio(AVCodecID codec) {
    return codec == AV_CODEC_ID_AAC || codec == AV_CODEC_ID_MP3 || codec == AV_CODEC_ID_OPUS;
}

int64_t startTime(const AVStream* stream) {
    return stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
}

int64_t packetTime(const AVPacket* packet) {
    return packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
}

// false is the end of the file; a read error or damaged data throws, so it can't pass for the end
bool readStreamPacket(Input& input, AVPacket* packet) {
    while (true) {
        const int result = av_read_frame(input.context, packet);
        if (result == AVERROR_EOF) return false;
        check(result, "could not read a packet");

        if (packet->stream_index == input.streamIndex) {
            if (packetTime(packet) != AV_NOPTS_VALUE) return true;
            input.packetsWithoutTime++;
        }
        av_packet_unref(packet);
    }
}

// Damaged audio shouldn't cost the whole video, so the audio stops there,
// everything before that point is kept and the note says what happened.
bool readAudioPacket(Input& audio, AVPacket* packet, string& notes) {
    try {
        return readStreamPacket(audio, packet);
    } catch (const exception& error) {
        addNote(notes, string("the audio stops early, ") + error.what());
        return false;
    }
}

AVStream* addCopiedStream(AVFormatContext* output, const AVStream* source) {
    AVStream* stream = avformat_new_stream(output, nullptr);
    if (stream == nullptr) throw runtime_error("could not add an output stream");
    check(avcodec_parameters_copy(stream->codecpar, source->codecpar), "could not copy the stream parameters");
    stream->codecpar->codec_tag = 0;  // let the MP4 writer pick the right tag
    return stream;
}

void writePacket(AVFormatContext* output, AVPacket* packet, const AVStream* from, const AVStream* to, int64_t offset) {
    if (packet->pts != AV_NOPTS_VALUE) packet->pts -= offset;
    if (packet->dts != AV_NOPTS_VALUE) packet->dts -= offset;
    packet->stream_index = to->index;
    packet->pos = -1;
    av_packet_rescale_ts(packet, from->time_base, to->time_base);
    check(av_interleaved_write_frame(output, packet), "could not write a packet");
}

}  // namespace

MuxResult muxToMp4(const string& videoPath, const string& audioPath, const string& outputPath) {
    Input video;
    if (!openBestStream(video, videoPath, AVMEDIA_TYPE_VIDEO)) {
        throw runtime_error(videoPath + " has no video stream");
    }

    MuxResult result;
    Input audio;
    int64_t audioOffset = 0;  // in the audio stream's time base

    if (!audioPath.empty() && openBestStream(audio, audioPath, AVMEDIA_TYPE_AUDIO)) {
        const AVCodecID codec = audio.stream()->codecpar->codec_id;
        if (browserPlaysAudio(codec)) {
            result.hasAudio = true;

            // The first video frame becomes time 0, so the audio is moved by
            // that frame's start in the source file to stay in sync.
            const int sourceVideo = av_find_best_stream(audio.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (sourceVideo >= 0) {
                const AVStream* stream = audio.context->streams[sourceVideo];
                audioOffset = av_rescale_q(startTime(stream), stream->time_base, audio.stream()->time_base);
            }
        } else {
            addNote(result.note, string("audio codec ") + avcodec_get_name(codec) + " can't be copied into a browser MP4, saved without sound");
        }
    }

    Output output;
    check(avformat_alloc_output_context2(&output.context, nullptr, "mp4", outputPath.c_str()),
          "could not create the output file");

    AVStream* outVideo = addCopiedStream(output.context, video.stream());
    AVStream* outAudio = result.hasAudio ? addCopiedStream(output.context, audio.stream()) : nullptr;

    check(avio_open(&output.context->pb, outputPath.c_str(), AVIO_FLAG_WRITE), "could not open the output file");

    // puts the index at the start of the file so browsers can start playing right away
    AVDictionary* options = nullptr;
    check(av_dict_set(&options, "movflags", "+faststart", 0), "could not set the MP4 options");
    const int headerResult = avformat_write_header(output.context, &options);
    av_dict_free(&options);
    check(headerResult, "could not write the MP4 header");

    Packet videoPacket;
    Packet audioPacket;
    if (videoPacket.packet == nullptr || audioPacket.packet == nullptr) {
        throw runtime_error("out of memory");
    }

    const int64_t videoOffset = startTime(video.stream());
    bool haveVideo = readStreamPacket(video, videoPacket.packet);
    bool haveAudio = result.hasAudio && readAudioPacket(audio, audioPacket.packet, result.note);

    // read from both files in timestamp order so the output is evenly interleaved
    while (haveVideo || haveAudio) {
        const bool takeVideo = haveVideo && (!haveAudio || av_compare_ts(packetTime(videoPacket.packet), video.stream()->time_base,
                                                                         packetTime(audioPacket.packet), audio.stream()->time_base) <= 0);
        if (takeVideo) {
            writePacket(output.context, videoPacket.packet, video.stream(), outVideo, videoOffset);
            haveVideo = readStreamPacket(video, videoPacket.packet);
        } else {
            if (packetTime(audioPacket.packet) - audioOffset >= 0) {
                writePacket(output.context, audioPacket.packet, audio.stream(), outAudio, audioOffset);
            } else {
                av_packet_unref(audioPacket.packet);  // audio from before the first frame
            }
            haveAudio = readAudioPacket(audio, audioPacket.packet, result.note);
        }
    }

    check(av_write_trailer(output.context), "could not finish the MP4");
    // the last of the file is only guaranteed to be on disk once this succeeds
    check(avio_closep(&output.context->pb), "could not finish writing the MP4");

    const uint64_t skipped = video.packetsWithoutTime + audio.packetsWithoutTime;
    if (skipped > 0) {
        addNote(result.note, to_string(skipped) + " packet(s) had no timestamp and were left out");
    }
    return result;
}

bool canCopyVideoStream(const string& path) {
    Input input;
    if (!openBestStream(input, path, AVMEDIA_TYPE_VIDEO)) return false;

    const AVCodecParameters* parameters = input.stream()->codecpar;
    if (parameters->codec_id != AV_CODEC_ID_H264 || parameters->format != AV_PIX_FMT_YUV420P) return false;

    // a rotation flag is only kept by re-encoding, the copy would play sideways
    return av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX) == nullptr;
}
