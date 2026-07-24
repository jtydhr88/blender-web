/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * NVENC H.264 hardware encoder using CUDA device context.
 */

#include "GHOST_EncoderNVENC.hh"

#include <cuew.h>

#include <cstdio>
#include <cstring>

#define LOG(fmt, ...) fprintf(stderr, "[NVENC] " fmt "\n", ##__VA_ARGS__)

static CUcontext s_cuda_ctx = nullptr;

static bool ensure_cuda()
{
  if (s_cuda_ctx) {
    return true;
  }
  if (cuewInit(CUEW_INIT_CUDA) != CUEW_SUCCESS) {
    LOG("cuewInit failed");
    return false;
  }
  if (cuInit(0) != CUDA_SUCCESS) {
    LOG("cuInit failed");
    return false;
  }
  CUdevice device;
  if (cuDeviceGet(&device, 0) != CUDA_SUCCESS) {
    LOG("cuDeviceGet failed");
    return false;
  }
  if (cuCtxCreate(&s_cuda_ctx, 0, device) != CUDA_SUCCESS) {
    LOG("cuCtxCreate failed");
    return false;
  }
  char name[256] = {};
  cuDeviceGetName(name, sizeof(name), device);
  LOG("CUDA device: %s", name);
  return true;
}

GHOST_EncoderNVENC::GHOST_EncoderNVENC() {}

GHOST_EncoderNVENC::~GHOST_EncoderNVENC()
{
  shutdown();
}

