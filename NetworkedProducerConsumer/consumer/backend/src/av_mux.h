#pragma once

#include <string>

struct MuxResult {
    bool hasAudio = false;
    std::string note;  // anything lost along the way: audio that was left out or cut short, packets skipped
};

// Writes an MP4 holding the video stream of videoPath and the audio stream of
// audioPath, copied without re-encoding. audioPath may be empty for no audio.
// A source with no audio track is not a problem. Throws std::runtime_error if
// a file can't be read or the MP4 can't be written.
MuxResult muxToMp4(const std::string& videoPath, const std::string& audioPath, const std::string& outputPath);

// true if the file's own video stream can be put in an MP4 and played by browsers.
// false is an answer about the stream; a file that can't be read throws std::runtime_error.
bool canCopyVideoStream(const std::string& path);
