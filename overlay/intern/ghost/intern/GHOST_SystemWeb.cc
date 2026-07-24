/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 * Implementation of GHOST_SystemWeb: Blender runs natively,
 * browser acts as display via WebSocket on localhost.
 */

#include "GHOST_SystemWeb.hh"
#include "GHOST_EventButton.hh"
#include "GHOST_EventCursor.hh"
#include "GHOST_EventKey.hh"
#include "GHOST_EventWheel.hh"
#include "GHOST_EventTrackpad.hh"
#include "GHOST_WindowManager.hh"
#include "GHOST_WindowWeb.hh"

#include "GHOST_Encoder.hh"
#ifdef WITH_WEB_NVENC
#  include "GHOST_EncoderNVENC.hh"
#endif

#if defined(WITH_OPENGL_BACKEND) && defined(_WIN32)
#  include "GHOST_ContextWGL.hh"
#endif

#ifdef WITH_VULKAN_BACKEND
#  include "GHOST_ContextVK.hh"
#endif

#include "CLG_log.h"

#include <jpeglib.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

static CLG_LogRef LOG = {"ghost.web"};

/* -------------------------------------------------------------------- */
/** \name Minimal SHA-1 (for WebSocket handshake only)
 * \{ */

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
  uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
           h4 = 0xC3D2E1F0;

  /* Pre-processing: pad message. */
  size_t new_len = len + 1;
  while (new_len % 64 != 56) {
    new_len++;
  }
  std::vector<uint8_t> msg(new_len + 8, 0);
  memcpy(msg.data(), data, len);
  msg[len] = 0x80;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) {
    msg[new_len + i] = (uint8_t)(bits >> (56 - 8 * i));
  }

  auto left_rotate = [](uint32_t v, int n) -> uint32_t {
    return (v << n) | (v >> (32 - n));
  };

  for (size_t offset = 0; offset < msg.size(); offset += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
      w[i] = ((uint32_t)msg[offset + 4 * i] << 24) | ((uint32_t)msg[offset + 4 * i + 1] << 16) |
             ((uint32_t)msg[offset + 4 * i + 2] << 8) | (uint32_t)msg[offset + 4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
      w[i] = left_rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999;
      }
      else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      }
      else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      }
      else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = left_rotate(b, 30);
      b = a;
      a = temp;
    }
    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
  }

  auto store32 = [](uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
  };
  store32(out + 0, h0);
  store32(out + 4, h1);
  store32(out + 8, h2);
  store32(out + 12, h3);
  store32(out + 16, h4);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Base64 encode (for WebSocket handshake)
 * \{ */

static std::string base64_encode(const uint8_t *data, size_t len)
{
  static const char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string result;
  result.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = ((uint32_t)data[i]) << 16;
    if (i + 1 < len) {
      n |= ((uint32_t)data[i + 1]) << 8;
    }
    if (i + 2 < len) {
      n |= (uint32_t)data[i + 2];
    }
    result += table[(n >> 18) & 0x3F];
    result += table[(n >> 12) & 0x3F];
    result += (i + 1 < len) ? table[(n >> 6) & 0x3F] : '=';
    result += (i + 2 < len) ? table[n & 0x3F] : '=';
  }
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Minimal JSON helpers (avoid dependency on full JSON library)
 * \{ */

static std::string json_get_string(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos) {
    return "";
  }
  size_t end = json.find('"', pos + 1);
  if (end == std::string::npos) {
    return "";
  }
  return json.substr(pos + 1, end - pos - 1);
}

static double json_get_number(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return 0;
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return 0;
  }
  pos++;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }
  return std::atof(json.c_str() + pos);
}