bool GHOST_EncoderNVENC::init(int width, int height)
{
  if (!ensure_cuda()) {
    return false;
  }
  if (nvew_init() != 0) {
    LOG("nvew_init failed");
    return false;
  }
  funcs_ = nvew_get_functions();
  if (!funcs_) {
    LOG("No NVENC function table");
    return false;
  }

  /* H.264 requires even dimensions for YUV 4:2:0. */
  width_ = (width + 1) & ~1;
  height_ = (height + 1) & ~1;
  LOG("Requested %dx%d -> aligned %dx%d", width, height, width_, height_);

  /* Push CUDA context for this thread. */
  cuCtxPushCurrent(s_cuda_ctx);

  /* Open NVENC session with CUDA device. */
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = {};
  sessionParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  sessionParams.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  sessionParams.device = s_cuda_ctx;
  sessionParams.apiVersion = NVENCAPI_VERSION;

  NVENCSTATUS st = funcs_->nvEncOpenEncodeSessionEx(&sessionParams, &encoder_);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncOpenEncodeSessionEx failed: %d", st);
    cuCtxPopCurrent(nullptr);
    return false;
  }

  /* Configure H.264 low-latency encoding. */
  NV_ENC_PRESET_CONFIG presetConfig = {};
  presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
  presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
  funcs_->nvEncGetEncodePresetConfigEx(encoder_,
                                        NV_ENC_CODEC_H264_GUID,
                                        NV_ENC_PRESET_P1_GUID,
                                        NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY,
                                        &presetConfig);

  NV_ENC_CONFIG encConfig = presetConfig.presetCfg;

  /* Rate control: CBR for consistent quality. */
  encConfig.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  encConfig.rcParams.averageBitRate = 20000000;  /* 20 Mbps for sharp text/UI */
  encConfig.rcParams.maxBitRate = 30000000;
  encConfig.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
  encConfig.rcParams.zeroReorderDelay = 1;  /* No frame reordering — output immediately. */
  encConfig.rcParams.enableLookahead = 0;   /* No lookahead buffering. */

  /* H.264 config (following Sunshine/Moonlight's proven streaming approach). */
  encConfig.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
  encConfig.encodeCodecConfig.h264Config.idrPeriod = NVENC_INFINITE_GOPLENGTH;
  encConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
  encConfig.encodeCodecConfig.h264Config.disableSPSPPS = 0;
  encConfig.encodeCodecConfig.h264Config.sliceMode = 0;
  encConfig.encodeCodecConfig.h264Config.sliceModeData = 0;

  /* VUI: tell decoder "no reordering, output each frame immediately". */
  NV_ENC_CONFIG_H264_VUI_PARAMETERS &vui = encConfig.encodeCodecConfig.h264Config.h264VUIParameters;
  vui.bitstreamRestrictionFlag = 1;
  vui.videoSignalTypePresentFlag = 1;
  vui.videoFullRangeFlag = 1;

  /* GOP: P-frames every frame, IDR only on explicit request. */
  encConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
  encConfig.frameIntervalP = 1;

  NV_ENC_INITIALIZE_PARAMS initParams = {};
  initParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
  initParams.encodeGUID = NV_ENC_CODEC_H264_GUID;
  initParams.presetGUID = NV_ENC_PRESET_P1_GUID;
  initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  initParams.encodeWidth = width_;
  initParams.encodeHeight = height_;
  initParams.darWidth = width_;
  initParams.darHeight = height_;
  initParams.frameRateNum = 60;
  initParams.frameRateDen = 1;
  initParams.enablePTD = 1;
  initParams.encodeConfig = &encConfig;

  st = funcs_->nvEncInitializeEncoder(encoder_, &initParams);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncInitializeEncoder failed: %d", st);
    funcs_->nvEncDestroyEncoder(encoder_);
    encoder_ = nullptr;
    cuCtxPopCurrent(nullptr);
    return false;
  }

  /* Create input buffer (system memory, NVENC copies to GPU internally). */
  NV_ENC_CREATE_INPUT_BUFFER createIn = {};
  createIn.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
  createIn.width = width_;
  createIn.height = height_;
  createIn.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;  /* RGBA in little-endian = ABGR to NVENC */

  st = funcs_->nvEncCreateInputBuffer(encoder_, &createIn);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncCreateInputBuffer failed: %d", st);
    shutdown();
    cuCtxPopCurrent(nullptr);
    return false;
  }
  input_buffer_ = createIn.inputBuffer;

  /* Create output bitstream buffer. */
  NV_ENC_CREATE_BITSTREAM_BUFFER createOut = {};
  createOut.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;

  st = funcs_->nvEncCreateBitstreamBuffer(encoder_, &createOut);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncCreateBitstreamBuffer failed: %d", st);
    shutdown();
    cuCtxPopCurrent(nullptr);
    return false;
  }
  output_buffer_ = createOut.bitstreamBuffer;

  cuCtxPopCurrent(nullptr);
  LOG("Encoder initialized: %dx%d H.264 CBR 20Mbps (Sunshine-style low-latency)", width_, height_);
  return true;
}

bool GHOST_EncoderNVENC::encode(const uint8_t *rgba,
                                int width,
                                int height,
                                std::vector<uint8_t> &out,
                                bool &is_keyframe)
{
  int aligned_w = (width + 1) & ~1;
  int aligned_h = (height + 1) & ~1;
  if (!encoder_ || !funcs_ || aligned_w != width_ || aligned_h != height_) {
    return false;
  }

  cuCtxPushCurrent(s_cuda_ctx);

  /* Lock input buffer and copy RGBA data. */
  NV_ENC_LOCK_INPUT_BUFFER lockIn = {};
  lockIn.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
  lockIn.inputBuffer = input_buffer_;

  NVENCSTATUS st = funcs_->nvEncLockInputBuffer(encoder_, &lockIn);
  if (st != NV_ENC_SUCCESS) {
    cuCtxPopCurrent(nullptr);
    return false;
  }

  /* Copy RGBA rows. Input pitch may differ from width*4. */
  const int src_pitch = width * 4;
  uint8_t *dst = (uint8_t *)lockIn.bufferDataPtr;
  const int dst_pitch = lockIn.pitch;

  /* OpenGL pixels are bottom-up, NVENC expects top-down. Flip rows.
   * Also zero-pad if aligned dimensions differ from actual. */
  if (width_ != width || height_ != height) {
    memset(dst, 0, dst_pitch * height_);
  }
  for (int y = 0; y < height; y++) {
    const uint8_t *src_row = rgba + (height - 1 - y) * src_pitch;
    memcpy(dst + y * dst_pitch, src_row, src_pitch);
  }

  funcs_->nvEncUnlockInputBuffer(encoder_, input_buffer_);

  const bool ok = encodePicture(input_buffer_, dst_pitch, out, is_keyframe);
  cuCtxPopCurrent(nullptr);
  return ok;
}

