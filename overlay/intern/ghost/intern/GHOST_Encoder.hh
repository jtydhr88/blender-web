/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Abstract video encoder interface for the Web display backend.
 */

#pragma once

#include <cstdint>
#include <vector>

/** Exportable GPU buffer for the zero-copy encoder input path. */
struct GHOST_EncoderExternalBuffer {
  /** Win32 HANDLE of the exported device memory. */
  void *win32_handle;
  /** Size of the memory allocation in bytes. */
  uint64_t size;
};

class GHOST_Encoder {
 public:
  virtual ~GHOST_Encoder() = default;

  /** Initialize the encoder for the given resolution. Returns true on success. */
  virtual bool init(int width, int height) = 0;

  /** Encode an RGBA frame. Output H.264 bitstream appended to `out`.
   * `is_keyframe` set to true if this frame is an IDR. */
  virtual bool encode(const uint8_t *rgba,
                      int width,
                      int height,
                      std::vector<uint8_t> &out,
                      bool &is_keyframe) = 0;

  /**
   * Zero-copy input path: import externally allocated GPU buffers (one per swap-chain
   * slot) so frames can be encoded without a host round-trip. Buffers hold RGBA rows,
   * top-down, `pitch` bytes apart. Returns false when unsupported.
   */
  virtual bool registerExternalBuffers(const GHOST_EncoderExternalBuffer * /*buffers*/,
                                       int /*count*/,
                                       int /*width*/,
                                       int /*height*/,
                                       int /*pitch*/)
  {
    return false;
  }

  /** Encode directly from a registered external buffer slot. */
  virtual bool encodeExternal(int /*slot*/, std::vector<uint8_t> & /*out*/, bool & /*is_keyframe*/)
  {
    return false;
  }

  /** Release imported external buffers (must precede freeing the underlying memory). */
  virtual void unregisterExternalBuffers() {}

  /** Force next frame to be a keyframe. */
  virtual void requestKeyframe() = 0;

  /** Shut down and release resources. */
  virtual void shutdown() = 0;

  /** Human-readable encoder name. */
  virtual const char *name() const = 0;
};
