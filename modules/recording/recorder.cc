#include "modules/recording/recorder.h"

#include <chrono>

#include "rtc_base/logging.h"

#include <arpa/inet.h>

#if !(defined(BYTE_ORDER) && defined(LITTLE_ENDIAN) && defined(BIG_ENDIAN))
#error "unknown endianess!"
#endif

#if BYTE_ORDER == LITTLE_ENDIAN

static inline uint8_t utils_tole_8(uint8_t val) {
  return val;
}

static inline uint16_t utils_tole_16(uint16_t val) {
  return val;
}

static inline uint32_t utils_tole_32(uint32_t val) {
  return val;
}

static inline uint64_t utils_tole_64(uint64_t val) {
  return val;
}

#elif BYTE_ORDER == BIG_ENDIAN

#ifndef __builtin_bswap16
static inline uint16_t __builtin_bswap16(uint16_t x) {
  return (x << 8U) | (x >> 8U);
}
#endif

static inline uint8_t utils_tole_8(uint8_t val) {
  return val;
}

static inline uint16_t utils_tole_16(uint16_t val) {
  return __builtin_bswap16(val);
}

static inline uint32_t utils_tole_32(uint32_t val) {
  return __builtin_bswap32(val);
}

static inline uint64_t utils_tole_64(uint64_t val) {
  return __builtin_bswap64(val);
}

#else
#error "unknown endianess!"
#endif

namespace webrtc {

static inline int64_t currentTimeMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

Recorder::Frame::Frame(const uint8_t* payload, uint32_t length) {
  this->payload = new uint8_t[length];
  memcpy(this->payload, payload, length);
  this->length = length;
  this->timestamp = currentTimeMs();
  this->duration = 0;
  this->is_video = false;
  this->is_key_frame = false;
}

Recorder::Frame::~Frame() {
  delete[] payload;
}

Recorder::Recorder(TaskQueueFactory* task_queue_factory)
    : got_audio_(false),
      sample_rate_(0),
      channel_num_(0),
      got_video_(false),
      width_(0),
      height_(0),
      record_queue_(task_queue_factory->CreateTaskQueue(
          "recorder",
          TaskQueueFactory::Priority::NORMAL)),
      timestamp_offset_(0),
      added_audio_frames_(0),
      added_video_frames_(0),
      drained_frames_(0) {}

Recorder::~Recorder() {
  Stop();
}

int32_t Recorder::Start(const std::string& path) {
  fp_ = std::fopen(path.c_str(), "w+");
  if (!fp_) {
    RTC_LOG(LS_ERROR) << "Recorder::Start error, fopen fail.";
    return -11;
  }

  RTC_LOG(LS_INFO) << "Recorder::Start success";
  return 0;
}

void Recorder::AddVideoFrame(const EncodedImage* frame,
                             VideoCodecType video_codec) {
  if (++added_video_frames_ % 125 == 1) {
    RTC_LOG(LS_INFO) << "Recorder::AddVideoFrame " << added_video_frames_
                     << " times";
  }
  if (!got_video_ && frame->_frameType == VideoFrameType::kVideoFrameKey) {
    got_video_ = true;
    video_codec_ = video_codec;
    width_ = frame->_encodedWidth;
    height_ = frame->_encodedHeight;
  }

  std::shared_ptr<Frame> media_frame(new Frame(frame->data(), frame->size()));
  media_frame->is_video = true;
  media_frame->is_key_frame =
      frame->_frameType == VideoFrameType::kVideoFrameKey;

  if (!last_video_frame_) {
    last_video_frame_ = media_frame;
    return;
  }

  last_video_frame_->duration =
      media_frame->timestamp - last_video_frame_->timestamp;
  if (last_video_frame_->duration <= 0) {
    last_video_frame_->duration = 1;
    media_frame->timestamp = last_video_frame_->timestamp + 1;
  }

  if (last_video_frame_->is_key_frame && !video_key_frame_) {
    video_key_frame_ = last_video_frame_;
  }

  frames_.push(last_video_frame_);
  last_video_frame_ = media_frame;

  record_queue_.PostTask([this]() { drainFrames(); });
}

void Recorder::AddAudioFrame(int32_t sample_rate,
                             int32_t channel_num,
                             const uint8_t* frame,
                             uint32_t size,
                             AudioEncoder::CodecType audio_codec) {
  if (++added_audio_frames_ % 500 == 1) {
    RTC_LOG(LS_INFO) << "Recorder::AddAudioFrame " << added_audio_frames_
                     << " times";
  }
  if (!frame || !size) {
    return;
  }

  if (!got_audio_) {
    got_audio_ = true;
    audio_codec_ = audio_codec;
    sample_rate_ = sample_rate;
    channel_num_ = channel_num;
  }

  std::shared_ptr<Frame> media_frame(new Frame(frame, size));

  if (!last_audio_frame_) {
    last_audio_frame_ = media_frame;
    return;
  }

  last_audio_frame_->duration =
      media_frame->timestamp - last_audio_frame_->timestamp;
  if (last_audio_frame_->duration <= 0) {
    last_audio_frame_->duration = 1;
    media_frame->timestamp = last_audio_frame_->timestamp + 1;
  }

  frames_.push(last_audio_frame_);
  last_audio_frame_ = media_frame;

  record_queue_.PostTask([this]() { drainFrames(); });
}

void Recorder::Stop() {
  if (fp_) {
    fclose(fp_);
    fp_ = nullptr;
  }
}

void Recorder::drainFrames() {
  if (!fp_) {
    return;
  }

  while (!frames_.empty()) {
    if (++drained_frames_ % 1000 == 1) {
      RTC_LOG(LS_INFO) << "Recorder::drainFrames " << drained_frames_
                       << " times";
    }
    std::shared_ptr<Frame> frame = frames_.front();
    frames_.pop();

    uint32_t pts = (frame->timestamp - timestamp_offset_) % 100000000;

    // Write Length
    uint32_t nvtLength = utils_tole_32(frame->length + 5);
    std::fwrite(&nvtLength, sizeof(uint32_t), 1, fp_);

    uint32_t nvtPts = utils_tole_32(pts);

    if (frame->is_video) {
      // Write Flag
      uint8_t flag = 0;
      if (frame->is_key_frame) {
        flag = 0x13;
      } else {
        flag = 0x12;
      }
      std::fwrite(&flag, sizeof(uint8_t), 1, fp_);

      // Write PTS
      std::fwrite(&nvtPts, sizeof(uint32_t), 1, fp_);

      // Write Media Data
      std::fwrite(frame->payload, frame->length, 1, fp_);

    } else {
      // Write Flag
      uint8_t flag = 0x22;
      std::fwrite(&flag, sizeof(uint8_t), 1, fp_);

      // Write PTS
      std::fwrite(&nvtPts, sizeof(uint32_t), 1, fp_);

      // Write Media Data
      std::fwrite(frame->payload, frame->length, 1, fp_);
    }
  }
}
}  // namespace webrtc
