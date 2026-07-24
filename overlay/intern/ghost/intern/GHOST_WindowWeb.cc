/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Implementation of GHOST_WindowWeb: offscreen window that renders to an FBO
 * and streams frames to the browser via GHOST_SystemWeb.
 */

#include "GHOST_WindowWeb.hh"
#include "GHOST_SystemWeb.hh"

#ifdef WITH_OPENGL_BACKEND
#  ifdef _WIN32
#    include "GHOST_ContextWGL.hh"
#    include <epoxy/gl.h>
#    include <epoxy/wgl.h>
#  endif
#endif

#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

#include <cstring>

#include "CLG_log.h"

static CLG_LogRef LOG = {"ghost.web.window"};

GHOST_WindowWeb::GHOST_WindowWeb(GHOST_SystemWeb *system,
                                 const char *title,
                                 int32_t /*left*/,
                                 int32_t /*top*/,
                                 uint32_t width,
                                 uint32_t height,
                                 GHOST_TWindowState state,
                                 const GHOST_IWindow * /*parent_window*/,
                                 GHOST_TDrawingContextType type,
                                 const GHOST_ContextParams &context_params)
    : GHOST_Window(width, height, state, context_params, false),
      system_(system),
      title_(title ? title : "Blender"),
      width_(width),
      height_(height)
{
#ifdef _WIN32
  /* Create a hidden window for WGL context. */
  hidden_hwnd_ = CreateWindowExA(0,
                                 "STATIC",
                                 "BlenderWebGhost",
                                 WS_OVERLAPPEDWINDOW,
                                 0,
                                 0,
                                 width,
                                 height,
                                 nullptr,
                                 nullptr,
                                 GetModuleHandle(nullptr),
                                 nullptr);
  if (hidden_hwnd_) {
    hidden_hdc_ = GetDC(hidden_hwnd_);
  }
#endif

  /* Create the drawing context. */
  if (setDrawingContextType(type) == GHOST_kSuccess) {
    valid_ = true;
  }
}

GHOST_WindowWeb::~GHOST_WindowWeb()
{
#ifdef WITH_VULKAN_BACKEND
  if (is_vulkan_) {
    /* Stop frame delivery before our members are destructed (the harvest thread
     * calls back into this window). */
    if (GHOST_ContextVK *context = static_cast<GHOST_ContextVK *>(getContext())) {
      context->shutdownVirtualPresentation();
    }
  }
#endif

  destroyFBO();

#ifdef _WIN32
  if (hidden_hdc_ && hidden_hwnd_) {
    ReleaseDC(hidden_hwnd_, hidden_hdc_);
  }
  if (hidden_hwnd_) {
    DestroyWindow(hidden_hwnd_);
  }
#endif
}

/* -------------------------------------------------------------------- */
/** \name Drawing context creation
 * \{ */