bool GHOST_EncoderNVENC::encodePicture(void *input_buffer,
                                       int pitch,
                                       std::vector<uint8_t> &out,
                                       bool &is_keyframe)
{
  /* Caller holds the CUDA context. */
  NV_ENC_PIC_PARAMS picParams = {};
  picParams.version = NV_ENC_PIC_PARAMS_VER;
  picParams.inputBuffer = input_buffer;
  picParams.outputBitstream = output_buffer_;
  picParams.inputWidth = width_;
  picParams.inputHeight = height_;
  picParams.inputPitch = pitch;
  picParams.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
  picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

  if (force_keyframe_) {
    picParams.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
    force_keyframe_ = false;
  }

  NVENCSTATUS st = funcs_->nvEncEncodePicture(encoder_, &picParams);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncEncodePicture failed: %d", st);
    return false;
  }

  NV_ENC_LOCK_BITSTREAM lockOut = {};
  lockOut.version = NV_ENC_LOCK_BITSTREAM_VER;
  lockOut.outputBitstream = output_buffer_;

  st = funcs_->nvEncLockBitstream(encoder_, &lockOut);
  if (st != NV_ENC_SUCCESS) {
    return false;
  }

  const uint8_t *bs = (const uint8_t *)lockOut.bitstreamBufferPtr;
  out.assign(bs, bs + lockOut.bitstreamSizeInBytes);
  is_keyframe = (lockOut.pictureType == NV_ENC_PIC_TYPE_IDR);

  funcs_->nvEncUnlockBitstream(encoder_, output_buffer_);

  frame_count_++;
  /* Force keyframe every 2 seconds (assuming ~60fps). */
  if (frame_count_ % 120 == 0) {
    force_keyframe_ = true;
  }
  return true;
}

bool GHOST_EncoderNVENC::registerExternalBuffers(const GHOST_EncoderExternalBuffer *buffers,
                                                 int count,
                                                 int width,
                                                 int height,
                                                 int pitch)
{
#ifdef _WIN32
  if (!encoder_ || !funcs_ || !buffers || count <= 0) {
    return false;
  }
  if (!cuImportExternalMemory || !cuExternalMemoryGetMappedBuffer || !cuDestroyExternalMemory) {
    LOG("CUDA external memory API unavailable");
    return false;
  }
  unregisterExternalBuffers();

  cuCtxPushCurrent(s_cuda_ctx);
  external_slots_.resize(count);
  for (int index = 0; index < count; index++) {
    ExternalSlot &slot = external_slots_[index];

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC handle_desc = {};
    handle_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
    handle_desc.handle.win32.handle = buffers[index].win32_handle;
    handle_desc.size = buffers[index].size;
    CUexternalMemory ext_memory = nullptr;
    if (cuImportExternalMemory(&ext_memory, &handle_desc) != CUDA_SUCCESS) {
      LOG("cuImportExternalMemory failed (slot %d)", index);
      cuCtxPopCurrent(nullptr);
      unregisterExternalBuffers();
      return false;
    }
    slot.cuda_ext_memory = ext_memory;

    CUDA_EXTERNAL_MEMORY_BUFFER_DESC buffer_desc = {};
    buffer_desc.offset = 0;
    buffer_desc.size = buffers[index].size;
    CUdeviceptr dev_ptr = 0;
    if (cuExternalMemoryGetMappedBuffer(&dev_ptr, ext_memory, &buffer_desc) != CUDA_SUCCESS) {
      LOG("cuExternalMemoryGetMappedBuffer failed (slot %d)", index);
      cuCtxPopCurrent(nullptr);
      unregisterExternalBuffers();
      return false;
    }
    slot.cuda_dev_ptr = dev_ptr;

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = (void *)(uintptr_t)dev_ptr;
    reg.width = width;
    reg.height = height;
    reg.pitch = pitch;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    NVENCSTATUS st = funcs_->nvEncRegisterResource(encoder_, &reg);
    if (st != NV_ENC_SUCCESS) {
      LOG("nvEncRegisterResource failed: %d (slot %d)", st, index);
      cuCtxPopCurrent(nullptr);
      unregisterExternalBuffers();
      return false;
    }
    slot.registered = reg.registeredResource;
  }
  external_pitch_ = pitch;
  cuCtxPopCurrent(nullptr);
  LOG("Zero-copy input: %d external buffers registered (%dx%d pitch %d)",
      count,
      width,
      height,
      pitch);
  return true;
#else
  (void)buffers;
  (void)count;
  (void)width;
  (void)height;
  (void)pitch;
  return false;
#endif
}

