/**
 * @file    engine/movie/sfd_video.cpp
 * @brief   Host decoding of the video in Sofdec (.sfd) movies. See sfd_video.h.
 */
#include "engine/movie/sfd_video.h"

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

namespace bd::movie {

namespace {

// Decoded frames held ahead of the one last asked for. Each is 1.4 MB at 720p.
constexpr size_t kAhead = 10;
// Display-order frames kept behind the one asked for: the game asks for each
// frame once, but an extra draw of the same one must still find it.
constexpr u64 kBehind = 2;
// Program stream read size.
constexpr size_t kReadSize = 512 * 1024;

constexpr u8 kVideoStream = 0xE0;

// Start codes of the video elementary stream.
constexpr u8 kPicture = 0x00;
constexpr u8 kSequence = 0xB3;
constexpr u8 kGop = 0xB8;

double Seconds(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double>(d).count();
}

} // namespace

Picture::~Picture() { av_frame_free(&frame_); }
u32 Picture::Width() const { return u32(frame_->width); }
u32 Picture::Height() const { return u32(frame_->height); }
const u8 *Picture::Plane(int plane) const { return frame_->data[plane]; }
int Picture::Stride(int plane) const { return frame_->linesize[plane]; }

SfdVideo::SfdVideo() = default;

SfdVideo::~SfdVideo() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  consumed_.notify_all();
  produced_.notify_all();
  if (thread_.joinable())
    thread_.join();
  if (file_)
    std::fclose(file_);
  av_frame_free(&frame_);
  av_packet_free(&packet_);
  avcodec_free_context(&codec_);
}

bool SfdVideo::Open(std::vector<std::string> paths, std::string &error) {
  if (paths.empty()) {
    error = "no file";
    return false;
  }
  for (const auto &path : paths) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) {
      error = "cannot open " + path;
      return false;
    }
    std::fclose(f);
  }
  paths_ = std::move(paths);

  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
  if (!codec) {
    error = "FFmpeg has no MPEG-1 video decoder";
    return false;
  }
  codec_ = avcodec_alloc_context3(codec);
  packet_ = av_packet_alloc();
  frame_ = av_frame_alloc();
  if (!codec_ || !packet_ || !frame_) {
    error = "out of memory";
    return false;
  }
  codec_->thread_count = 1;
  if (avcodec_open2(codec_, codec, nullptr) < 0) {
    error = "avcodec_open2 failed";
    return false;
  }

  // The picture size, from the first sequence header.
  file_ = std::fopen(paths_[0].c_str(), "rb");
  while (width_ == 0) {
    if (Fill() != FillResult::Data) {
      error = error_.empty() ? "no MPEG-1 sequence header in " + paths_[0]
                             : error_;
      return false;
    }
    for (size_t i = 0; i + 7 < es_.size(); ++i) {
      if (es_[i] == 0 && es_[i + 1] == 0 && es_[i + 2] == 1 &&
          es_[i + 3] == kSequence) {
        width_ = (u32(es_[i + 4]) << 4) | (es_[i + 5] >> 4);
        height_ = (u32(es_[i + 5] & 0x0F) << 8) | es_[i + 6];
        break;
      }
    }
    if (es_.size() > 4 * 1024 * 1024) {
      error = "no MPEG-1 sequence header in " + paths_[0];
      return false;
    }
  }
  stats_.width = width_;
  stats_.height = height_;
  return true;
}

void SfdVideo::Start(std::function<void()> setup) {
  thread_ = std::thread([this, setup = std::move(setup)] {
    if (setup)
      setup();
    Run();
  });
}

void SfdVideo::Append(std::string path) {
  std::lock_guard lock(mutex_);
  appended_.push_back(std::move(path));
}

std::string SfdVideo::LastError() const {
  std::lock_guard lock(mutex_);
  return error_;
}

StreamStats SfdVideo::Stats() const {
  std::lock_guard lock(mutex_);
  return stats_;
}

void SfdVideo::Fail(std::string error) {
  std::lock_guard lock(mutex_);
  if (error_.empty())
    error_ = std::move(error);
  failed_.store(true, std::memory_order_relaxed);
  produced_.notify_all();
}

std::shared_ptr<const Picture> SfdVideo::ByDecodeIndex(u64 index, int waitMs) {
  std::unique_lock lock(mutex_);
  wanted_ = std::max(wanted_, index);
  consumed_.notify_all();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
  for (;;) {
    for (size_t i = 0; i < ready_.size(); ++i) {
      if (ready_[i]->decodeIndex != index)
        continue;
      std::shared_ptr<const Picture> found = ready_[i];
      // Frames shown before this one are done with.
      const u64 shown = found->displayIndex;
      while (!ready_.empty() && ready_.front()->displayIndex + kBehind < shown)
        ready_.pop_front();
      consumed_.notify_all();
      return found;
    }
    // Asked for something newer than all that is held: the old frames will
    // not be asked for again, and they block the decoder.
    while (ready_.size() >= kAhead && ready_.front()->decodeIndex + 8 < index)
      ready_.pop_front();
    consumed_.notify_all();
    if (failed_.load(std::memory_order_relaxed) || stop_)
      return nullptr;
    // The decoder must not stop for a full queue now: what it holds can be
    // later pictures in stream order (B pictures) ahead of this one.
    ++waiting_;
    consumed_.notify_all();
    const bool timedOut =
        produced_.wait_until(lock, deadline) == std::cv_status::timeout;
    --waiting_;
    if (timedOut) {
      for (const auto &p : ready_)
        if (p->decodeIndex == index)
          return p;
      return nullptr;
    }
  }
}