GHOST_Context *GHOST_WindowWeb::newDrawingContext(GHOST_TDrawingContextType type)
{
#ifdef WITH_VULKAN_BACKEND
  if (type == GHOST_kDrawingContextTypeVulkan) {
    GHOST_ContextVK *context = new GHOST_ContextVK(want_context_params_,
#  ifdef _WIN32
                                                   HWND(nullptr),
#  elif defined(__APPLE__)
                                                   nullptr,
#  else
                                                   GHOST_kVulkanPlatformHeadless,
                                                   Window(nullptr),
                                                   nullptr,
                                                   nullptr,
                                                   nullptr,
                                                   nullptr,
#  endif
                                                   1,
                                                   2,
                                                   GHOST_GPUDevice{});
    if (context->initializeDrawingContext()) {
      /* Capture by value: the teardown callback can fire from the base-class destructor
       * after this window's members were destructed. */
      GHOST_SystemWeb *system = system_;
      GHOST_WindowWeb *window = this;
      context->initVirtualSwapchain(
          width_,
          height_,
          [window](const GHOST_VirtualFrame &frame) { window->vulkanFramePresent(frame); },
          [system, window]() { system->releaseExternalFrameResources(window); });
      is_vulkan_ = true;
      CLOG_INFO(&LOG, "Created Vulkan context with virtual swapchain for web window");
      return context;
    }
    delete context;
    return nullptr;
  }
#endif

#if defined(WITH_OPENGL_BACKEND) && defined(_WIN32)
  if (type == GHOST_kDrawingContextTypeOpenGL && hidden_hwnd_ && hidden_hdc_) {
    for (int minor = 6; minor >= 3; --minor) {
      GHOST_ContextWGL *context = new GHOST_ContextWGL(
          want_context_params_,
          false,
          hidden_hwnd_,
          hidden_hdc_,
          WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
          4,
          minor,
          (want_context_params_.is_debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0),
          GHOST_OPENGL_WGL_RESET_NOTIFICATION_STRATEGY);

      if (context->initializeDrawingContext()) {
        CLOG_INFO(&LOG, "Created OpenGL 4.%d WGL context for web window", minor);
        /* Activate and create FBO. */
        context->activateDrawingContext();
        setupFBO();
        return context;
      }
      delete context;
    }
  }
#else
  (void)type;
#endif
  return nullptr;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name FBO management
 * \{ */

void GHOST_WindowWeb::setupFBO()
{
  destroyFBO();

  glGenFramebuffers(1, &fbo_id_);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo_id_);

  /* Color texture. */
  glGenTextures(1, &color_tex_);
  glBindTexture(GL_TEXTURE_2D, color_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex_, 0);

  /* Depth + stencil renderbuffer. */
  glGenRenderbuffers(1, &depth_rb_);
  glBindRenderbuffer(GL_RENDERBUFFER, depth_rb_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
  glFramebufferRenderbuffer(
      GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb_);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    CLOG_ERROR(&LOG, "FBO incomplete: 0x%x", status);
    destroyFBO();
  }
  else {
    CLOG_INFO(&LOG, "Created FBO %u (%ux%u)", fbo_id_, width_, height_);

    /* Create double-buffered PBOs for async readback. */
    const size_t buf_size = width_ * height_ * 4;
    glGenBuffers(2, pbo_);
    for (int i = 0; i < 2; i++) {
      glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo_[i]);
      glBufferData(GL_PIXEL_PACK_BUFFER, buf_size, nullptr, GL_STREAM_READ);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pbo_index_ = 0;
    pbo_ready_ = false;
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GHOST_WindowWeb::destroyFBO()
{
  if (pbo_[0] || pbo_[1]) {
    glDeleteBuffers(2, pbo_);
    pbo_[0] = pbo_[1] = 0;
    pbo_ready_ = false;
  }
  if (fbo_id_) {
    glDeleteFramebuffers(1, &fbo_id_);
    fbo_id_ = 0;
  }
  if (color_tex_) {
    glDeleteTextures(1, &color_tex_);
    color_tex_ = 0;
  }
  if (depth_rb_) {
    glDeleteRenderbuffers(1, &depth_rb_);
    depth_rb_ = 0;
  }
}

void GHOST_WindowWeb::resizeFBO(uint32_t width, uint32_t height)
{
  if (width == width_ && height == height_) {
    return;
  }
  width_ = width;
  height_ = height;

#ifdef WITH_VULKAN_BACKEND
  if (is_vulkan_) {
    if (GHOST_ContextVK *context = static_cast<GHOST_ContextVK *>(getContext())) {
      context->resizeVirtualSwapchain(width, height);
    }
    return;
  }
#endif

  if (fbo_id_ && getContext()) {
    getContext()->activateDrawingContext();
    setupFBO();
  }
}

unsigned int GHOST_WindowWeb::getDefaultFramebuffer()
{
  return fbo_id_;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame capture and swap
 * \{ */

GHOST_TSuccess GHOST_WindowWeb::swapBufferRelease()
{
#ifdef WITH_VULKAN_BACKEND
  if (is_vulkan_) {
    /* Virtual presentation: the context downloads the frame and calls
     * #vulkanFramePresent with the pixel data. */
    return getContext() ? getContext()->swapBufferRelease() : GHOST_kFailure;
  }
#endif

  if (!fbo_id_) {
    return GHOST_kSuccess;
  }

  /* Only send frames from the active window. */
  if (system_->getActiveWindow() != this) {
    return GHOST_kSuccess;
  }

  /* Skip this frame if the previous one is still being sent. */
  if (send_busy_) {
    return GHOST_kSuccess;
  }

  /* Read pixels from FBO into send_buffer_. */
  const size_t buf_size = width_ * height_ * 4;
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_buffer_.resize(buf_size);
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_id_);
  glReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, send_buffer_.data());
  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

  /* Compress + send on background thread. */
  send_busy_ = true;
  uint32_t w = width_, h = height_;
  std::thread([this, w, h]() {
    system_->sendFrame(send_buffer_.data(), w, h);
    send_busy_ = false;
  }).detach();

  return GHOST_kSuccess;
}

#ifdef WITH_VULKAN_BACKEND

void GHOST_WindowWeb::vulkanFramePresent(const GHOST_VirtualFrame &frame)
{
  /* Only send frames from the active window. */
  if (system_->getActiveWindow() != this) {
    return;
  }

  /* Zero-copy H.264 path: encode straight from the exported GPU buffer.
   * We are already on the harvest thread, everything below is synchronous. */
  if (system_->sendFrameExternal(this, frame)) {
    return;
  }

  /* CPU fallback (JPEG / resize / no NVENC): #GHOST_SystemWeb::sendFrame expects
   * tightly packed bottom-up rows, the virtual swap-chain delivers top-down
   * pitched rows — flip and pack while copying. */
  const size_t row_size = size_t(frame.width) * 4;
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_buffer_.resize(row_size * frame.height);
    for (uint32_t y = 0; y < frame.height; y++) {
      memcpy(send_buffer_.data() + (frame.height - 1 - y) * row_size,
             frame.host_pixels + size_t(y) * frame.pitch,
             row_size);
    }
  }
  system_->sendFrame(send_buffer_.data(), frame.width, frame.height);
}

uint32_t GHOST_WindowWeb::vulkanSlotCount()
{
  GHOST_ContextVK *context = static_cast<GHOST_ContextVK *>(getContext());
  return context ? context->getVirtualSlotCount() : 0;
}

bool GHOST_WindowWeb::getVulkanSlotExport(uint32_t slot, GHOST_VirtualSlotExport &r_export)
{
  GHOST_ContextVK *context = static_cast<GHOST_ContextVK *>(getContext());
  return context && context->getVirtualSlotExport(slot, r_export) == GHOST_kSuccess;
}

#endif /* WITH_VULKAN_BACKEND */

GHOST_TSuccess GHOST_WindowWeb::activateDrawingContext()
{
  if (getContext()) {
    return getContext()->activateDrawingContext();
  }
  return GHOST_kFailure;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Window properties (simple implementations)
 * \{ */

bool GHOST_WindowWeb::getValid() const
{
  return valid_;
}

void GHOST_WindowWeb::setTitle(const char *title)
{
  title_ = title ? title : "";
}

std::string GHOST_WindowWeb::getTitle() const
{
  return title_;
}

void GHOST_WindowWeb::getWindowBounds(GHOST_Rect &bounds) const
{
  getClientBounds(bounds);
}

void GHOST_WindowWeb::getClientBounds(GHOST_Rect &bounds) const
{
  bounds.l_ = 0;
  bounds.t_ = 0;
  bounds.r_ = width_;
  bounds.b_ = height_;
}

GHOST_TSuccess GHOST_WindowWeb::setClientWidth(uint32_t width)
{
  resizeFBO(width, height_);
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setClientHeight(uint32_t height)
{
  resizeFBO(width_, height);
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setClientSize(uint32_t width, uint32_t height)
{
  resizeFBO(width, height);
  return GHOST_kSuccess;
}

void GHOST_WindowWeb::screenToClient(int32_t inX,
                                     int32_t inY,
                                     int32_t &outX,
                                     int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

void GHOST_WindowWeb::clientToScreen(int32_t inX,
                                     int32_t inY,
                                     int32_t &outX,
                                     int32_t &outY) const
{
  outX = inX;
  outY = inY;
}

GHOST_TWindowState GHOST_WindowWeb::getState() const
{
  return window_state_;
}

GHOST_TSuccess GHOST_WindowWeb::setState(GHOST_TWindowState state)
{
  window_state_ = state;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setOrder(GHOST_TWindowOrder order)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::invalidate()
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::hasCursorShape(GHOST_TStandardCursor /*cursor_shape*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setWindowCursorVisibility(bool /*visible*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setWindowCursorGrab(GHOST_TGrabCursorMode mode)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setWindowCursorShape(GHOST_TStandardCursor /*shape*/)
{
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_WindowWeb::setWindowCustomCursorShape(const uint8_t * /*bitmap*/,
                                                           const uint8_t * /*mask*/,
                                                           const int /*size*/[2],
                                                           const int /*hot_spot*/[2],
                                                           bool /*can_invert_color*/)
{
  return GHOST_kSuccess;
}

/** \} */
