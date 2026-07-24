/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "GHOST_Encoder.hh"
#include <nvew.h>

class GHOST_EncoderNVENC : public GHOST_Encoder {
 public:
  GHOST_EncoderNVENC();
  ~GHOST_EncoderNVENC() override;

  bool init(int width, int height) override;
  bool encode(const uint8_t *rgba,
              int width,
              int height,
              std::vector<uint8_t> &out,
              bool &is_keyframe) override;
  bool registerExternalBuffers(const GHOST_EncoderExternalBuffer *buffers,
                               int count,
                               int width,
                               int height,
                               int pitch) override;
  bool encodeExternal(int slot, std::vector<uint8_t> &out, bool &is_keyframe) override;
  void unregisterExternalBuffers() override;
  void requestKeyframe() override;
  void shutdown() override;
  const char *name() const override { return "NVENC H.264"; }

 private:
  /** Shared tail of both encode paths: encode picture, fetch bitstream, keyframe logic. */
  bool encodePicture(void *input_buffer,
                     int pitch,
                     std::vector<uint8_t> &out,
                     bool &is_keyframe);

  struct ExternalSlot {
    void *cuda_ext_memory = nullptr; /* CUexternalMemory */
    unsigned long long cuda_dev_ptr = 0; /* CUdeviceptr */
    void *registered = nullptr; /* NV_ENC_REGISTERED_PTR */
  };

  void *encoder_ = nullptr;  /* NV_ENC_REGISTERED_PTR style handle. */
  NV_ENCODE_API_FUNCTION_LIST *funcs_ = nullptr;
  void *input_buffer_ = nullptr;
  void *output_buffer_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  bool force_keyframe_ = true;  /* First frame is always IDR. */
  int frame_count_ = 0;
  std::vector<ExternalSlot> external_slots_;
  int external_pitch_ = 0;
};
