/**
 * @file    engine/movie/sfd_video.h
 * @brief   Host decoding of the video in Sofdec (.sfd) movies with FFmpeg.
 *
 * A .sfd is an MPEG program stream: MPEG-1 video on stream 0xE0 and CRI ADX
 * audio on 0xC0-0xC3. This reads the video out of one or more files played
 * back to back, splits it into pictures and decodes them on its own thread,
 * keeping a few frames ahead of the one asked for. It knows nothing of the
 * guest: engine/movie/native_movie.cpp asks it for the picture the game's
 * player is about to show.
 *
 * Pictures are numbered from 0 in stream (decode) order across all the files,
 * which is how the game's decoder meets them too. The files repeat once the
 * last one ends: the game loops its seamless lists, and decoding a little past
 * the end of a movie that does not loop costs nothing.
 */
#pragma once

#include <rex/types.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace bd::movie {

// A decoded picture, planar 4:2:0. Kept as FFmpeg's own frame so nothing is
// copied until it goes into guest memory.
class Picture {
public:
  explicit Picture(AVFrame *frame) : frame_(frame) {}
  ~Picture();
  Picture(const Picture &) = delete;
  Picture &operator=(const Picture &) = delete;

  u32 Width() const;
  u32 Height() const;
  // Plane 0 is Y, 1 is Cb (U), 2 is Cr (V).
  const u8 *Plane(int plane) const;
  int Stride(int plane) const;

  u64 decodeIndex = 0;  // place in stream order
  u64 displayIndex = 0; // place in display order
  int pictureType = 0;  // 1 I, 2 P, 3 B

private:
  AVFrame *frame_;
};

struct StreamStats {
  u64 decoded = 0;     // pictures FFmpeg returned
  u64 errors = 0;      // packets FFmpeg rejected
  double decodeSeconds = 0;
  u32 width = 0, height = 0;
};

class SfdVideo {
public:
  SfdVideo();
  ~SfdVideo(); // stops the decoder thread
  SfdVideo(const SfdVideo &) = delete;
  SfdVideo &operator=(const SfdVideo &) = delete;

  // Checks that every file opens and that the first one has MPEG-1 video,
  // and reads its picture size. False with 'error' set otherwise.
  bool Open(std::vector<std::string> paths, std::string &error);
  // Runs 'setup' on the new thread first (name, core).
  void Start(std::function<void()> setup = {});

  // Files entered after Start (a seamless list growing during playback).
  void Append(std::string path);

  // The picture that was the 'index'-th in stream order, waiting up to
  // 'waitMs' for the decoder to reach it. Null when it does not come in time
  // or the stream has failed. Pictures before it in display order are freed.
  std::shared_ptr<const Picture> ByDecodeIndex(u64 index, int waitMs);

  u32 Width() const { return width_; }
  u32 Height() const { return height_; }
  bool Failed() const { return failed_.load(std::memory_order_relaxed); }
  std::string LastError() const;
  StreamStats Stats() const;

private:
  struct Unit; // one picture's bytes, with the headers in front of it

  enum class FillResult { Data, FileEnd, Error };

  void Run();
  FillResult Fill(); // demux more of the elementary stream
  bool NextUnit(std::vector<u8> &out, int &type, bool &fileEnd);
  bool Decode(const std::vector<u8> *unit, int type); // null flushes
  void Fail(std::string error);

  // Demuxer.
  std::vector<std::string> paths_;
  size_t fileIndex_ = 0;
  std::FILE *file_ = nullptr;
  std::vector<u8> ps_;   // program stream bytes not yet parsed
  size_t psPos_ = 0;
  bool psEnd_ = false;   // the current file is exhausted
  bool fileEndReported_ = false;
  std::vector<u8> es_;   // video elementary stream not yet split
  size_t esScan_ = 0;    // next byte of es_ to look at for start codes
  ptrdiff_t unitStart_ = -1;
  ptrdiff_t headerStart_ = -1;
  int unitType_ = 0;
  u64 nextDecodeIndex_ = 0;
  u64 nextDisplayIndex_ = 0;
  u64 loops_ = 0;

  // Decoder.
  AVCodecContext *codec_ = nullptr;
  AVPacket *packet_ = nullptr;
  AVFrame *frame_ = nullptr;

  u32 width_ = 0, height_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable produced_, consumed_;
  std::deque<std::shared_ptr<Picture>> ready_; // display order
  std::vector<std::string> appended_;
  u64 wanted_ = 0;  // decode index last asked for
  int waiting_ = 0; // callers waiting for a frame
  bool stop_ = false;
  std::atomic<bool> failed_{false};
  std::string error_;
  StreamStats stats_;
  std::thread thread_;
};

} // namespace bd::movie