// Reads program stream packets of the current file until some video has been
// added to es_. At the end of a file it reports that once (FileEnd) before
// moving on to the next file, or back to the first.
SfdVideo::FillResult SfdVideo::Fill() {
  const size_t before = es_.size();
  while (es_.size() == before) {
    if (psEnd_) {
      if (!fileEndReported_) {
        fileEndReported_ = true;
        return FillResult::FileEnd;
      }
      fileEndReported_ = false;
      // Next file.
      if (file_) {
        std::fclose(file_);
        file_ = nullptr;
      }
      {
        std::lock_guard lock(mutex_);
        for (auto &p : appended_)
          paths_.push_back(std::move(p));
        appended_.clear();
      }
      fileIndex_ = (fileIndex_ + 1) % paths_.size();
      if (fileIndex_ == 0)
        ++loops_;
      file_ = std::fopen(paths_[fileIndex_].c_str(), "rb");
      if (!file_) {
        Fail("cannot open " + paths_[fileIndex_]);
        return FillResult::Error;
      }
      ps_.clear();
      psPos_ = 0;
      psEnd_ = false;
      continue;
    }
    // Make sure a whole packet header and its 16-bit length are in ps_.
    if (ps_.size() - psPos_ < 64 * 1024 + 32) {
      ps_.erase(ps_.begin(), ps_.begin() + ptrdiff_t(psPos_));
      psPos_ = 0;
      const size_t old = ps_.size();
      ps_.resize(old + kReadSize);
      const size_t got = std::fread(ps_.data() + old, 1, kReadSize, file_);
      ps_.resize(old + got);
      if (got == 0 && ps_.empty()) {
        psEnd_ = true;
        continue;
      }
    }
    const u8 *p = ps_.data();
    const size_t n = ps_.size();
    size_t i = psPos_;
    // Find the next start code.
    while (i + 4 <= n && !(p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1))
      ++i;
    if (i + 4 > n) {
      // Nothing more in what was read.
      psPos_ = n >= 3 ? n - 3 : 0;
      if (std::feof(file_))
        psEnd_ = true;
      continue;
    }
    const u8 id = p[i + 3];
    if (id == 0xBA) { // pack header
      if (i + 5 > n)
        goto need_more;
      size_t len;
      if ((p[i + 4] & 0xC0) == 0x40) { // MPEG-2
        if (i + 14 > n)
          goto need_more;
        len = 14 + (p[i + 13] & 7);
      } else {
        len = 12; // MPEG-1
      }
      psPos_ = i + len;
      continue;
    }
    if (id == 0xB9) { // program end
      psPos_ = i + 4;
      continue;
    }
    if (id < 0xBB) { // not a packet: skip the code
      psPos_ = i + 4;
      continue;
    }
    if (i + 6 > n)
      goto need_more;
    {
      const size_t len = (size_t(p[i + 4]) << 8) | p[i + 5];
      if (i + 6 + len > n)
        goto need_more;
      if (id == kVideoStream) {
        size_t h = i + 6;
        const size_t end = i + 6 + len;
        if (h < end && (p[h] & 0xC0) == 0x80) {
          // MPEG-2 PES header.
          if (h + 3 > end)
            goto skip;
          h += 3 + p[h + 2];
        } else {
          // MPEG-1 PES header.
          while (h < end && p[h] == 0xFF)
            ++h;
          if (h < end && (p[h] & 0xC0) == 0x40)
            h += 2;
          if (h < end) {
            if ((p[h] & 0xF0) == 0x20)
              h += 5;
            else if ((p[h] & 0xF0) == 0x30)
              h += 10;
            else if (p[h] == 0x0F)
              h += 1;
          }
        }
        if (h < end)
          es_.insert(es_.end(), p + h, p + end);
      }
    skip:
      psPos_ = i + 6 + len;
      continue;
    }
  need_more:
    if (std::feof(file_)) {
      // A truncated packet at the end of the file.
      psEnd_ = true;
      continue;
    }
    // Force a read: keep what is unparsed and fetch more.
    {
      ps_.erase(ps_.begin(), ps_.begin() + ptrdiff_t(i));
      psPos_ = 0;
      const size_t old = ps_.size();
      ps_.resize(old + kReadSize);
      const size_t got = std::fread(ps_.data() + old, 1, kReadSize, file_);
      ps_.resize(old + got);
      if (got == 0)
        psEnd_ = true;
    }
  }
  return FillResult::Data;
}

