/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 *
 * This file contains the WEBRTC VP8 wrapper implementation
 *
 */
#include "vp8.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "module_common_types.h"
#include "reference_picture_selection.h"
#include "temporal_layers.h"
#include "tick_util.h"
#include "vpx/vpx_encoder.h"
#include "vpx/vpx_decoder.h"
#include "vpx/vp8cx.h"
#include "vpx/vp8dx.h"

enum { kVp8ErrorPropagationTh = 30 };

namespace webrtc
{

VP8Encoder* VP8Encoder::Create() {
  return new VP8Encoder();
}

VP8Encoder::VP8Encoder()
    : encoded_image_(),
      encoded_complete_callback_(NULL),
      inited_(false),
      timestamp_(0),
      picture_id_(0),
      feedback_mode_(false),
      cpu_speed_(-6), // default value
      rc_max_intra_target_(0),
      token_partitions_(VP8_ONE_TOKENPARTITION),
      rps_(new ReferencePictureSelection),
#if WEBRTC_LIBVPX_VERSION >= 971
      temporal_layers_(NULL),
#endif
      encoder_(NULL),
      config_(NULL),
      raw_(NULL) {
  memset(&codec_, 0, sizeof(codec_));
  uint32_t seed = static_cast<uint32_t>(TickTime::MillisecondTimestamp());
  srand(seed);
}

VP8Encoder::~VP8Encoder() {
  Release();
  delete rps_;
}

int VP8Encoder::Release() {
  if (encoded_image_._buffer != NULL) {
    delete [] encoded_image_._buffer;
    encoded_image_._buffer = NULL;
  }
  if (encoder_ != NULL) {
    if (vpx_codec_destroy(encoder_)) {
      return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    delete encoder_;
    encoder_ = NULL;
  }
  if (config_ != NULL) {
    delete config_;
    config_ = NULL;
  }
  if (raw_ != NULL) {
    vpx_img_free(raw_);
    raw_ = NULL;
  }
#if WEBRTC_LIBVPX_VERSION >= 971
  if (temporal_layers_ != NULL) {
    delete temporal_layers_;
    temporal_layers_ = NULL;
  }
#endif
  inited_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Encoder::SetRates(uint32_t new_bitrate_kbit, uint32_t new_framerate) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (encoder_->err) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  if (new_framerate < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // update bit rate
  if (codec_.maxBitrate > 0 && new_bitrate_kbit > codec_.maxBitrate) {
    new_bitrate_kbit = codec_.maxBitrate;
  }
  config_->rc_target_bitrate = new_bitrate_kbit; // in kbit/s

#if WEBRTC_LIBVPX_VERSION >= 971
  if (temporal_layers_) {
    temporal_layers_->ConfigureBitrates(new_bitrate_kbit, config_);
  }
#endif
  codec_.maxFramerate = new_framerate;

  // update encoder context
  if (vpx_codec_enc_config_set(encoder_, config_)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Encoder::InitEncode(const VideoCodec* inst,
                           int number_of_cores,
                           uint32_t /*max_payload_size*/) {
  if (inst == NULL) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->maxFramerate < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  // allow zero to represent an unspecified maxBitRate
  if (inst->maxBitrate > 0 && inst->startBitrate > inst->maxBitrate) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (inst->width < 1 || inst->height < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (number_of_cores < 1) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  feedback_mode_ = inst->codecSpecific.VP8.feedbackModeOn;

  int retVal = Release();
  if (retVal < 0) {
    return retVal;
  }
  if (encoder_ == NULL) {
    encoder_ = new vpx_codec_ctx_t;
  }
  if (config_ == NULL) {
    config_ = new vpx_codec_enc_cfg_t;
  }
  timestamp_ = 0;

  codec_ = *inst;

#if WEBRTC_LIBVPX_VERSION >= 971
  if (inst->codecSpecific.VP8.numberOfTemporalLayers > 1) {
    assert(temporal_layers_ == NULL);
    temporal_layers_ =
        new TemporalLayers(inst->codecSpecific.VP8.numberOfTemporalLayers);
  }
#endif
  // random start 16 bits is enough.
  picture_id_ = static_cast<uint16_t>(rand()) & 0x7FFF;

  // allocate memory for encoded image
  if (encoded_image_._buffer != NULL) {
    delete [] encoded_image_._buffer;
  }
  encoded_image_._size = CalcBufferSize(kI420, codec_.width, codec_.height);
  encoded_image_._buffer = new uint8_t[encoded_image_._size];
  encoded_image_._completeFrame = true;

  unsigned int align = 1;
  if (codec_.width % 32 == 0) {
    align = 32;
  }
  raw_ = vpx_img_alloc(NULL, IMG_FMT_I420, codec_.width, codec_.height, align);
  // populate encoder configuration with default values
  if (vpx_codec_enc_config_default(vpx_codec_vp8_cx(), config_, 0)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  config_->g_w = codec_.width;
  config_->g_h = codec_.height;
  config_->rc_target_bitrate = inst->startBitrate;  // in kbit/s

#if WEBRTC_LIBVPX_VERSION >= 971
  if (temporal_layers_) {
    temporal_layers_->ConfigureBitrates(inst->startBitrate, config_);
  }
#endif
  // setting the time base of the codec
  config_->g_timebase.num = 1;
  config_->g_timebase.den = 90000;

  // Set the error resilience mode according to user settings.
  switch (inst->codecSpecific.VP8.resilience) {
    case kResilienceOff:
      config_->g_error_resilient = 0;
#if WEBRTC_LIBVPX_VERSION >= 971
      if (temporal_layers_) {
        // Must be on for temporal layers.
        config_->g_error_resilient = 1;
      }
#endif
      break;
    case kResilientStream:
      config_->g_error_resilient = 1;  // TODO(holmer): Replace with
      // VPX_ERROR_RESILIENT_DEFAULT when we
      // drop support for libvpx 9.6.0.
      break;
    case kResilientFrames:
#ifdef INDEPENDENT_PARTITIONS
      config_->g_error_resilient = VPX_ERROR_RESILIENT_DEFAULT |
      VPX_ERROR_RESILIENT_PARTITIONS;
      break;
#else
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;  // Not supported
#endif
  }
  config_->g_lag_in_frames = 0; // 0- no frame lagging

  // Determining number of threads based on the image size
  if (codec_.width * codec_.height > 704 * 576 && number_of_cores > 1) {
    // 2 threads when larger than 4CIF
    config_->g_threads = 2;
  } else {
    config_->g_threads = 1;
  }

  // rate control settings
  config_->rc_dropframe_thresh = 30;
  config_->rc_end_usage = VPX_CBR;
  config_->g_pass = VPX_RC_ONE_PASS;
  config_->rc_resize_allowed = inst->codecSpecific.VP8.automaticResizeOn ?
      1 : 0;
  config_->rc_min_quantizer = 8;
  config_->rc_max_quantizer = 56;
  config_->rc_undershoot_pct = 100;
  config_->rc_overshoot_pct = 15;
  config_->rc_buf_initial_sz = 500;
  config_->rc_buf_optimal_sz = 600;
  config_->rc_buf_sz = 1000;
  // set the maximum target size of any key-frame.
  rc_max_intra_target_ = MaxIntraTarget(config_->rc_buf_optimal_sz);

  if (feedback_mode_) {
    // Disable periodic key frames if we get feedback from the decoder
    // through SLI and RPSI.
    config_->kf_mode = VPX_KF_DISABLED;
  } else {
    config_->kf_mode = VPX_KF_AUTO;
    config_->kf_max_dist = 3000;
  }
  switch (inst->codecSpecific.VP8.complexity) {
    case kComplexityHigh:
      cpu_speed_ = -5;
      break;
    case kComplexityHigher:
      cpu_speed_ = -4;
      break;
    case kComplexityMax:
      cpu_speed_ = -3;
      break;
    default:
      cpu_speed_ = -6;
      break;
  }
#ifdef WEBRTC_ANDROID
  // On mobile platform, always set to -12 to leverage between cpu usage
  // and video quality
  cpu_speed_ = -12;
#endif
  rps_->Init();
  return InitAndSetControlSettings(inst);
}

int VP8Encoder::InitAndSetControlSettings(const VideoCodec* inst) {
  vpx_codec_flags_t flags = 0;
  // TODO(holmer): We should make a smarter decision on the number of
  // partitions. Eight is probably not the optimal number for low resolution
  // video.

#if WEBRTC_LIBVPX_VERSION >= 971
  flags |= VPX_CODEC_USE_OUTPUT_PARTITION;
#endif
  if (vpx_codec_enc_init(encoder_, vpx_codec_vp8_cx(), config_, flags)) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  vpx_codec_control(encoder_, VP8E_SET_STATIC_THRESHOLD, 800);
  vpx_codec_control(encoder_, VP8E_SET_CPUUSED, cpu_speed_);
  vpx_codec_control(encoder_, VP8E_SET_TOKEN_PARTITIONS,
                    static_cast<vp8e_token_partitions>(token_partitions_));
  vpx_codec_control(encoder_, VP8E_SET_NOISE_SENSITIVITY,
                    inst->codecSpecific.VP8.denoisingOn ? 1 : 0);
#if WEBRTC_LIBVPX_VERSION >= 971
  vpx_codec_control(encoder_, VP8E_SET_MAX_INTRA_BITRATE_PCT,
                    rc_max_intra_target_);
#endif
  inited_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

uint32_t VP8Encoder::MaxIntraTarget(uint32_t optimalBuffersize) {
  // Set max to the optimal buffer level (normalized by target BR),
  // and scaled by a scalePar.
  // Max target size = scalePar * optimalBufferSize * targetBR[Kbps].
  // This values is presented in percentage of perFrameBw:
  // perFrameBw = targetBR[Kbps] * 1000 / frameRate.
  // The target in % is as follows:

  float scalePar = 0.5;
  uint32_t targetPct = optimalBuffersize * scalePar * codec_.maxFramerate / 10;

  // Don't go below 3 times the per frame bandwidth.
  const uint32_t minIntraTh = 300;
  return (targetPct < minIntraTh) ? minIntraTh: targetPct;
}

int VP8Encoder::Encode(const VideoFrame& input_image,
                       const CodecSpecificInfo* codec_specific_info,
                       const VideoFrameType frame_type) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (input_image.Buffer() == NULL) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  if (encoded_complete_callback_ == NULL) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  // Check for change in frame size.
  if (input_image.Width() != codec_.width ||
      input_image.Height() != codec_.height) {
    int ret = UpdateCodecFrameSize(input_image.Width(), input_image.Height());
    if (ret < 0) {
      return ret;
    }
  }
  // Image in vpx_image_t format.
  uint8_t* buffer = input_image.Buffer();
  uint32_t v_plane_loc = codec_.height * codec_.width +
    ((codec_.width + 1) >> 1) * ((codec_.height + 1) >> 1);
  raw_->planes[PLANE_Y] = buffer;
  raw_->planes[PLANE_U] = &buffer[codec_.width * codec_.height];
  raw_->planes[PLANE_V] = &buffer[v_plane_loc];

  int flags = 0;
#if WEBRTC_LIBVPX_VERSION >= 971
  if (temporal_layers_) {
    flags |= temporal_layers_->EncodeFlags();
  }
#endif
  bool send_keyframe = (frame_type == kKeyFrame);
  if (send_keyframe) {
    // Key frame request from caller.
    // Will update both golden and alt-ref.
    flags = VPX_EFLAG_FORCE_KF;
  } else if (feedback_mode_ && codec_specific_info) {
    // Handle RPSI and SLI messages and set up the appropriate encode flags.
    bool sendRefresh = false;
    if (codec_specific_info->codecType == kVideoCodecVP8) {
      if (codec_specific_info->codecSpecific.VP8.hasReceivedRPSI) {
        rps_->ReceivedRPSI(
            codec_specific_info->codecSpecific.VP8.pictureIdRPSI);
      }
      if (codec_specific_info->codecSpecific.VP8.hasReceivedSLI) {
        sendRefresh = rps_->ReceivedSLI(input_image.TimeStamp());
      }
    }
    flags = rps_->EncodeFlags(picture_id_, sendRefresh,
                              input_image.TimeStamp());
  }

  // TODO(holmer): Ideally the duration should be the timestamp diff of this
  // frame and the next frame to be encoded, which we don't have. Instead we
  // would like to use the duration of the previous frame. Unfortunately the
  // rate control seems to be off with that setup. Using the average input
  // frame rate to calculate an average duration for now.
  assert(codec_.maxFramerate > 0);
  uint32_t duration = 90000 / codec_.maxFramerate;
  if (vpx_codec_encode(encoder_, raw_, timestamp_, duration, flags,
                       VPX_DL_REALTIME)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  timestamp_ += duration;

#if WEBRTC_LIBVPX_VERSION >= 971
  return GetEncodedPartitions(input_image);
#else
  return GetEncodedFrame(input_image);
#endif
}

int VP8Encoder::UpdateCodecFrameSize(WebRtc_UWord32 input_image_width,
                                     WebRtc_UWord32 input_image_height) {
  codec_.width = input_image_width;
  codec_.height = input_image_height;

  raw_->w = codec_.width;
  raw_->h = codec_.height;
  raw_->d_w = codec_.width;
  raw_->d_h = codec_.height;
  raw_->stride[VPX_PLANE_Y] = codec_.width;
  raw_->stride[VPX_PLANE_U] = codec_.width / 2;
  raw_->stride[VPX_PLANE_V] = codec_.width / 2;
  vpx_img_set_rect(raw_, 0, 0, codec_.width, codec_.height);

  // Update encoder context for new frame size.
  // Change of frame size will automatically trigger a key frame.
  config_->g_w = codec_.width;
  config_->g_h = codec_.height;
  if (vpx_codec_enc_config_set(encoder_, config_)) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

void VP8Encoder::PopulateCodecSpecific(CodecSpecificInfo* codec_specific,
                                       const vpx_codec_cx_pkt& pkt) {
  assert(codec_specific != NULL);
  codec_specific->codecType = kVideoCodecVP8;
  CodecSpecificInfoVP8 *vp8Info = &(codec_specific->codecSpecific.VP8);
  vp8Info->pictureId = picture_id_;
  vp8Info->simulcastIdx = 0;
  vp8Info->keyIdx = kNoKeyIdx;  // TODO(hlundin) populate this
  vp8Info->nonReference = (pkt.data.frame.flags & VPX_FRAME_IS_DROPPABLE) != 0;
#if WEBRTC_LIBVPX_VERSION >= 971
  if (temporal_layers_) {
    temporal_layers_->PopulateCodecSpecific(
        (pkt.data.frame.flags & VPX_FRAME_IS_KEY) ? true : false, vp8Info);
  } else {
#endif
    vp8Info->temporalIdx = kNoTemporalIdx;
    vp8Info->layerSync = false;
    vp8Info->tl0PicIdx = kNoTl0PicIdx;
#if WEBRTC_LIBVPX_VERSION >= 971
  }
#endif
  picture_id_ = (picture_id_ + 1) & 0x7FFF;  // prepare next
}

int VP8Encoder::GetEncodedFrame(const VideoFrame& input_image) {
  vpx_codec_iter_t iter = NULL;
  encoded_image_._frameType = kDeltaFrame;
  const vpx_codec_cx_pkt_t *pkt= vpx_codec_get_cx_data(encoder_, &iter);
  if (pkt == NULL) {
    if (!encoder_->err) {
      // dropped frame
      return WEBRTC_VIDEO_CODEC_OK;
    } else {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  } else if (pkt->kind == VPX_CODEC_CX_FRAME_PKT) {
    CodecSpecificInfo codecSpecific;
    PopulateCodecSpecific(&codecSpecific, *pkt);

    assert(pkt->data.frame.sz <= encoded_image_._size);
    memcpy(encoded_image_._buffer, pkt->data.frame.buf, pkt->data.frame.sz);
    encoded_image_._length = uint32_t(pkt->data.frame.sz);
    encoded_image_._encodedHeight = raw_->h;
    encoded_image_._encodedWidth = raw_->w;

    // Check if encoded frame is a key frame.
    if (pkt->data.frame.flags & VPX_FRAME_IS_KEY) {
      encoded_image_._frameType = kKeyFrame;
      rps_->EncodedKeyFrame(picture_id_);
    }

    if (encoded_image_._length > 0) {
      encoded_image_._timeStamp = input_image.TimeStamp();
      // TODO(mikhal): Resolve confusion in terms.
      encoded_image_.capture_time_ms_ = input_image.RenderTimeMs();

      // Figure out where partition boundaries are located.
      RTPFragmentationHeader fragInfo;
      fragInfo.VerifyAndAllocateFragmentationHeader(2);
      // two partitions: 1st and 2nd

      // First partition
      fragInfo.fragmentationOffset[0] = 0;
      uint8_t *firstByte = encoded_image_._buffer;
      uint32_t tmpSize = (firstByte[2] << 16) | (firstByte[1] << 8)
                    | firstByte[0];
      fragInfo.fragmentationLength[0] = (tmpSize >> 5) & 0x7FFFF;
      fragInfo.fragmentationPlType[0] = 0; // not known here
      fragInfo.fragmentationTimeDiff[0] = 0;

      // Second partition
      fragInfo.fragmentationOffset[1] = fragInfo.fragmentationLength[0];
      fragInfo.fragmentationLength[1] = encoded_image_._length -
          fragInfo.fragmentationLength[0];
      fragInfo.fragmentationPlType[1] = 0; // not known here
      fragInfo.fragmentationTimeDiff[1] = 0;

      encoded_complete_callback_->Encoded(encoded_image_, &codecSpecific,
                                        &fragInfo);
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }
  return WEBRTC_VIDEO_CODEC_ERROR;
}

#if WEBRTC_LIBVPX_VERSION >= 971
int VP8Encoder::GetEncodedPartitions(const VideoFrame& input_image) {
  vpx_codec_iter_t iter = NULL;
  int part_idx = 0;
  encoded_image_._length = 0;
  encoded_image_._frameType = kDeltaFrame;
  RTPFragmentationHeader frag_info;
  frag_info.VerifyAndAllocateFragmentationHeader((1 << token_partitions_) + 1);
  CodecSpecificInfo codec_specific;

  const vpx_codec_cx_pkt_t *pkt = NULL;
  while ((pkt = vpx_codec_get_cx_data(encoder_, &iter)) != NULL) {
    switch(pkt->kind) {
      case VPX_CODEC_CX_FRAME_PKT: {
        memcpy(&encoded_image_._buffer[encoded_image_._length],
               pkt->data.frame.buf,
               pkt->data.frame.sz);
        frag_info.fragmentationOffset[part_idx] = encoded_image_._length;
        frag_info.fragmentationLength[part_idx] =  pkt->data.frame.sz;
        frag_info.fragmentationPlType[part_idx] = 0;  // not known here
        frag_info.fragmentationTimeDiff[part_idx] = 0;
        encoded_image_._length += pkt->data.frame.sz;
        assert(encoded_image_._length <= encoded_image_._size);
        ++part_idx;
        break;
      }
      default: {
        break;
      }
    }
    // End of frame
    if ((pkt->data.frame.flags & VPX_FRAME_IS_FRAGMENT) == 0) {
      // check if encoded frame is a key frame
      if (pkt->data.frame.flags & VPX_FRAME_IS_KEY) {
          encoded_image_._frameType = kKeyFrame;
          rps_->EncodedKeyFrame(picture_id_);
      }
      PopulateCodecSpecific(&codec_specific, *pkt);
      break;
    }
  }
  if (encoded_image_._length > 0) {
    encoded_image_._timeStamp = input_image.TimeStamp();
    encoded_image_.capture_time_ms_ = input_image.RenderTimeMs();
    encoded_image_._encodedHeight = raw_->h;
    encoded_image_._encodedWidth = raw_->w;
    encoded_complete_callback_->Encoded(encoded_image_, &codec_specific,
                                      &frag_info);
  }
  return WEBRTC_VIDEO_CODEC_OK;
}
#endif

int VP8Encoder::SetChannelParameters(uint32_t /*packet_loss*/, int rtt) {
  rps_->SetRtt(rtt);
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Encoder::RegisterEncodeCompleteCallback(
    EncodedImageCallback* callback) {
  encoded_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

VP8Decoder* VP8Decoder::Create() {
  return new VP8Decoder();
}

VP8Decoder::VP8Decoder()
    : decode_complete_callback_(NULL),
      inited_(false),
      feedback_mode_(false),
      decoder_(NULL),
      last_keyframe_(),
      image_format_(VPX_IMG_FMT_NONE),
      ref_frame_(NULL),
      propagation_cnt_(-1),
      latest_keyframe_complete_(false),
      mfqe_enabled_(false) {
  memset(&codec_, 0, sizeof(codec_));
}

VP8Decoder::~VP8Decoder() {
  inited_ = true; // in order to do the actual release
  Release();
}

int VP8Decoder::Reset() {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  InitDecode(&codec_, 1);
  propagation_cnt_ = -1;
  latest_keyframe_complete_ = false;
  mfqe_enabled_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::InitDecode(const VideoCodec* inst, int number_of_cores) {
  if (inst == NULL) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
  int ret_val = Release();
  if (ret_val < 0 ) {
    return ret_val;
  }
  if (decoder_ == NULL) {
    decoder_ = new vpx_dec_ctx_t;
  }
  if (inst->codecType == kVideoCodecVP8) {
    feedback_mode_ = inst->codecSpecific.VP8.feedbackModeOn;
  }
  vpx_codec_dec_cfg_t  cfg;
  // Setting number of threads to a constant value (1)
  cfg.threads = 1;
  cfg.h = cfg.w = 0; // set after decode

  vpx_codec_flags_t flags = 0;
#if (WEBRTC_LIBVPX_VERSION >= 971) && !defined(WEBRTC_ANDROID)
  flags = VPX_CODEC_USE_POSTPROC;
  if (inst->codecSpecific.VP8.errorConcealmentOn) {
    flags |= VPX_CODEC_USE_ERROR_CONCEALMENT;
  }
#ifdef INDEPENDENT_PARTITIONS
  flags |= VPX_CODEC_USE_INPUT_PARTITION;
#endif
#endif

  if (vpx_codec_dec_init(decoder_, vpx_codec_vp8_dx(), &cfg, flags)) {
    return WEBRTC_VIDEO_CODEC_MEMORY;
  }

#if (WEBRTC_LIBVPX_VERSION >= 971) && !defined(WEBRTC_ANDROID)
  vp8_postproc_cfg_t  ppcfg;
  ppcfg.post_proc_flag = VP8_DEMACROBLOCK | VP8_DEBLOCK;
  // Strength of deblocking filter. Valid range:[0,16]
  ppcfg.deblocking_level = 3;
  vpx_codec_control(decoder_, VP8_SET_POSTPROC, &ppcfg);
#endif

  // Save VideoCodec instance for later; mainly for duplicating the decoder.
  codec_ = *inst;
  propagation_cnt_ = -1;
  latest_keyframe_complete_ = false;

  inited_ = true;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::Decode(const EncodedImage& input_image,
                       bool missing_frames,
                       const RTPFragmentationHeader* fragmentation,
                       const CodecSpecificInfo* codec_specific_info,
                       int64_t /*render_time_ms*/) {
  if (!inited_) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (decode_complete_callback_ == NULL) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }
  if (input_image._buffer == NULL && input_image._length > 0) {
    // Reset to avoid requesting key frames too often.
    if (propagation_cnt_ > 0)
      propagation_cnt_ = 0;
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }

#ifdef INDEPENDENT_PARTITIONS
  if (fragmentation == NULL) {
    return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
  }
#endif

#if (WEBRTC_LIBVPX_VERSION >= 971) && !defined(WEBRTC_ANDROID)
  if (!mfqe_enabled_ && codec_specific_info &&
      codec_specific_info->codecSpecific.VP8.temporalIdx > 0) {
    // Enable MFQE if we are receiving layers.
    // temporalIdx is set in the jitter buffer according to what the RTP
    // header says.
    mfqe_enabled_ = true;
    vp8_postproc_cfg_t  ppcfg;
    ppcfg.post_proc_flag = VP8_MFQE | VP8_DEMACROBLOCK | VP8_DEBLOCK;
    ppcfg.deblocking_level = 3;
    vpx_codec_control(decoder_, VP8_SET_POSTPROC, &ppcfg);
  }
#endif

  // Restrict error propagation using key frame requests. Disabled when
  // the feedback mode is enabled (RPS).
  // Reset on a key frame refresh.
  if (!feedback_mode_) {
    if (input_image._frameType == kKeyFrame && input_image._completeFrame)
      propagation_cnt_ = -1;
    // Start count on first loss.
    else if ((!input_image._completeFrame || missing_frames) &&
        propagation_cnt_ == -1)
      propagation_cnt_ = 0;
    if (propagation_cnt_ >= 0)
      propagation_cnt_++;
  }

  vpx_codec_iter_t iter = NULL;
  vpx_image_t* img;
  int ret;

  // Check for missing frames.
  if (missing_frames) {
    // Call decoder with zero data length to signal missing frames.
    if (vpx_codec_decode(decoder_, NULL, 0, 0, VPX_DL_REALTIME)) {
      // Reset to avoid requesting key frames too often.
      if (propagation_cnt_ > 0)
        propagation_cnt_ = 0;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    // We don't render this frame.
    vpx_codec_get_frame(decoder_, &iter);
    iter = NULL;
  }

#ifdef INDEPENDENT_PARTITIONS
  if (DecodePartitions(inputImage, fragmentation)) {
    // Reset to avoid requesting key frames too often.
    if (propagation_cnt_ > 0) {
      propagation_cnt_ = 0;
    }
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
#else
  uint8_t* buffer = input_image._buffer;
  if (input_image._length == 0) {
    buffer = NULL; // Triggers full frame concealment.
  }
  if (vpx_codec_decode(decoder_,
                       buffer,
                       input_image._length,
                       0,
                       VPX_DL_REALTIME)) {
    // Reset to avoid requesting key frames too often.
    if (propagation_cnt_ > 0)
      propagation_cnt_ = 0;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
#endif

  // Store encoded frame if key frame. (Used in Copy method.)
  if (input_image._frameType == kKeyFrame && input_image._buffer != NULL) {
    const uint32_t bytes_to_copy = input_image._length;
    if (last_keyframe_._size < bytes_to_copy) {
      delete [] last_keyframe_._buffer;
      last_keyframe_._buffer = NULL;
      last_keyframe_._size = 0;
    }

    uint8_t* temp_buffer = last_keyframe_._buffer; // Save buffer ptr.
    uint32_t temp_size = last_keyframe_._size; // Save size.
    last_keyframe_ = input_image; // Shallow copy.
    last_keyframe_._buffer = temp_buffer; // Restore buffer ptr.
    last_keyframe_._size = temp_size; // Restore buffer size.
    if (!last_keyframe_._buffer) {
      // Allocate memory.
      last_keyframe_._size = bytes_to_copy;
      last_keyframe_._buffer = new uint8_t[last_keyframe_._size];
    }
    // Copy encoded frame.
    memcpy(last_keyframe_._buffer, input_image._buffer, bytes_to_copy);
    last_keyframe_._length = bytes_to_copy;
  }

  img = vpx_codec_get_frame(decoder_, &iter);
  ret = ReturnFrame(img, input_image._timeStamp);
  if (ret != 0) {
    // Reset to avoid requesting key frames too often.
    if (ret < 0 && propagation_cnt_ > 0)
      propagation_cnt_ = 0;
    return ret;
  }
  if (feedback_mode_) {
    // Whenever we receive an incomplete key frame all reference buffers will
    // be corrupt. If that happens we must request new key frames until we
    // decode a complete.
    if (input_image._frameType == kKeyFrame)
      latest_keyframe_complete_ = input_image._completeFrame;
    if (!latest_keyframe_complete_)
      return WEBRTC_VIDEO_CODEC_ERROR;

    // Check for reference updates and last reference buffer corruption and
    // signal successful reference propagation or frame corruption to the
    // encoder.
    int reference_updates = 0;
    if (vpx_codec_control(decoder_, VP8D_GET_LAST_REF_UPDATES,
                          &reference_updates)) {
      // Reset to avoid requesting key frames too often.
      if (propagation_cnt_ > 0)
        propagation_cnt_ = 0;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    int corrupted = 0;
    if (vpx_codec_control(decoder_, VP8D_GET_FRAME_CORRUPTED, &corrupted)) {
      // Reset to avoid requesting key frames too often.
      if (propagation_cnt_ > 0)
        propagation_cnt_ = 0;
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    int16_t picture_id = -1;
    if (codec_specific_info) {
      picture_id = codec_specific_info->codecSpecific.VP8.pictureId;
    }
    if (picture_id > -1) {
      if (((reference_updates & VP8_GOLD_FRAME) ||
          (reference_updates & VP8_ALTR_FRAME)) && !corrupted) {
        decode_complete_callback_->ReceivedDecodedReferenceFrame(picture_id);
      }
      decode_complete_callback_->ReceivedDecodedFrame(picture_id);
    }
    if (corrupted) {
      // we can decode but with artifacts
      return WEBRTC_VIDEO_CODEC_REQUEST_SLI;
    }
  }
  // Check Vs. threshold
  if (propagation_cnt_ > kVp8ErrorPropagationTh) {
    // Reset to avoid requesting key frames too often.
    propagation_cnt_ = 0;
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::DecodePartitions(
    const EncodedImage& input_image,
    const RTPFragmentationHeader* fragmentation) {
  for (int i = 0; i < fragmentation->fragmentationVectorSize; ++i) {
    const uint8_t* partition = input_image._buffer +
        fragmentation->fragmentationOffset[i];
    const uint32_t partition_length =
        fragmentation->fragmentationLength[i];
    if (vpx_codec_decode(decoder_,
                         partition,
                         partition_length,
                         0,
                         VPX_DL_REALTIME)) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
  }
  // Signal end of frame data. If there was no frame data this will trigger
  // a full frame concealment.
  if (vpx_codec_decode(decoder_, NULL, 0, 0, VPX_DL_REALTIME))
    return WEBRTC_VIDEO_CODEC_ERROR;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::ReturnFrame(const vpx_image_t* img, uint32_t timestamp) {
  if (img == NULL) {
    // Decoder OK and NULL image => No show frame
    return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
  }

  uint32_t required_size = CalcBufferSize(kI420, img->d_w, img->d_h);
  decoded_image_.VerifyAndAllocate(required_size);

  uint8_t* buf;
  uint32_t pos = 0;
  uint32_t plane, y;
  uint8_t* buffer = decoded_image_.Buffer();
  for (plane = 0; plane < 3; plane++) {
    unsigned int width = (plane ? (img->d_w + 1) >> 1 : img->d_w);
    unsigned int height = (plane ? (img->d_h + 1) >> 1 : img->d_h);
    buf = img->planes[plane];
    for(y = 0; y < height; y++) {
      memcpy(&buffer[pos], buf, width);
      pos += width;
      buf += img->stride[plane];
    }
  }

  // Set decoded image parameters.
  decoded_image_.SetHeight(img->d_h);
  decoded_image_.SetWidth(img->d_w);
  decoded_image_.SetLength(CalcBufferSize(kI420, img->d_w, img->d_h));
  decoded_image_.SetTimeStamp(timestamp);
  int ret = decode_complete_callback_->Decoded(decoded_image_);
  if (ret != 0)
    return ret;

  // Remember image format for later
  image_format_ = img->fmt;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::RegisterDecodeCompleteCallback(
    DecodedImageCallback* callback) {
  decode_complete_callback_ = callback;
  return WEBRTC_VIDEO_CODEC_OK;
}

int VP8Decoder::Release() {
  decoded_image_.Free();
  if (last_keyframe_._buffer != NULL) {
    delete [] last_keyframe_._buffer;
    last_keyframe_._buffer = NULL;
  }
  if (decoder_ != NULL) {
    if(vpx_codec_destroy(decoder_)) {
      return WEBRTC_VIDEO_CODEC_MEMORY;
    }
    delete decoder_;
    decoder_ = NULL;
  }
  if (ref_frame_ != NULL) {
    vpx_img_free(&ref_frame_->img);
    delete ref_frame_;
    ref_frame_ = NULL;
  }
  inited_ = false;
  return WEBRTC_VIDEO_CODEC_OK;
}

VideoDecoder* VP8Decoder::Copy() {
  // Sanity checks.
  if (!inited_) {
    // Not initialized.
    assert(false);
    return NULL;
  }
  if (decoded_image_.Buffer() == NULL) {
    // Nothing has been decoded before; cannot clone.
    return NULL;
  }
  if (last_keyframe_._buffer == NULL) {
    // Cannot clone if we have no key frame to start with.
    return NULL;
  }
  // Create a new VideoDecoder object
  VP8Decoder *copy = new VP8Decoder;

  // Initialize the new decoder
  if (copy->InitDecode(&codec_, 1) != WEBRTC_VIDEO_CODEC_OK) {
    delete copy;
    return NULL;
  }
  // Inject last key frame into new decoder.
  if (vpx_codec_decode(copy->decoder_, last_keyframe_._buffer,
                       last_keyframe_._length, NULL, VPX_DL_REALTIME)) {
    delete copy;
    return NULL;
  }
  // Allocate memory for reference image copy
  assert(decoded_image_.Width() > 0);
  assert(decoded_image_.Height() > 0);
  assert(image_format_ > VPX_IMG_FMT_NONE);
  // Check if frame format has changed.
  if (ref_frame_ &&
      (decoded_image_.Width() != ref_frame_->img.d_w ||
          decoded_image_.Height() != ref_frame_->img.d_h ||
          image_format_ != ref_frame_->img.fmt)) {
    vpx_img_free(&ref_frame_->img);
    delete ref_frame_;
    ref_frame_ = NULL;
  }


  if (!ref_frame_) {
    ref_frame_ = new vpx_ref_frame_t;

    unsigned int align = 1;
    if (decoded_image_.Width() % 32 == 0) {
      align = 32;
    }
    if (!vpx_img_alloc(&ref_frame_->img,
                       static_cast<vpx_img_fmt_t>(image_format_),
                       decoded_image_.Width(), decoded_image_.Height(),
                       align)) {
      assert(false);
      delete copy;
      return NULL;
    }
  }
  const vpx_ref_frame_type_t type_vec[] = { VP8_LAST_FRAME, VP8_GOLD_FRAME,
      VP8_ALTR_FRAME };
  for (uint32_t ix = 0;
      ix < sizeof(type_vec) / sizeof(vpx_ref_frame_type_t); ++ix) {
    ref_frame_->frame_type = type_vec[ix];
    if (CopyReference(copy) < 0) {
      delete copy;
      return NULL;
    }
  }
  // Copy all member variables (that are not set in initialization).
  copy->feedback_mode_ = feedback_mode_;
  copy->image_format_ = image_format_;
  copy->last_keyframe_ = last_keyframe_; // Shallow copy.
  // Allocate memory. (Discard copied _buffer pointer.)
  copy->last_keyframe_._buffer = new uint8_t[last_keyframe_._size];
  memcpy(copy->last_keyframe_._buffer, last_keyframe_._buffer,
         last_keyframe_._length);

  return static_cast<VideoDecoder*>(copy);
}

int VP8Decoder::CopyReference(VP8Decoder* copyTo) {
  // The type of frame to copy should be set in ref_frame_->frame_type
  // before the call to this function.
  if (vpx_codec_control(decoder_, VP8_COPY_REFERENCE, ref_frame_)
      != VPX_CODEC_OK) {
    return -1;
  }
  if (vpx_codec_control(copyTo->decoder_, VP8_SET_REFERENCE, ref_frame_)
      != VPX_CODEC_OK) {
    return -1;
  }
  return 0;
}

} // namespace webrtc