bool GHOST_EncoderNVENC::encodeExternal(int slot_index,
                                        std::vector<uint8_t> &out,
                                        bool &is_keyframe)
{
  if (!encoder_ || !funcs_ || slot_index < 0 ||
      slot_index >= int(external_slots_.size()) || !external_slots_[slot_index].registered)
  {
    return false;
  }

  cuCtxPushCurrent(s_cuda_ctx);

  NV_ENC_MAP_INPUT_RESOURCE map = {};
  map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
  map.registeredResource = external_slots_[slot_index].registered;
  NVENCSTATUS st = funcs_->nvEncMapInputResource(encoder_, &map);
  if (st != NV_ENC_SUCCESS) {
    LOG("nvEncMapInputResource failed: %d", st);
    cuCtxPopCurrent(nullptr);
    return false;
  }

  const bool ok = encodePicture(map.mappedResource, external_pitch_, out, is_keyframe);

  funcs_->nvEncUnmapInputResource(encoder_, map.mappedResource);
  cuCtxPopCurrent(nullptr);
  return ok;
}

void GHOST_EncoderNVENC::unregisterExternalBuffers()
{
  if (external_slots_.empty()) {
    return;
  }
  cuCtxPushCurrent(s_cuda_ctx);
  for (ExternalSlot &slot : external_slots_) {
    if (slot.registered && funcs_ && encoder_) {
      funcs_->nvEncUnregisterResource(encoder_, (NV_ENC_REGISTERED_PTR)slot.registered);
    }
    if (slot.cuda_ext_memory && cuDestroyExternalMemory) {
      cuDestroyExternalMemory((CUexternalMemory)slot.cuda_ext_memory);
    }
  }
  external_slots_.clear();
  external_pitch_ = 0;
  cuCtxPopCurrent(nullptr);
}

void GHOST_EncoderNVENC::requestKeyframe()
{
  force_keyframe_ = true;
}

void GHOST_EncoderNVENC::shutdown()
{
  if (!encoder_ || !funcs_) {
    return;
  }

  unregisterExternalBuffers();

  cuCtxPushCurrent(s_cuda_ctx);

  if (input_buffer_) {
    funcs_->nvEncDestroyInputBuffer(encoder_, input_buffer_);
    input_buffer_ = nullptr;
  }
  if (output_buffer_) {
    funcs_->nvEncDestroyBitstreamBuffer(encoder_, output_buffer_);
    output_buffer_ = nullptr;
  }
  funcs_->nvEncDestroyEncoder(encoder_);
  encoder_ = nullptr;

  cuCtxPopCurrent(nullptr);
  LOG("Encoder shut down");
}