// The next picture: its sequence/GOP headers, picture header and slices. A
// unit ends where the next picture's headers (or its picture header) begin,
// or at the end of its file, which sets 'fileEnd'.
bool SfdVideo::NextUnit(std::vector<u8> &out, int &type, bool &fileEnd) {
  fileEnd = false;
  int emptyFiles = 0;
  for (;;) {
    while (esScan_ + 4 <= es_.size()) {
      const u8 *p = es_.data() + esScan_;
      if (!(p[0] == 0 && p[1] == 0 && p[2] == 1)) {
        ++esScan_;
        continue;
      }
      const u8 code = p[3];
      const ptrdiff_t at = ptrdiff_t(esScan_);
      esScan_ += 4;
      if (code == kSequence || code == kGop) {
        if (headerStart_ < 0)
          headerStart_ = at;
        continue;
      }
      if (code != kPicture)
        continue; // slices, extensions, user data, the end of a sequence
      const ptrdiff_t start = headerStart_ >= 0 ? headerStart_ : at;
      headerStart_ = -1;
      if (esScan_ + 2 > es_.size()) {
        // The picture type is not in yet: look at this code again later.
        esScan_ = size_t(at);
        headerStart_ = start == at ? -1 : start;
        break;
      }
      const int nextType = (es_[esScan_ + 1] >> 3) & 7;
      if (unitStart_ < 0) {
        unitStart_ = start;
        unitType_ = nextType;
        continue;
      }
      // The unit before this picture is complete. An end of sequence code
      // (the end of a file) stays in it: FFmpeg then puts out the reference
      // picture it was holding.
      out.assign(es_.begin() + unitStart_, es_.begin() + start);
      type = unitType_;
      unitType_ = nextType;
      // Keep the bytes from this picture on.
      es_.erase(es_.begin(), es_.begin() + start);
      esScan_ -= size_t(start);
      unitStart_ = 0;
      return true;
    }
    switch (Fill()) {
    case FillResult::Data:
      break;
    case FillResult::Error:
      return false;
    case FillResult::FileEnd:
      if (unitStart_ < 0) {
        // A file without a picture.
        es_.clear();
        esScan_ = 0;
        headerStart_ = -1;
        if (++emptyFiles > int(paths_.size()) + 1)
          return false;
        break;
      }
      out.assign(es_.begin() + unitStart_, es_.end());
      type = unitType_;
      es_.clear();
      esScan_ = 0;
      unitStart_ = -1;
      headerStart_ = -1;
      fileEnd = true;
      return true;
    }
  }
}

bool SfdVideo::Decode(const std::vector<u8> *unit, int type) {
  (void)type;
  // Time spent in FFmpeg, not waiting for room.
  auto t0 = std::chrono::steady_clock::now();
  auto busy = [&] {
    std::lock_guard lock(mutex_);
    stats_.decodeSeconds += Seconds(std::chrono::steady_clock::now() - t0);
  };
  int ret;
  if (unit) {
    av_packet_unref(packet_);
    if (av_new_packet(packet_, int(unit->size())) < 0)
      return false;
    std::memcpy(packet_->data, unit->data(), unit->size());
    packet_->pts = packet_->dts = i64(nextDecodeIndex_);
    ++nextDecodeIndex_;
    ret = avcodec_send_packet(codec_, packet_);
  } else {
    ret = avcodec_send_packet(codec_, nullptr);
  }
  if (ret < 0 && ret != AVERROR(EAGAIN)) {
    std::lock_guard lock(mutex_);
    ++stats_.errors;
  }
  for (;;) {
    ret = avcodec_receive_frame(codec_, frame_);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      busy();
      return true;
    }
    if (ret < 0) {
      busy();
      std::lock_guard lock(mutex_);
      ++stats_.errors;
      return true;
    }
    AVFrame *kept = av_frame_alloc();
    if (!kept)
      return false;
    av_frame_move_ref(kept, frame_);
    auto picture = std::make_shared<Picture>(kept);
    picture->decodeIndex = kept->pts >= 0 ? u64(kept->pts) : 0;
    picture->displayIndex = nextDisplayIndex_++;
    picture->pictureType = int(kept->pict_type);
    busy();
    std::unique_lock lock(mutex_);
    ++stats_.decoded;
    // Wait for room, unless someone is waiting for a frame.
    consumed_.wait(lock, [&] {
      return stop_ || ready_.size() < kAhead || waiting_ > 0 ||
             picture->decodeIndex <= wanted_;
    });
    if (stop_)
      return false;
    ready_.push_back(std::move(picture));
    produced_.notify_all();
    lock.unlock();
    t0 = std::chrono::steady_clock::now();
  }
}

void SfdVideo::Run() {
  std::vector<u8> unit;
  int type = 0;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      if (stop_)
        return;
    }
    bool fileEnd = false;
    if (!NextUnit(unit, type, fileEnd)) {
      Fail(error_.empty() ? "no video in the files" : error_);
      return;
    }
    if (!Decode(&unit, type))
      return;
    if (fileEnd) {
      // Out with the reference picture FFmpeg holds back, which it would
      // otherwise drop at the next file's sequence header, and start the
      // next file afresh: the same as decoding each file on its own.
      if (!Decode(nullptr, 0))
        return;
      avcodec_flush_buffers(codec_);
    }
  }
}

} // namespace bd::movie