static bool json_get_bool(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return false;
  }
  return json.find("true", pos) == pos + 1 || json.find("true", pos) == pos + 2;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Key code mapping: browser KeyboardEvent.code -> GHOST_TKey
 * \{ */

static GHOST_TKey web_key_to_ghost(const std::string &code)
{
  /* Letters. */
  if (code.size() == 4 && code[0] == 'K' && code[1] == 'e' && code[2] == 'y') {
    char c = code[3];
    if (c >= 'A' && c <= 'Z') {
      return (GHOST_TKey)c;
    }
  }
  /* Digits. */
  if (code.size() == 6 && code.substr(0, 5) == "Digit") {
    return (GHOST_TKey)code[5];
  }
  /* Numpad digits. */
  if (code.size() == 7 && code.substr(0, 6) == "Numpad") {
    char c = code[6];
    if (c >= '0' && c <= '9') {
      return (GHOST_TKey)(GHOST_kKeyNumpad0 + (c - '0'));
    }
  }
  /* Function keys. */
  if (code[0] == 'F' && code.size() >= 2 && code.size() <= 3) {
    int num = std::atoi(code.c_str() + 1);
    if (num >= 1 && num <= 24) {
      return (GHOST_TKey)(GHOST_kKeyF1 + num - 1);
    }
  }

  /* Named keys. */
  if (code == "Space") return GHOST_kKeySpace;
  if (code == "Enter") return GHOST_kKeyEnter;
  if (code == "NumpadEnter") return GHOST_kKeyNumpadEnter;
  if (code == "Backspace") return GHOST_kKeyBackSpace;
  if (code == "Tab") return GHOST_kKeyTab;
  if (code == "Escape") return GHOST_kKeyEsc;
  if (code == "Delete") return GHOST_kKeyDelete;
  if (code == "Insert") return GHOST_kKeyInsert;
  if (code == "Home") return GHOST_kKeyHome;
  if (code == "End") return GHOST_kKeyEnd;
  if (code == "PageUp") return GHOST_kKeyUpPage;
  if (code == "PageDown") return GHOST_kKeyDownPage;
  if (code == "ArrowLeft") return GHOST_kKeyLeftArrow;
  if (code == "ArrowRight") return GHOST_kKeyRightArrow;
  if (code == "ArrowUp") return GHOST_kKeyUpArrow;
  if (code == "ArrowDown") return GHOST_kKeyDownArrow;
  if (code == "ShiftLeft") return GHOST_kKeyLeftShift;
  if (code == "ShiftRight") return GHOST_kKeyRightShift;
  if (code == "ControlLeft") return GHOST_kKeyLeftControl;
  if (code == "ControlRight") return GHOST_kKeyRightControl;
  if (code == "AltLeft") return GHOST_kKeyLeftAlt;
  if (code == "AltRight") return GHOST_kKeyRightAlt;
  if (code == "MetaLeft") return GHOST_kKeyLeftOS;
  if (code == "MetaRight") return GHOST_kKeyRightOS;
  if (code == "CapsLock") return GHOST_kKeyCapsLock;
  if (code == "NumLock") return GHOST_kKeyNumLock;
  if (code == "ScrollLock") return GHOST_kKeyScrollLock;
  if (code == "Minus" || code == "NumpadSubtract") return GHOST_kKeyMinus;
  if (code == "Equal") return GHOST_kKeyEqual;
  if (code == "NumpadAdd") return GHOST_kKeyNumpadPlus;
  if (code == "NumpadMultiply") return GHOST_kKeyNumpadAsterisk;
  if (code == "NumpadDivide") return GHOST_kKeyNumpadSlash;
  if (code == "NumpadDecimal") return GHOST_kKeyNumpadPeriod;
  if (code == "BracketLeft") return GHOST_kKeyLeftBracket;
  if (code == "BracketRight") return GHOST_kKeyRightBracket;
  if (code == "Semicolon") return GHOST_kKeySemicolon;
  if (code == "Quote") return GHOST_kKeyQuote;
  if (code == "Backslash") return GHOST_kKeyBackslash;
  if (code == "Comma") return GHOST_kKeyComma;
  if (code == "Period") return GHOST_kKeyPeriod;
  if (code == "Slash") return GHOST_kKeySlash;
  if (code == "Backquote") return GHOST_kKeyAccentGrave;

  return GHOST_kKeyUnknown;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Socket helpers
 * \{ */

static void set_nonblocking(socket_t sock)
{
#ifdef _WIN32
  u_long mode = 1;
  ioctlsocket(sock, FIONBIO, &mode);
#else
  int flags = fcntl(sock, F_GETFL, 0);
  fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

static void close_socket(socket_t sock)
{
#ifdef _WIN32
  closesocket(sock);
#else
  close(sock);
#endif
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GHOST_SystemWeb implementation
 * \{ */

GHOST_SystemWeb::GHOST_SystemWeb()
{
  auto now = std::chrono::steady_clock::now();
  start_time_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                        .count();

  /* BLENDER_WEB_PORT: custom port (default 7681).
   * BLENDER_WEB_BIND: bind address (default 127.0.0.1, use 0.0.0.0 for LAN access). */
  const char *port_env = getenv("BLENDER_WEB_PORT");
  if (port_env) {
    int p = std::atoi(port_env);
    if (p > 0 && p < 65536) {
      port_ = p;
    }
  }
  const char *bind_env = getenv("BLENDER_WEB_BIND");
  if (bind_env) {
    bind_addr_ = bind_env;
  }
}

GHOST_SystemWeb::~GHOST_SystemWeb()
{
  running_ = false;

  if (server_thread_.joinable()) {
    server_thread_.join();
  }

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (socket_t s : clients_) {
      close_socket(s);
    }
    clients_.clear();
  }

  if (listen_sock_ != INVALID_SOCK) {
    close_socket(listen_sock_);
  }

#ifdef _WIN32
  WSACleanup();
#endif
}

GHOST_TSuccess GHOST_SystemWeb::init()
{
  GHOST_TSuccess success = GHOST_System::init();
  if (success != GHOST_kSuccess) {
    return GHOST_kFailure;
  }

#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
    CLOG_ERROR(&LOG, "WSAStartup failed");
    return GHOST_kFailure;
  }
#endif

  /* Create listening socket. */
  listen_sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_sock_ == INVALID_SOCK) {
    CLOG_ERROR(&LOG, "Failed to create listen socket");
    return GHOST_kFailure;
  }

  /* Allow port reuse. */
  int opt = 1;
  setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  if (bind_addr_ == "0.0.0.0") {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  else {
    inet_pton(AF_INET, bind_addr_.c_str(), &addr.sin_addr);
  }
  addr.sin_port = htons((uint16_t)port_);

  if (bind(listen_sock_, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    CLOG_ERROR(&LOG, "Failed to bind on port %d", port_);
    close_socket(listen_sock_);
    listen_sock_ = INVALID_SOCK;
    return GHOST_kFailure;
  }

  if (listen(listen_sock_, 4) != 0) {
    CLOG_ERROR(&LOG, "Failed to listen on port %d", port_);
    close_socket(listen_sock_);
    listen_sock_ = INVALID_SOCK;
    return GHOST_kFailure;
  }

  set_nonblocking(listen_sock_);

  running_ = true;
  server_thread_ = std::thread(&GHOST_SystemWeb::serverThreadFunc, this);

  fprintf(stderr,
          "\n"
          "==============================================\n"
          "  Blender Web Display\n"
          "  Bind: %s:%d\n"
          "  Open in browser: http://localhost:%d\n"
          "==============================================\n\n",
          bind_addr_.c_str(), port_, port_);

  return GHOST_kSuccess;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Server thread
 * \{ */

static bool ws_do_handshake(socket_t sock, const std::string &request)
{
  /* Extract Sec-WebSocket-Key. */
  std::string key_header = "Sec-WebSocket-Key: ";
  size_t key_pos = request.find(key_header);
  if (key_pos == std::string::npos) {
    return false;
  }
  key_pos += key_header.size();
  size_t key_end = request.find("\r\n", key_pos);
  if (key_end == std::string::npos) {
    return false;
  }
  std::string ws_key = request.substr(key_pos, key_end - key_pos);

  /* Compute accept key per RFC 6455. */
  std::string accept_input = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  uint8_t hash[20];
  sha1((const uint8_t *)accept_input.c_str(), accept_input.size(), hash);
  std::string accept_key = base64_encode(hash, 20);

  /* Send upgrade response. */
  std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: " +
                         accept_key + "\r\n\r\n";

  int sent = send(sock, response.c_str(), (int)response.size(), 0);
  return sent > 0;
}

void GHOST_SystemWeb::serverThreadFunc()
{
  while (running_) {
    acceptNewClients();
    readClientData();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

void GHOST_SystemWeb::acceptNewClients()
{
  /* Accept ALL pending connections in a loop (non-blocking listen socket). */
  for (;;) {
    struct sockaddr_in client_addr = {};
    int addr_len = sizeof(client_addr);

    socket_t client = accept(listen_sock_, (struct sockaddr *)&client_addr, &addr_len);
    if (client == INVALID_SOCK) {
      break; /* No more pending connections. */
    }

    /* Keep socket BLOCKING for the handshake phase (just like Python's server).
     * Use a recv timeout to avoid hanging the thread forever. */
#ifdef _WIN32
    DWORD recv_timeout = 2000; /* 2 seconds. */
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&recv_timeout, sizeof(recv_timeout));
#else
    struct timeval recv_timeout = {2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
#endif

    /* Enable TCP_NODELAY for low latency. */
    int nodelay = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    /* Read the HTTP request (blocking with timeout). */
    char buf[8192] = {};
    int total = 0;
    bool request_complete = false;
    while (total < (int)sizeof(buf) - 1) {
      int n = recv(client, buf + total, sizeof(buf) - 1 - total, 0);
      if (n > 0) {
        total += n;
        if (total >= 4 && std::string(buf, total).find("\r\n\r\n") != std::string::npos) {
          request_complete = true;
          break;
        }
      }
      else {
        break;
      }
    }

    if (total <= 0 || !request_complete) {
      close_socket(client);
      continue;
    }

    buf[total] = '\0';
    std::string request(buf, total);

    /* Check for WebSocket upgrade. */
    if (request.find("Upgrade: websocket") != std::string::npos ||
        request.find("Upgrade: WebSocket") != std::string::npos)
    {
      if (ws_do_handshake(client, request)) {
        /* Switch to non-blocking AFTER handshake response is sent. */
        set_nonblocking(client);
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(client);
#ifdef WITH_WEB_NVENC
        /* Force next frame to be a keyframe so new client can decode. */
        if (encoder_) {
          encoder_->requestKeyframe();
        }
#endif
      }
      else {
        close_socket(client);
      }
    }
    else {
      /* Serve HTTP response. */
      serveHTTP(client, request);
      shutdown(client, SD_BOTH);
      close_socket(client);
    }
  }
}

void GHOST_SystemWeb::readClientData()
{
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (clients_.empty()) {
    return;
  }

  /* Use select() to check which sockets have data ready. */
  fd_set readfds, errfds;
  FD_ZERO(&readfds);
  FD_ZERO(&errfds);
  for (socket_t sock : clients_) {
    FD_SET(sock, &readfds);
    FD_SET(sock, &errfds);
  }
  struct timeval tv = {0, 0}; /* Non-blocking check. */
  int sel = select(0, &readfds, nullptr, &errfds, &tv);
  if (sel <= 0) {
    return; /* No data available on any socket. */
  }

  std::vector<socket_t> dead;

  for (socket_t sock : clients_) {
    if (FD_ISSET(sock, &errfds)) {
      dead.push_back(sock);
      continue;
    }
    if (!FD_ISSET(sock, &readfds)) {
      continue; /* No data on this socket. */
    }

    /* Read WebSocket frame header (2 bytes). */
    uint8_t header[2];
    int n = recv(sock, (char *)header, 2, 0);
    if (n <= 0) {
      dead.push_back(sock);
      continue;
    }
    if (n < 2) {
      /* Partial read, put back for next time - but for simplicity, drop. */
      dead.push_back(sock);
      continue;
    }

    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payload_len = header[1] & 0x7F;

    if (payload_len == 126) {
      uint8_t ext[2];
      if (recv(sock, (char *)ext, 2, 0) < 2) {
        dead.push_back(sock);
        continue;
      }
      payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    }
    else if (payload_len == 127) {
      uint8_t ext[8];
      if (recv(sock, (char *)ext, 8, 0) < 8) {
        dead.push_back(sock);
        continue;
      }
      payload_len = 0;
      for (int i = 0; i < 8; i++) {
        payload_len = (payload_len << 8) | ext[i];
      }
    }

    uint8_t mask_key[4] = {};
    if (masked) {
      if (recv(sock, (char *)mask_key, 4, 0) < 4) {
        dead.push_back(sock);
        continue;
      }
    }

    /* Read payload. */
    if (payload_len > 1024 * 1024) {
      /* Sanity limit: 1MB max for input events. */
      dead.push_back(sock);
      continue;
    }
    std::vector<uint8_t> payload(payload_len);
    size_t total_read = 0;
    while (total_read < payload_len) {
      n = recv(sock, (char *)payload.data() + total_read, (int)(payload_len - total_read), 0);
      if (n <= 0) {
        break;
      }
      total_read += n;
    }

    if (total_read < payload_len) {
      dead.push_back(sock);
      continue;
    }

    /* Unmask. */
    if (masked) {
      for (size_t i = 0; i < payload_len; i++) {
        payload[i] ^= mask_key[i % 4];
      }
    }

    if (opcode == 0x08) {
      /* Close frame. */
      dead.push_back(sock);
    }
    else if (opcode == 0x09) {
      /* Ping -> Pong. */
      wsSendFrame(sock, payload.data(), payload.size(), 0x0A);
    }
    else if (opcode == 0x01) {
      /* Text frame = input event JSON. */
      std::string text((char *)payload.data(), payload.size());
      std::lock_guard<std::mutex> ilock(input_mutex_);
      input_queue_.push_back(std::move(text));
    }
  }

  for (socket_t s : dead) {
    close_socket(s);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name WebSocket protocol
 * \{ */

bool GHOST_SystemWeb::wsSendFrame(socket_t sock, const uint8_t *data, size_t len, uint8_t opcode)
{
  std::vector<uint8_t> frame;

  /* FIN + opcode. */
  frame.push_back(0x80 | opcode);

  /* Payload length (server frames are NOT masked). */
  if (len < 126) {
    frame.push_back((uint8_t)len);
  }
  else if (len < 65536) {
    frame.push_back(126);
    frame.push_back((uint8_t)(len >> 8));
    frame.push_back((uint8_t)(len & 0xFF));
  }
  else {
    frame.push_back(127);
    for (int i = 7; i >= 0; i--) {
      frame.push_back((uint8_t)(len >> (8 * i)));
    }
  }

  frame.insert(frame.end(), data, data + len);

  size_t total_sent = 0;
  while (total_sent < frame.size()) {
    int n = send(sock, (const char *)frame.data() + total_sent, (int)(frame.size() - total_sent), 0);
    if (n > 0) {
      total_sent += n;
    }
    else if (n < 0) {
#ifdef _WIN32
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        /* Send buffer full. Wait briefly for space, then retry. */
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {0, 50000}; /* 50ms */
        if (select(0, nullptr, &wfds, nullptr, &tv) <= 0) {
          return false; /* Timeout or error — drop this frame. */
        }
        continue;
      }
#else
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = {0, 50000};
        if (select(sock + 1, nullptr, &wfds, nullptr, &tv) <= 0) {
          return false;
        }
        continue;
      }
#endif
      return false; /* Real error — connection dead. */
    }
    else {
      return false; /* Connection closed. */
    }
  }
  return true;
}

bool GHOST_SystemWeb::serveHTTP(socket_t sock, const std::string &request)
{
  /* Parse request path. */
  std::string path = "/";
  if (request.substr(0, 4) == "GET ") {
    size_t end = request.find(' ', 4);
    if (end != std::string::npos) {
      path = request.substr(4, end - 4);
    }
  }

  /* /capture — return latest viewport frame as JPEG (for ComfyUI integration). */
  if (path == "/capture") {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (capture_jpeg_.empty()) {
      std::string r = "HTTP/1.1 503 No Frame\r\nConnection: close\r\n\r\n";
      send(sock, r.c_str(), (int)r.size(), 0);
      return true;
    }
    std::ostringstream hdr;
    hdr << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: image/jpeg\r\n"
        << "Content-Length: " << capture_jpeg_.size() << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "\r\n";
    std::string header = hdr.str();
    send(sock, header.c_str(), (int)header.size(), 0);
    send(sock, (const char *)capture_jpeg_.data(), (int)capture_jpeg_.size(), 0);
    return true;
  }

  const char *content_type = "text/html";
  const char *body = getClientHTML();

  if (path == "/client.js") {
    content_type = "application/javascript";
    body = getClientJS();
  }

  std::ostringstream resp;
  resp << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << strlen(body) << "\r\n"
       << "Access-Control-Allow-Origin: *\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;

  std::string response = resp.str();
  send(sock, response.c_str(), (int)response.size(), 0);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name GHOST_ISystem interface implementation
 * \{ */

GHOST_IWindow *GHOST_SystemWeb::createWindow(const char *title,
                                             int32_t left,
                                             int32_t top,
                                             uint32_t width,
                                             uint32_t height,
                                             GHOST_TWindowState state,
                                             GHOST_GPUSettings gpu_settings,
                                             const bool /*exclusive*/,
                                             const bool /*is_dialog*/,
                                             const GHOST_IWindow *parent_window)
{
  const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS(gpu_settings);

  GHOST_WindowWeb *window = new GHOST_WindowWeb(this,
                                                title,
                                                left,
                                                top,
                                                width,
                                                height,
                                                state,
                                                parent_window,
                                                gpu_settings.context_type,
                                                context_params);

  if (window->getValid()) {
    if (window_manager_->addWindow(window) == GHOST_kSuccess) {
      uint64_t ms = getMilliSeconds();
      pushEvent(std::make_unique<GHOST_Event>(ms, GHOST_kEventWindowSize, window));
      /* Activate the window so it receives input events. */
      pushEvent(std::make_unique<GHOST_Event>(ms, GHOST_kEventWindowActivate, window));
      /* New window becomes the active one (stream frames + receive input). */
      active_window_ = window;
      return window;
    }
  }

  delete window;
  return nullptr;
}

bool GHOST_SystemWeb::processEvents(bool /*waitForEvent*/)
{
  bool has_events = false;

  /* If active window was disposed, fall back to the first remaining window. */
  if (active_window_ && !window_manager_->getWindowFound(active_window_)) {
    const std::vector<GHOST_IWindow *> &windows = window_manager_->getWindows();
    active_window_ = windows.empty() ? nullptr : windows[0];
    if (active_window_) {
      uint64_t ms = getMilliSeconds();
      pushEvent(std::make_unique<GHOST_Event>(ms, GHOST_kEventWindowActivate, active_window_));
      pushEvent(std::make_unique<GHOST_Event>(ms, GHOST_kEventWindowSize, active_window_));
      has_events = true;
    }
  }

  /* Drain input queue. */
  std::deque<std::string> events;
  {
    std::lock_guard<std::mutex> lock(input_mutex_);
    events.swap(input_queue_);
  }

  for (const std::string &json : events) {
    processInputEvent(json);
    has_events = true;
  }

  return has_events;
}

void GHOST_SystemWeb::processInputEvent(const std::string &json)
{
  std::string type = json_get_string(json, "type");
  /* Send events to the active window. */
  GHOST_IWindow *window = active_window_;
  if (!window) {
    const std::vector<GHOST_IWindow *> &windows = window_manager_->getWindows();
    if (!windows.empty()) {
      window = windows[0];
    }
  }
  if (!window) {
    return;
  }

  uint64_t ms = getMilliSeconds();

  if (type == "mousemove") {
    int32_t x = (int32_t)json_get_number(json, "x");
    int32_t y = (int32_t)json_get_number(json, "y");
    cursor_x_ = x;
    cursor_y_ = y;
    pushEvent(std::make_unique<GHOST_EventCursor>(
        ms, GHOST_kEventCursorMove, window, x, y, GHOST_TABLET_DATA_NONE));
  }
  else if (type == "mousedown" || type == "mouseup") {
    int32_t x = (int32_t)json_get_number(json, "x");
    int32_t y = (int32_t)json_get_number(json, "y");
    int button = (int)json_get_number(json, "button");
    GHOST_TButton gbtn = GHOST_kButtonMaskLeft;
    if (button == 1) gbtn = GHOST_kButtonMaskMiddle;
    else if (button == 2) gbtn = GHOST_kButtonMaskRight;

    GHOST_TEventType etype = (type == "mousedown") ? GHOST_kEventButtonDown :
                                                     GHOST_kEventButtonUp;

    if (type == "mousedown") {
      if (gbtn == GHOST_kButtonMaskLeft) btn_left_ = true;
      else if (gbtn == GHOST_kButtonMaskMiddle) btn_middle_ = true;
      else if (gbtn == GHOST_kButtonMaskRight) btn_right_ = true;
    }
    else {
      if (gbtn == GHOST_kButtonMaskLeft) btn_left_ = false;
      else if (gbtn == GHOST_kButtonMaskMiddle) btn_middle_ = false;
      else if (gbtn == GHOST_kButtonMaskRight) btn_right_ = false;
    }

    /* Send cursor position before button event so Blender knows where the click is. */
    cursor_x_ = x;
    cursor_y_ = y;
    pushEvent(std::make_unique<GHOST_EventCursor>(
        ms, GHOST_kEventCursorMove, window, x, y, GHOST_TABLET_DATA_NONE));
    pushEvent(std::make_unique<GHOST_EventButton>(
        ms, etype, window, gbtn, GHOST_TABLET_DATA_NONE));
  }
  else if (type == "wheel") {
    int32_t deltaY = (int32_t)json_get_number(json, "deltaY");
    /* Browser deltaY: positive = scroll down, Blender wheel: positive = up. */
    int32_t ticks = (deltaY > 0) ? -1 : 1;
    pushEvent(std::make_unique<GHOST_EventWheel>(
        ms, window, GHOST_kEventWheelAxisVertical, ticks));
  }
  else if (type == "keydown" || type == "keyup") {
    std::string code = json_get_string(json, "code");
    GHOST_TKey gkey = web_key_to_ghost(code);
    GHOST_TEventType etype = (type == "keydown") ? GHOST_kEventKeyDown : GHOST_kEventKeyUp;
    bool is_repeat = json_get_bool(json, "repeat");

    /* Update modifier state. */
    mod_shift_ = json_get_bool(json, "shiftKey");
    mod_ctrl_ = json_get_bool(json, "ctrlKey");
    mod_alt_ = json_get_bool(json, "altKey");
    mod_os_ = json_get_bool(json, "metaKey");

    /* Get UTF-8 character. */
    std::string key_char = json_get_string(json, "key");
    char utf8_buf[6] = {};
    if (key_char.size() == 1) {
      utf8_buf[0] = key_char[0];
    }
    else if (key_char.size() > 1 && key_char.size() <= 4) {
      memcpy(utf8_buf, key_char.c_str(), std::min(key_char.size(), (size_t)6));
    }

    pushEvent(std::make_unique<GHOST_EventKey>(
        ms, etype, window, gkey, is_repeat, utf8_buf));

    /* Escape closes temporary windows (Preferences, file browser, etc.)
     * since we have no native title bar with close button. */
    if (etype == GHOST_kEventKeyDown && gkey == GHOST_kKeyEsc) {
      const std::vector<GHOST_IWindow *> &wins = window_manager_->getWindows();
      if (wins.size() > 1 && window == active_window_) {
        pushEvent(std::make_unique<GHOST_Event>(ms, GHOST_kEventWindowClose, window));
      }
    }
  }
  else if (type == "resize") {
    uint32_t w = (uint32_t)json_get_number(json, "width");
    uint32_t h = (uint32_t)json_get_number(json, "height");
    if (w > 0 && h > 0) {
      display_width_ = w;
      display_height_ = h;
      GHOST_WindowWeb *web_win = static_cast<GHOST_WindowWeb *>(window);
      web_win->resizeFBO(w, h);
      pushEvent(std::make_unique<GHOST_Event>(
          ms, GHOST_kEventWindowSize, window));
    }
  }
}

GHOST_TSuccess GHOST_SystemWeb::getModifierKeys(GHOST_ModifierKeys &keys) const
{
  keys.set(GHOST_kModifierKeyLeftShift, mod_shift_);
  keys.set(GHOST_kModifierKeyRightShift, false);
  keys.set(GHOST_kModifierKeyLeftControl, mod_ctrl_);
  keys.set(GHOST_kModifierKeyRightControl, false);
  keys.set(GHOST_kModifierKeyLeftAlt, mod_alt_);
  keys.set(GHOST_kModifierKeyRightAlt, false);
  keys.set(GHOST_kModifierKeyLeftOS, mod_os_);
  keys.set(GHOST_kModifierKeyRightOS, false);
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemWeb::getButtons(GHOST_Buttons &buttons) const
{
  buttons.set(GHOST_kButtonMaskLeft, btn_left_);
  buttons.set(GHOST_kButtonMaskMiddle, btn_middle_);
  buttons.set(GHOST_kButtonMaskRight, btn_right_);
  return GHOST_kSuccess;
}

GHOST_TCapabilityFlag GHOST_SystemWeb::getCapabilities() const
{
  return GHOST_TCapabilityFlag(
      GHOST_kCapabilityWindowPosition |
      GHOST_kCapabilityClipboardPrimary);
  /* Note: GHOST_kCapabilityCursorWarp intentionally omitted.
   * Browser cannot warp the mouse cursor. If reported as supported,
   * Blender warps cursor into popups/menus, but the next real mouse
   * event from the browser resets the position, causing menus to
   * instantly close. */
}

char *GHOST_SystemWeb::getClipboard(bool /*selection*/) const
{
  if (clipboard_.empty()) {
    return nullptr;
  }
  char *buf = (char *)malloc(clipboard_.size() + 1);
  memcpy(buf, clipboard_.c_str(), clipboard_.size() + 1);
  return buf;
}

void GHOST_SystemWeb::putClipboard(const char *buffer, bool /*selection*/) const
{
  clipboard_ = buffer ? buffer : "";
}

uint64_t GHOST_SystemWeb::getMilliSeconds() const
{
  auto now = std::chrono::steady_clock::now();
  uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count();
  return now_ms - start_time_ms_;
}

uint8_t GHOST_SystemWeb::getNumDisplays() const
{
  return 1;
}

GHOST_TSuccess GHOST_SystemWeb::getCursorPosition(int32_t &x, int32_t &y) const
{
  x = cursor_x_;
  y = cursor_y_;
  return GHOST_kSuccess;
}

GHOST_TSuccess GHOST_SystemWeb::setCursorPosition(int32_t x, int32_t y)
{
  cursor_x_ = x;
  cursor_y_ = y;
  return GHOST_kSuccess;
}

void GHOST_SystemWeb::getMainDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  width = display_width_;
  height = display_height_;
}

void GHOST_SystemWeb::getAllDisplayDimensions(uint32_t &width, uint32_t &height) const
{
  getMainDisplayDimensions(width, height);
}

GHOST_IContext *GHOST_SystemWeb::createOffscreenContext(GHOST_GPUSettings gpu_settings)
{
#ifdef WITH_VULKAN_BACKEND
  if (gpu_settings.context_type == GHOST_kDrawingContextTypeVulkan) {
    const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(
        gpu_settings);
    GHOST_Context *context = new GHOST_ContextVK(context_params,
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
                                                 gpu_settings.preferred_device);
    if (context->initializeDrawingContext()) {
      return context;
    }
    delete context;
    return nullptr;
  }
#endif

#if defined(WITH_OPENGL_BACKEND) && defined(_WIN32)
  if (gpu_settings.context_type == GHOST_kDrawingContextTypeOpenGL) {
    const GHOST_ContextParams context_params = GHOST_CONTEXT_PARAMS_FROM_GPU_SETTINGS_OFFSCREEN(
        gpu_settings);
    for (int minor = 6; minor >= 3; --minor) {
      GHOST_Context *context = new GHOST_ContextWGL(context_params,
                                                     false,
                                                     0,
                                                     0,
                                                     WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
                                                     4,
                                                     minor,
                                                     0,
                                                     GHOST_OPENGL_WGL_RESET_NOTIFICATION_STRATEGY);
      if (context->initializeDrawingContext()) {
        return context;
      }
      delete context;
    }
  }
#else
  (void)gpu_settings;
#endif
  return nullptr;
}

GHOST_TSuccess GHOST_SystemWeb::disposeContext(GHOST_IContext *context)
{
  delete context;
  return GHOST_kSuccess;
}

GHOST_IWindow *GHOST_SystemWeb::getWindowUnderCursor(int32_t /*x*/, int32_t /*y*/)
{
  if (active_window_) {
    return active_window_;
  }
  const std::vector<GHOST_IWindow *> &windows = window_manager_->getWindows();
  return windows.empty() ? nullptr : windows[0];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame streaming
 * \{ */

void GHOST_SystemWeb::sendFrame(const uint8_t *rgba_pixels, int width, int height)
{
  if (!running_) {
    return;
  }

  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (clients_.empty()) {
    return;
  }

#ifdef WITH_WEB_NVENC
  /* Try NVENC H.264 encoding first. */
  if (!encoder_init_attempted_) {
    encoder_init_attempted_ = true;
    auto nvenc = std::make_unique<GHOST_EncoderNVENC>();
    if (nvenc->init(width, height)) {
      encoder_ = std::move(nvenc);
      encoder_width_ = width;
      encoder_height_ = height;
      fprintf(stderr, "[WebServer] Using %s encoder\n", encoder_->name());
    }
    else {
      fprintf(stderr, "[WebServer] NVENC unavailable, falling back to JPEG\n");
    }
  }

  if (encoder_) {
    /* If resolution changed, don't reinit NVENC immediately (resize spams many
     * sizes). Instead, mark encoder as stale and fall back to JPEG. Reinit
     * only after the size has been stable for a few frames. */
    int aw = (width + 1) & ~1, ah = (height + 1) & ~1;
    if (aw != encoder_width_ || ah != encoder_height_) {
      if (aw != encoder_pending_width_ || ah != encoder_pending_height_) {
        /* New size — reset counter. */
        encoder_pending_width_ = aw;
        encoder_pending_height_ = ah;
        encoder_pending_frames_ = 0;
      }
      encoder_pending_frames_++;
      if (encoder_pending_frames_ >= 5) {
        /* Size has been stable for 5 frames — reinit encoder. */
        encoder_->shutdown();
        if (encoder_->init(width, height)) {
          encoder_width_ = aw;
          encoder_height_ = ah;
          encoder_->requestKeyframe();
        }
        else {
          encoder_.reset();
        }
        encoder_pending_width_ = 0;
        encoder_pending_height_ = 0;
      }
      /* Fall through to JPEG path during resize. */
    }
  }

  if (encoder_ && encoder_pending_width_ == 0) {
    std::vector<uint8_t> h264_data;
    bool is_keyframe = false;
    if (encoder_->encode(rgba_pixels, width, height, h264_data, is_keyframe)) {
      /* Send H.264 with 1-byte header: 0x01=keyframe, 0x02=delta. */
      std::vector<uint8_t> msg(1 + h264_data.size());
      msg[0] = is_keyframe ? 0x01 : 0x02;
      memcpy(msg.data() + 1, h264_data.data(), h264_data.size());


      std::vector<socket_t> dead;
      for (socket_t sock : clients_) {
        if (!wsSendFrame(sock, msg.data(), msg.size(), 0x02)) {
          dead.push_back(sock);
        }
      }
      for (socket_t s : dead) {
        close_socket(s);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
      }
      return;
    }
  }
#endif

  /* Fallback: JPEG encoding. */
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);

  unsigned char *jpeg_buf = nullptr;
  unsigned long jpeg_size = 0;
  jpeg_mem_dest(&cinfo, &jpeg_buf, &jpeg_size);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 4;
  cinfo.in_color_space = JCS_EXT_RGBA;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, 60, TRUE);
  cinfo.dct_method = JDCT_FASTEST;
  jpeg_start_compress(&cinfo, TRUE);

  while (cinfo.next_scanline < cinfo.image_height) {
    int src_y = height - 1 - cinfo.next_scanline;
    uint8_t *row_ptr = (uint8_t *)(rgba_pixels + src_y * width * 4);
    jpeg_write_scanlines(&cinfo, &row_ptr, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  {
    std::lock_guard<std::mutex> clock(capture_mutex_);
    capture_jpeg_.assign(jpeg_buf, jpeg_buf + jpeg_size);
  }

  /* JPEG frames: send with 0x00 header byte. */
  std::vector<uint8_t> msg(1 + jpeg_size);
  msg[0] = 0x00;  /* JPEG marker. */
  memcpy(msg.data() + 1, jpeg_buf, jpeg_size);
  free(jpeg_buf);

  std::vector<socket_t> dead;
  for (socket_t sock : clients_) {
    if (!wsSendFrame(sock, msg.data(), msg.size(), 0x02)) {
      dead.push_back(sock);
    }
  }
  for (socket_t s : dead) {
    close_socket(s);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
  }
}

#ifdef WITH_VULKAN_BACKEND

bool GHOST_SystemWeb::sendFrameExternal(GHOST_WindowWeb *window, const GHOST_VirtualFrame &frame)
{
#  if defined(WITH_WEB_NVENC) && defined(_WIN32)
  if (!running_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (clients_.empty()) {
    /* Nothing to send — frame handled, skip the CPU fallback as well. */
    return true;
  }

  /* First frame: attempt NVENC init (mirrors sendFrame). */
  if (!encoder_init_attempted_) {
    encoder_init_attempted_ = true;
    auto nvenc = std::make_unique<GHOST_EncoderNVENC>();
    if (nvenc->init(frame.width, frame.height)) {
      encoder_ = std::move(nvenc);
      encoder_width_ = (frame.width + 1) & ~1;
      encoder_height_ = (frame.height + 1) & ~1;
      fprintf(stderr, "[WebServer] Using %s encoder (zero-copy input)\n", encoder_->name());
    }
    else {
      fprintf(stderr, "[WebServer] NVENC unavailable, falling back to JPEG\n");
    }
  }
  if (!encoder_) {
    return false;
  }

  /* During a resize the encoder is stale: let the CPU path run — it owns the
   * "stable size" bookkeeping and the eventual encoder re-init. */
  const int aligned_w = (int(frame.width) + 1) & ~1;
  const int aligned_h = (int(frame.height) + 1) & ~1;
  if (aligned_w != encoder_width_ || aligned_h != encoder_height_ ||
      encoder_pending_width_ != 0) {
    return false;
  }

  /* (Re)register the exported buffers when the virtual swapchain was recreated. */
  if (external_generation_ != frame.generation || external_window_ != window) {
    encoder_->unregisterExternalBuffers();
    external_generation_ = 0;
    external_window_ = nullptr;

    const uint32_t count = window->vulkanSlotCount();
    std::vector<GHOST_EncoderExternalBuffer> buffers(count);
    for (uint32_t index = 0; index < count; index++) {
      GHOST_VirtualSlotExport export_info;
      if (!window->getVulkanSlotExport(index, export_info)) {
        return false; /* Export unavailable (no extension) — CPU path. */
      }
      buffers[index].win32_handle = export_info.win32_handle;
      buffers[index].size = export_info.size;
    }
    if (count == 0 || !encoder_->registerExternalBuffers(buffers.data(),
                                                         int(count),
                                                         encoder_width_,
                                                         encoder_height_,
                                                         int(frame.pitch)))
    {
      return false;
    }
    external_generation_ = frame.generation;
    external_window_ = window;
  }

  std::vector<uint8_t> h264_data;
  bool is_keyframe = false;
  if (!encoder_->encodeExternal(int(frame.slot), h264_data, is_keyframe)) {
    return false;
  }

  std::vector<uint8_t> msg(1 + h264_data.size());
  msg[0] = is_keyframe ? 0x01 : 0x02;
  memcpy(msg.data() + 1, h264_data.data(), h264_data.size());

  std::vector<socket_t> dead;
  for (socket_t sock : clients_) {
    if (!wsSendFrame(sock, msg.data(), msg.size(), 0x02)) {
      dead.push_back(sock);
    }
  }
  for (socket_t s : dead) {
    close_socket(s);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), s), clients_.end());
  }
  return true;
#  else
  (void)window;
  (void)frame;
  return false;
#  endif
}

void GHOST_SystemWeb::releaseExternalFrameResources(GHOST_WindowWeb *window)
{
  std::lock_guard<std::mutex> lock(clients_mutex_);
  if (external_window_ != window) {
    return;
  }
  if (encoder_) {
    encoder_->unregisterExternalBuffers();
  }
  external_generation_ = 0;
  external_window_ = nullptr;
}

#endif /* WITH_VULKAN_BACKEND */

/** \} */

/* -------------------------------------------------------------------- */
/** \name Embedded web client
 * \{ */

const char *GHOST_SystemWeb::getClientHTML()
{
  return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Blender Web Display</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body { background: #1d1d1d; overflow: hidden; display: flex; align-items: center; justify-content: center; height: 100vh; }
#viewport { image-rendering: pixelated; cursor: default; }
#status { position: fixed; top: 10px; right: 10px; color: #aaa; font: 12px monospace; background: rgba(0,0,0,0.7); padding: 6px 10px; border-radius: 4px; z-index: 10; }
#status.connected { color: #8f8; }
</style>
</head>
<body>
<div id="status">Connecting...</div>
<canvas id="viewport"></canvas>
<script src="/client.js"></script>
</body>
</html>
)HTML";
}

const char *GHOST_SystemWeb::getClientJS()
{
  return R"JS(
(function() {
  'use strict';

  const canvas = document.getElementById('viewport');
  const ctx = canvas.getContext('2d');
  const status = document.getElementById('status');

  let ws = null;
  let frameCount = 0;
  let lastFpsTime = Date.now();
  let fps = 0;
  let videoDecoder = null;
  let frameTimestamp = 0;
  let encoderMode = 'unknown'; /* 'h264' or 'jpeg' */

  function updateFps() {
    frameCount++;
    const now = Date.now();
    if (now - lastFpsTime >= 1000) {
      fps = frameCount;
      frameCount = 0;
      lastFpsTime = now;
      status.textContent = 'Connected | ' + fps + ' fps | ' + encoderMode.toUpperCase() + ' | ' + canvas.width + 'x' + canvas.height;
    }
  }

  function drawVideoFrame(frame) {
    if (canvas.width !== frame.displayWidth || canvas.height !== frame.displayHeight) {
      canvas.width = frame.displayWidth;
      canvas.height = frame.displayHeight;
    }
    ctx.drawImage(frame, 0, 0);
    frame.close();
    updateFps();
  }

  function initVideoDecoder() {
    if (typeof VideoDecoder === 'undefined') { status.textContent = 'No WebCodecs'; return null; }
    try {
      return new VideoDecoder({
        output: drawVideoFrame,
        error: function(e) { status.textContent = 'DecErr: ' + e.message; }
      });
    } catch(e) { status.textContent = 'DecInit: ' + e.message; return null; }
  }

  function connect() {
    const wsUrl = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
    ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';

    ws.onopen = function() {
      status.textContent = 'Connected';
      status.className = 'connected';
      encoderMode = 'unknown';
      videoDecoder = initVideoDecoder();
      sendResize();
    };

    ws.onclose = function() {
      status.textContent = 'Disconnected - reconnecting...';
      status.className = '';
      if (videoDecoder) { try { videoDecoder.close(); } catch(e) {} videoDecoder = null; }
      setTimeout(connect, 1000);
    };

    ws.onerror = function() { ws.close(); };

    ws.onmessage = function(evt) {
      if (!(evt.data instanceof ArrayBuffer) || evt.data.byteLength < 2) return;
      var bytes = new Uint8Array(evt.data);
      var type = bytes[0]; /* 0x00=JPEG, 0x01=H.264 keyframe, 0x02=H.264 delta */
      var payload = bytes.subarray(1);

      if (type === 0x01 || type === 0x02) {
        /* H.264 frame via WebCodecs VideoDecoder.
         * NVENC outputs Annex B (start codes). Convert to avcC (length-prefixed)
         * for Chrome WebCodecs which is more reliable with avcC + description. */
        encoderMode = 'h264';

        /* Parse NAL units from Annex B. */
        var nalus = [];
        var sps = null, pps = null;
        for (var j = 0; j < payload.byteLength - 3; ) {
          /* Find start code: 00 00 00 01 or 00 00 01 */
          var scLen = 0;
          if (j+3 < payload.byteLength && payload[j]===0 && payload[j+1]===0 && payload[j+2]===0 && payload[j+3]===1) scLen = 4;
          else if (payload[j]===0 && payload[j+1]===0 && payload[j+2]===1) scLen = 3;
          if (scLen === 0) { j++; continue; }
          j += scLen;
          /* Find end of this NAL (next start code or end of data). */
          var nalStart = j;
          while (j < payload.byteLength - 3) {
            if ((payload[j]===0 && payload[j+1]===0 && payload[j+2]===1) ||
                (payload[j]===0 && payload[j+1]===0 && payload[j+2]===0 && j+3<payload.byteLength && payload[j+3]===1)) break;
            j++;
          }
          if (j >= payload.byteLength - 3) j = payload.byteLength;
          var nalData = payload.subarray(nalStart, j);
          var nalType = nalData[0] & 0x1F;
          nalus.push(nalData);
          if (nalType === 7) sps = nalData;
          if (nalType === 8) pps = nalData;
        }

        if (!videoDecoder || videoDecoder.state === 'closed') {
          videoDecoder = initVideoDecoder();
        }
        /* Reconfigure on keyframes: always reconfigure if SPS changed (resolution change)
         * or if decoder is unconfigured. */
        var needsConfigure = false;
        if (videoDecoder && type === 0x01 && sps && pps) {
          if (videoDecoder.state === 'unconfigured' || videoDecoder.state === 'closed') {
            needsConfigure = true;
          } else if (videoDecoder.state === 'configured' && window._lastSpsHex) {
            var spsHex = Array.from(sps.slice(0,8)).join(',');
            if (spsHex !== window._lastSpsHex) {
              needsConfigure = true;
              videoDecoder.reset();
            }
          }
          if (sps) window._lastSpsHex = Array.from(sps.slice(0,8)).join(',');
        }
        if (needsConfigure) {
          if (videoDecoder.state === 'closed') videoDecoder = initVideoDecoder();
          if (videoDecoder) {
            try {
              /* Build avcC description from SPS + PPS. */
              var desc = new Uint8Array(11 + sps.byteLength + pps.byteLength);
              desc[0] = 1; /* version */
              desc[1] = sps[1]; /* profile */
              desc[2] = sps[2]; /* compat */
              desc[3] = sps[3]; /* level */
              desc[4] = 0xFF; /* lengthSizeMinusOne = 3 (4-byte lengths) */
              desc[5] = 0xE1; /* numSPS = 1 */
              desc[6] = (sps.byteLength >> 8) & 0xFF;
              desc[7] = sps.byteLength & 0xFF;
              desc.set(sps, 8);
              var off = 8 + sps.byteLength;
              desc[off] = 1; /* numPPS */
              desc[off+1] = (pps.byteLength >> 8) & 0xFF;
              desc[off+2] = pps.byteLength & 0xFF;
              desc.set(pps, off + 3);

              var codecStr = 'avc1.' + sps[1].toString(16).padStart(2,'0') + sps[2].toString(16).padStart(2,'0') + sps[3].toString(16).padStart(2,'0');

              videoDecoder.configure({
                codec: codecStr,
                description: desc.buffer,
                hardwareAcceleration: 'prefer-hardware',
                optimizeForLatency: true,
              });
              status.textContent = 'Configured: ' + codecStr;
            } catch(e) { status.textContent = 'CfgErr: ' + e.message; }
          }
        }
        if (videoDecoder && videoDecoder.state === 'configured' && nalus.length > 0) {
          try {
            /* Convert NALs to avcC format: 4-byte big-endian length + NAL data. */
            var totalLen = 0;
            for (var k = 0; k < nalus.length; k++) {
              var nt = nalus[k][0] & 0x1F;
              if (nt !== 7 && nt !== 8) totalLen += 4 + nalus[k].byteLength; /* skip SPS/PPS in data */
            }
            var avcC = new Uint8Array(totalLen);
            var pos = 0;
            for (var k = 0; k < nalus.length; k++) {
              var nt = nalus[k][0] & 0x1F;
              if (nt === 7 || nt === 8) continue; /* SPS/PPS in description, not in data */
              var len = nalus[k].byteLength;
              avcC[pos] = (len >> 24) & 0xFF;
              avcC[pos+1] = (len >> 16) & 0xFF;
              avcC[pos+2] = (len >> 8) & 0xFF;
              avcC[pos+3] = len & 0xFF;
              avcC.set(nalus[k], pos + 4);
              pos += 4 + len;
            }
            var chunk = new EncodedVideoChunk({
              type: type === 0x01 ? 'key' : 'delta',
              timestamp: frameTimestamp,
              data: avcC,
            });
            frameTimestamp += 16667;
            videoDecoder.decode(chunk);
            /* No flush() needed — NVENC configured with zeroReorderDelay + VUI
             * bitstreamRestrictionFlag, so decoder outputs each frame immediately. */
          } catch(e) { status.textContent = 'DecErr: ' + e.message; }
        }
      } else {
        /* JPEG fallback (type === 0x00 or legacy without header). */
        encoderMode = 'jpeg';
        var blob = new Blob([payload], {type: 'image/jpeg'});
        createImageBitmap(blob).then(function(bmp) {
          if (canvas.width !== bmp.width || canvas.height !== bmp.height) {
            canvas.width = bmp.width;
            canvas.height = bmp.height;
          }
          ctx.drawImage(bmp, 0, 0);
          bmp.close();
          updateFps();
        });
      }
    };
  }

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  function sendResize() {
    send({ type: 'resize', width: window.innerWidth, height: window.innerHeight });
  }

  /* --- Input event handlers --- */

  canvas.addEventListener('mousemove', function(e) {
    send({ type: 'mousemove', x: e.offsetX, y: e.offsetY });
  });

  canvas.addEventListener('mousedown', function(e) {
    e.preventDefault();
    send({ type: 'mousedown', button: e.button, x: e.offsetX, y: e.offsetY });
  });

  canvas.addEventListener('mouseup', function(e) {
    e.preventDefault();
    send({ type: 'mouseup', button: e.button, x: e.offsetX, y: e.offsetY });
  });

  canvas.addEventListener('wheel', function(e) {
    e.preventDefault();
    send({ type: 'wheel', deltaX: e.deltaX, deltaY: e.deltaY, x: e.offsetX, y: e.offsetY });
  }, { passive: false });

  canvas.addEventListener('contextmenu', function(e) {
    e.preventDefault();
  });

  document.addEventListener('keydown', function(e) {
    /* Allow browser refresh and dev tools. */
    if ((e.ctrlKey || e.metaKey) && (e.key === 'r' || e.key === 'R' || e.key === 'F12')) return;
    if (e.key === 'F12') return;
    e.preventDefault();
    send({
      type: 'keydown',
      key: e.key,
      code: e.code,
      repeat: e.repeat,
      shiftKey: e.shiftKey,
      ctrlKey: e.ctrlKey,
      altKey: e.altKey,
      metaKey: e.metaKey
    });
  });

  document.addEventListener('keyup', function(e) {
    e.preventDefault();
    send({
      type: 'keyup',
      key: e.key,
      code: e.code,
      repeat: false,
      shiftKey: e.shiftKey,
      ctrlKey: e.ctrlKey,
      altKey: e.altKey,
      metaKey: e.metaKey
    });
  });

  window.addEventListener('resize', function() {
    sendResize();
  });

  /* Prevent default drag behavior. */
  canvas.addEventListener('dragover', function(e) { e.preventDefault(); });
  canvas.addEventListener('drop', function(e) { e.preventDefault(); });

  /* --- Touch input (mobile/tablet) --- */
  var touchState = {
    startTime: 0,
    startX: 0,
    startY: 0,
    lastX: 0,
    lastY: 0,
    fingers: 0,
    pinchDist: 0,
    dragging: false,
    longPressTimer: null,
    isLongPress: false,
    orbitActive: false
  };

  function touchDist(t) {
    var dx = t[1].clientX - t[0].clientX;
    var dy = t[1].clientY - t[0].clientY;
    return Math.sqrt(dx * dx + dy * dy);
  }

  function touchMid(t, r) {
    return {
      x: ((t[0].clientX + t[1].clientX) / 2) - r.left,
      y: ((t[0].clientY + t[1].clientY) / 2) - r.top
    };
  }

  canvas.addEventListener('touchstart', function(e) {
    e.preventDefault();
    var r = canvas.getBoundingClientRect();
    var t = e.touches;
    touchState.fingers = t.length;
    touchState.startTime = Date.now();
    touchState.dragging = false;
    touchState.isLongPress = false;

    if (t.length === 1) {
      touchState.startX = t[0].clientX - r.left;
      touchState.startY = t[0].clientY - r.top;
      touchState.lastX = touchState.startX;
      touchState.lastY = touchState.startY;

      /* Long press = right click (700ms). */
      clearTimeout(touchState.longPressTimer);
      touchState.longPressTimer = setTimeout(function() {
        touchState.isLongPress = true;
        send({ type: 'mousemove', x: touchState.startX, y: touchState.startY });
        send({ type: 'mousedown', button: 2, x: touchState.startX, y: touchState.startY });
        send({ type: 'mouseup', button: 2, x: touchState.startX, y: touchState.startY });
      }, 700);
    }
    else if (t.length === 2) {
      clearTimeout(touchState.longPressTimer);
      touchState.pinchDist = touchDist(t);
      var m = touchMid(t, r);
      touchState.lastX = m.x;
      touchState.lastY = m.y;

      /* Start orbit (middle mouse down). */
      send({ type: 'mousemove', x: m.x, y: m.y });
      send({ type: 'mousedown', button: 1, x: m.x, y: m.y });
      touchState.orbitActive = true;
    }
  }, { passive: false });

  canvas.addEventListener('touchmove', function(e) {
    e.preventDefault();
    var r = canvas.getBoundingClientRect();
    var t = e.touches;

    if (t.length === 1 && !touchState.isLongPress) {
      var x = t[0].clientX - r.left;
      var y = t[0].clientY - r.top;
      var dx = x - touchState.startX;
      var dy = y - touchState.startY;

      /* Cancel long press if finger moved. */
      if (Math.abs(dx) > 10 || Math.abs(dy) > 10) {
        clearTimeout(touchState.longPressTimer);
        if (!touchState.dragging) {
          touchState.dragging = true;
          send({ type: 'mousedown', button: 0, x: touchState.startX, y: touchState.startY });
        }
      }

      send({ type: 'mousemove', x: x, y: y });
      touchState.lastX = x;
      touchState.lastY = y;
    }
    else if (t.length === 2) {
      clearTimeout(touchState.longPressTimer);
      var m = touchMid(t, r);

      /* Pinch zoom. */
      var dist = touchDist(t);
      var delta = dist - touchState.pinchDist;
      if (Math.abs(delta) > 5) {
        var ticks = delta > 0 ? 1 : -1;
        send({ type: 'wheel', deltaX: 0, deltaY: -ticks * 120, x: m.x, y: m.y });
        touchState.pinchDist = dist;
      }

      /* Two-finger drag = orbit. */
      send({ type: 'mousemove', x: m.x, y: m.y });
      touchState.lastX = m.x;
      touchState.lastY = m.y;
    }
  }, { passive: false });

  canvas.addEventListener('touchend', function(e) {
    e.preventDefault();
    clearTimeout(touchState.longPressTimer);

    if (touchState.orbitActive) {
      send({ type: 'mouseup', button: 1, x: touchState.lastX, y: touchState.lastY });
      touchState.orbitActive = false;
    }

    if (touchState.dragging) {
      send({ type: 'mouseup', button: 0, x: touchState.lastX, y: touchState.lastY });
      touchState.dragging = false;
    }

    /* Single tap = left click (if no drag and not long press). */
    if (e.touches.length === 0 && !touchState.isLongPress && !touchState.dragging) {
      var elapsed = Date.now() - touchState.startTime;
      if (elapsed < 300 && touchState.fingers === 1) {
        send({ type: 'mousemove', x: touchState.startX, y: touchState.startY });
        send({ type: 'mousedown', button: 0, x: touchState.startX, y: touchState.startY });
        send({ type: 'mouseup', button: 0, x: touchState.startX, y: touchState.startY });
      }
    }

    touchState.fingers = e.touches.length;
    touchState.isLongPress = false;
  }, { passive: false });

  canvas.addEventListener('touchcancel', function(e) {
    e.preventDefault();
    clearTimeout(touchState.longPressTimer);
    if (touchState.orbitActive) {
      send({ type: 'mouseup', button: 1, x: touchState.lastX, y: touchState.lastY });
      touchState.orbitActive = false;
    }
    if (touchState.dragging) {
      send({ type: 'mouseup', button: 0, x: touchState.lastX, y: touchState.lastY });
      touchState.dragging = false;
    }
  }, { passive: false });

  /* Start connection. */
  connect();
})();
)JS";
}

/** \} */

