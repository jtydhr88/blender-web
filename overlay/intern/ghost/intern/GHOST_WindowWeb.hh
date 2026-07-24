/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_WindowWeb class.
 *
 * A window that renders to an offscreen FBO and streams
 * frames to the browser through GHOST_SystemWeb's WebSocket server.
 */

#pragma once

#include "GHOST_Window.hh"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

class GHOST_SystemWeb;
struct GHOST_VirtualFrame;
struct GHOST_VirtualSlotExport;

class GHOST_WindowWeb : public GHOST_Window {
 public:
  GHOST_WindowWeb(GHOST_SystemWeb *system,
                  const char *title,
                  int32_t left,
                  int32_t top,
                  uint32_t width,
                  uint32_t height,
                  GHOST_TWindowState state,
                  const GHOST_IWindow *parent_window,
                  GHOST_TDrawingContextType type,
                  const GHOST_ContextParams &context_params);

  ~GHOST_WindowWeb() override;

  /* ----- GHOST_Window pure virtuals ----- */

  bool getValid() const override;
  void setTitle(const char *title) override;
  std::string getTitle() const override;
  void getWindowBounds(GHOST_Rect &bounds) const override;
  void getClientBounds(GHOST_Rect &bounds) const override;
  GHOST_TSuccess setClientWidth(uint32_t width) override;
  GHOST_TSuccess setClientHeight(uint32_t height) override;
  GHOST_TSuccess setClientSize(uint32_t width, uint32_t height) override;
  void screenToClient(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;
  void clientToScreen(int32_t inX, int32_t inY, int32_t &outX, int32_t &outY) const override;
  GHOST_TWindowState getState() const override;
  GHOST_TSuccess setState(GHOST_TWindowState state) override;
  GHOST_TSuccess setOrder(GHOST_TWindowOrder order) override;
  GHOST_TSuccess invalidate() override;
  GHOST_TSuccess swapBufferRelease() override;
  GHOST_TSuccess activateDrawingContext() override;
  GHOST_TSuccess hasCursorShape(GHOST_TStandardCursor cursor_shape) override;

  /** Return the FBO that Blender renders into. */
  unsigned int getDefaultFramebuffer() override;

  /** Resize the offscreen FBO (called when browser canvas changes size). */
  void resizeFBO(uint32_t width, uint32_t height);

#ifdef WITH_VULKAN_BACKEND
  /** Frame delivery from the Vulkan virtual swap-chain (called on its harvest thread). */
  void vulkanFramePresent(const GHOST_VirtualFrame &frame);

  /** Number of virtual swap-chain buffer slots. */
  uint32_t vulkanSlotCount();

  /** Export info of a slot's device-local buffer (zero-copy encoder input). */
  bool getVulkanSlotExport(uint32_t slot, GHOST_VirtualSlotExport &r_export);
#endif

 protected:
  GHOST_Context *newDrawingContext(GHOST_TDrawingContextType type) override;
  GHOST_TSuccess setWindowCursorVisibility(bool visible) override;
  GHOST_TSuccess setWindowCursorGrab(GHOST_TGrabCursorMode mode) override;
  GHOST_TSuccess setWindowCursorShape(GHOST_TStandardCursor shape) override;
  GHOST_TSuccess setWindowCustomCursorShape(const uint8_t *bitmap,
                                            const uint8_t *mask,
                                            const int size[2],
                                            const int hot_spot[2],
                                            bool can_invert_color) override;

 private:
  void setupFBO();
  void destroyFBO();

  GHOST_SystemWeb *system_;
  std::string title_;
  uint32_t width_, height_;

#ifdef _WIN32
  HWND hidden_hwnd_ = nullptr;
  HDC hidden_hdc_ = nullptr;
#endif

  /** Offscreen FBO for rendering. */
  unsigned int fbo_id_ = 0;
  unsigned int color_tex_ = 0;
  unsigned int depth_rb_ = 0;

  /** Double-buffered PBO for async GPU readback. */
  unsigned int pbo_[2] = {0, 0};
  int pbo_index_ = 0;
  bool pbo_ready_ = false; /** True after the first frame (PBO pipeline primed). */

  /** Async frame send: compress + send on background thread. */
  std::vector<uint8_t> send_buffer_;
  std::mutex send_mutex_;
  std::atomic<bool> send_busy_{false};

  GHOST_TWindowState window_state_ = GHOST_kWindowStateNormal;
  bool valid_ = false;
  /** True when the drawing context is Vulkan (virtual swap-chain instead of GL FBO). */
  bool is_vulkan_ = false;
};
