/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Declaration of GHOST_SystemWeb class.
 *
 * Web display backend: Blender renders locally with full GPU,
 * streams frames to a browser via WebSocket on localhost.
 */

#pragma once

#include "GHOST_System.hh"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class GHOST_Encoder;
class GHOST_WindowWeb;
struct GHOST_VirtualFrame;
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
#  define INVALID_SOCK INVALID_SOCKET
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
#  define INVALID_SOCK (-1)
#endif

class GHOST_SystemWeb : public GHOST_System {
 public:
  GHOST_SystemWeb();
  ~GHOST_SystemWeb() override;

  /* ----- GHOST_System interface ----- */

  bool processEvents(bool waitForEvent) override;
  bool setConsoleWindowState(GHOST_TConsoleWindowState /*action*/) override
  {
    return false;
  }

  GHOST_TSuccess getModifierKeys(GHOST_ModifierKeys &keys) const override;
  GHOST_TSuccess getButtons(GHOST_Buttons &buttons) const override;

  GHOST_TCapabilityFlag getCapabilities() const override;

  char *getClipboard(bool selection) const override;
  void putClipboard(const char *buffer, bool selection) const override;

  uint64_t getMilliSeconds() const override;
  uint8_t getNumDisplays() const override;

  GHOST_TSuccess getCursorPosition(int32_t &x, int32_t &y) const override;
  GHOST_TSuccess setCursorPosition(int32_t x, int32_t y) override;

  void getMainDisplayDimensions(uint32_t &width, uint32_t &height) const override;
  void getAllDisplayDimensions(uint32_t &width, uint32_t &height) const override;

  GHOST_IContext *createOffscreenContext(GHOST_GPUSettings gpu_settings) override;
  GHOST_TSuccess disposeContext(GHOST_IContext *context) override;

  GHOST_IWindow *getWindowUnderCursor(int32_t x, int32_t y) override;

  /* ----- WebSocket frame sending ----- */

  /** Called by GHOST_WindowWeb on swapBufferRelease to stream the frame. */
  void sendFrame(const uint8_t *rgba_pixels, int width, int height);

#ifdef WITH_VULKAN_BACKEND
  /**
   * Zero-copy H.264 path: encode straight from the window's exported GPU buffer
   * (no host round-trip). Returns true when the frame was handled — encoded and
   * sent, or intentionally dropped (no clients). False = use the CPU pixel path.
   */
  bool sendFrameExternal(GHOST_WindowWeb *window, const GHOST_VirtualFrame &frame);

  /** Drop encoder registrations of the window's exported buffers (resize/teardown). */
  void releaseExternalFrameResources(GHOST_WindowWeb *window);
#endif

  /** Get the active window (the one being streamed). */
  GHOST_IWindow *getActiveWindow() const { return active_window_; }

  /** Get the WebSocket server port. */
  int getPort() const
  {
    return port_;
  }

 private:
  GHOST_TSuccess init() override;

  GHOST_IWindow *createWindow(const char *title,
                              int32_t left,
                              int32_t top,
                              uint32_t width,
                              uint32_t height,
                              GHOST_TWindowState state,
                              GHOST_GPUSettings gpu_settings,
                              const bool exclusive,
                              const bool is_dialog,
                              const GHOST_IWindow *parent_window) override;

  /* ----- WebSocket server internals ----- */

  void serverThreadFunc();
  void acceptNewClients();
  void readClientData();
  void processInputEvent(const std::string &json_str);
  void removeClient(socket_t sock);

  /** Send a WebSocket frame (binary or text). */
  bool wsSendFrame(socket_t sock, const uint8_t *data, size_t len, uint8_t opcode);

  /** Perform the WebSocket upgrade handshake, also serves HTTP for the client page. */
  bool wsHandshake(socket_t client_sock);

  /** Serve the embedded HTML/JS client via HTTP. */
  bool serveHTTP(socket_t sock, const std::string &request);

  /* ----- Members ----- */

  int port_ = 7681;
  std::string bind_addr_ = "127.0.0.1";
  socket_t listen_sock_ = INVALID_SOCK;
  std::vector<socket_t> clients_;
  std::mutex clients_mutex_;

  std::thread server_thread_;
  std::atomic<bool> running_{false};

  /** Input events received from browser, waiting to be processed. */
  std::deque<std::string> input_queue_;
  std::mutex input_mutex_;

  /** Modifier key state tracked from browser events. */
  bool mod_shift_ = false;
  bool mod_ctrl_ = false;
  bool mod_alt_ = false;
  bool mod_os_ = false;

  /** Mouse button state. */
  bool btn_left_ = false;
  bool btn_middle_ = false;
  bool btn_right_ = false;

  /** Last known cursor position. */
  int32_t cursor_x_ = 0;
  int32_t cursor_y_ = 0;

  /** Clipboard storage. */
  mutable std::string clipboard_;

  /** Start time for getMilliSeconds(). */
  uint64_t start_time_ms_ = 0;

  /** Active window — the one we stream and send input to. */
  GHOST_IWindow *active_window_ = nullptr;

  /** Display dimensions (updated when browser sends resize). */
  uint32_t display_width_ = 1280;
  uint32_t display_height_ = 720;

  /** H.264 encoder (NVENC or fallback). nullptr = JPEG mode. */
  std::unique_ptr<GHOST_Encoder> encoder_;
  bool encoder_init_attempted_ = false;
  int encoder_width_ = 0;
  int encoder_height_ = 0;
  int encoder_pending_width_ = 0;   /** Deferred resize: target width (0 = no pending). */
  int encoder_pending_height_ = 0;
  int encoder_pending_frames_ = 0;  /** Count frames at pending size before reinit. */

  /** Zero-copy input registration state (guarded by clients_mutex_). */
  uint64_t external_generation_ = 0;
  void *external_window_ = nullptr;

  /** Last captured JPEG frame for /capture HTTP endpoint. */
  std::vector<uint8_t> capture_jpeg_;
  std::mutex capture_mutex_;

  /* ----- Embedded client HTML/JS ----- */
  static const char *getClientHTML();
  static const char *getClientJS();
};
