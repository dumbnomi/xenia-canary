/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/debug/xbdm/xbdm_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/xex_module.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"


DEFINE_bool(xbdm_enable, false,
            "Run an XBDM (Xbox debug monitor) server on port 730.", "XBDM");
DEFINE_string(xbdm_bind, "127.0.0.1",
              "Address XBDM listens on. Use 0.0.0.0 for LAN discovery; the "
              "protocol is unauthenticated and allows memory writes.",
              "XBDM");
DEFINE_string(xbdm_name, "xenia", "Console name reported over XBDM.", "XBDM");

namespace xe {
namespace debug {
namespace xbdm {

constexpr uint16_t kXbdmPort = 730;
constexpr uint32_t kMaxGetMem = 1u << 20;  // per request
constexpr uintptr_t kInvalid = ~uintptr_t(0);

static SOCKET S(uintptr_t s) { return static_cast<SOCKET>(s); }

bool XbdmServer::Command::GetU32(const std::string& k, uint32_t* out) const {
  auto it = args.find(k);
  if (it == args.end() || it->second.empty()) return false;
  char* end = nullptr;
  unsigned long long v = std::strtoull(it->second.c_str(), &end, 0);
  if (!end || *end != '\0') return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

bool XbdmServer::Start() {
  if (running_) return true;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    XELOGE("XBDM: WSAStartup failed");
    return false;
  }
  in_addr bind_addr{};
  if (inet_pton(AF_INET, cvars::xbdm_bind.c_str(), &bind_addr) != 1) {
    XELOGE("XBDM: bad xbdm_bind '{}'", cvars::xbdm_bind);
    return false;
  }
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(kXbdmPort);
  sa.sin_addr = bind_addr;

  SOCKET t = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  BOOL yes = TRUE;
  setsockopt(t, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
             sizeof(yes));
  if (t == INVALID_SOCKET ||
      bind(t, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0 ||
      listen(t, 4) != 0) {
    XELOGE("XBDM: failed to listen on {}:{}", cvars::xbdm_bind, kXbdmPort);
    if (t != INVALID_SOCKET) closesocket(t);
    return false;
  }
  tcp_listen_ = static_cast<uintptr_t>(t);

  SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  setsockopt(u, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes),
             sizeof(yes));
  if (u != INVALID_SOCKET &&
      bind(u, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0) {
    udp_sock_ = static_cast<uintptr_t>(u);
  } else {
    XELOGW("XBDM: UDP discovery unavailable");
    if (u != INVALID_SOCKET) closesocket(u);
  }

  running_ = true;
  tcp_thread_ = std::thread([this] { TcpAcceptLoop(); });
  if (udp_sock_ != kInvalid) {
    udp_thread_ = std::thread([this] { UdpDiscoveryLoop(); });
  }
  XELOGI("XBDM: listening on {}:{}", cvars::xbdm_bind, kXbdmPort);
  return true;
}

void XbdmServer::Stop() {
  if (!running_.exchange(false)) return;
  if (tcp_listen_ != kInvalid) closesocket(S(tcp_listen_));
  if (udp_sock_ != kInvalid) closesocket(S(udp_sock_));
  if (tcp_thread_.joinable()) tcp_thread_.join();
  if (udp_thread_.joinable()) udp_thread_.join();
  tcp_listen_ = udp_sock_ = kInvalid;
}

void XbdmServer::TcpAcceptLoop() {
  while (running_) {
    SOCKET c = accept(S(tcp_listen_), nullptr, nullptr);
    if (c == INVALID_SOCKET) break;
    // Detached: client threads exit when their socket closes.
    std::thread([this, c] { ClientLoop(static_cast<uintptr_t>(c)); }).detach();
  }
}

// Neighborhood-style name lookup. Request: [type][len][name]; reply:
// [type=2][len][name]. TODO: verify framing against a real devkit capture.
void XbdmServer::UdpDiscoveryLoop() {
  uint8_t buf[256];
  while (running_) {
    sockaddr_in from{};
    int fl = sizeof(from);
    int n = recvfrom(S(udp_sock_), reinterpret_cast<char*>(buf), sizeof(buf), 0,
                     reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 2) {
      if (n < 0) break;
      continue;
    }
    const std::string& name = cvars::xbdm_name;
    std::string req(reinterpret_cast<char*>(buf) + 2,
                    std::min<size_t>(buf[1], static_cast<size_t>(n) - 2));
    if (buf[0] != 3) continue;
    if (!req.empty() && req != name) continue;  // empty = wildcard
    std::string reply;
    reply.push_back(2);
    reply.push_back(static_cast<char>(name.size()));
    reply += name;
    sendto(S(udp_sock_), reply.data(), static_cast<int>(reply.size()), 0,
           reinterpret_cast<sockaddr*>(&from), fl);
  }
}

void XbdmServer::ClientLoop(uintptr_t sock) {
  SendLine(sock, "201- connected");
  std::string pending;
  char chunk[1024];
  bool open = true;
  while (open && running_) {
    int n = recv(S(sock), chunk, sizeof(chunk), 0);
    if (n <= 0) break;
    pending.append(chunk, static_cast<size_t>(n));
    size_t pos;
    while (open && (pos = pending.find("\r\n")) != std::string::npos) {
      std::string line = pending.substr(0, pos);
      pending.erase(0, pos + 2);
      if (line.empty()) continue;
      Command cmd;
      if (!ParseLine(line, &cmd)) {
        open = SendLine(sock, "400- malformed command");
        continue;
      }
      open = Dispatch(sock, cmd);
    }
    if (pending.size() > 64 * 1024) break;  // runaway line
  }
  closesocket(S(sock));
}

bool XbdmServer::ParseLine(const std::string& line, Command* out) {
  size_t i = 0, n = line.size();
  auto skip_ws = [&] {
    while (i < n && line[i] == ' ') ++i;
  };
  skip_ws();
  size_t s = i;
  while (i < n && line[i] != ' ') ++i;
  if (i == s) return false;
  out->name = line.substr(s, i - s);
  std::transform(out->name.begin(), out->name.end(), out->name.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  while (true) {
    skip_ws();
    if (i >= n) break;
    size_t ks = i;
    while (i < n && line[i] != ' ' && line[i] != '=') ++i;
    std::string key = line.substr(ks, i - ks);
    std::string val;
    if (i < n && line[i] == '=') {
      ++i;
      if (i < n && line[i] == '"') {
        ++i;
        while (i < n && line[i] != '"') val.push_back(line[i++]);
        if (i < n) ++i;
      } else {
        while (i < n && line[i] != ' ') val.push_back(line[i++]);
      }
    }
    out->args[key] = val;  // bare flags get an empty value
  }
  return true;
}

bool XbdmServer::SendRaw(uintptr_t sock, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    int n = send(S(sock), s.data() + off, static_cast<int>(s.size() - off), 0);
    if (n <= 0) return false;
    off += static_cast<size_t>(n);
  }
  return true;
}

bool XbdmServer::SendMultiline(uintptr_t sock, const std::string& lines) {
  return SendLine(sock, "202- multiline response follows") &&
         SendRaw(sock, lines) && SendLine(sock, ".");
}

bool XbdmServer::Dispatch(uintptr_t sock, const Command& c) {
  const std::string& n = c.name;
  if (n == "bye") {
    SendLine(sock, "200- bye");
    return false;
  }
  if (n == "dmversion") return SendLine(sock, "200- 2.0.7645.2");
  if (n == "consoletype") return SendLine(sock, "200- devkit");
  if (n == "consolefeatures") return SendLine(sock, "200- ");
  if (n == "debugger" || n == "debugname" || n == "notifyat" ||
      n == "reboot" || n == "setsystime") {
    // Accepted so clients proceed; real behavior is a later phase.
    return SendLine(sock, "200- OK");
  }
  if (n == "systime") {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return SendLine(sock, fmt::format("200- hi=0x{:08x} lo=0x{:08x}",
                                      ft.dwHighDateTime, ft.dwLowDateTime));
  }
  if (n == "getmem") return CmdGetMem(sock, c);
  if (n == "setmem") return CmdSetMem(sock, c);
  if (n == "modules") return CmdModules(sock);
  if (n == "threads") return CmdThreads(sock);
  if (n == "stop" || n == "go" || n == "break" || n == "halt" ||
      n == "continue") {
    // Needs Processor suspend/resume exposed; see notes.
    return SendLine(sock, "400- not implemented");
  }
  return SendLine(sock, "407- unknown command");
}

static bool GuestPageReadable(Memory* mem, uint32_t addr) {
  auto* heap = mem->LookupHeap(addr);
  uint32_t prot = 0;
  return heap && heap->QueryProtect(addr, &prot) &&
         (prot & kMemoryProtectRead) != 0;
}

bool XbdmServer::CmdGetMem(uintptr_t sock, const Command& c) {
  uint32_t addr = 0, len = 0;
  if (!c.GetU32("addr", &addr) || !c.GetU32("length", &len)) {
    return SendLine(sock, "400- missing addr/length");
  }
  if (len > kMaxGetMem) return SendLine(sock, "400- length too large");
  Memory* mem = emulator_->memory();
  std::string out;
  out.reserve(len * 2 + len / 32 + 8);
  static const char* kHex = "0123456789abcdef";
  uint32_t col = 0;
  for (uint32_t i = 0; i < len; ++i) {
    uint32_t a = addr + i;
    if (GuestPageReadable(mem, a)) {
      uint8_t b = *mem->TranslateVirtual<uint8_t*>(a);
      out.push_back(kHex[b >> 4]);
      out.push_back(kHex[b & 15]);
    } else {
      out += "??";
    }
    if (++col == 32 || i + 1 == len) {
      out += "\r\n";
      col = 0;
    }
  }
  return SendMultiline(sock, out);
}

bool XbdmServer::CmdSetMem(uintptr_t sock, const Command& c) {
  uint32_t addr = 0;
  auto it = c.args.find("data");
  if (!c.GetU32("addr", &addr) || it == c.args.end() ||
      it->second.size() % 2 != 0) {
    return SendLine(sock, "400- bad addr/data");
  }
  auto nib = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  const std::string& hex = it->second;
  Memory* mem = emulator_->memory();
  size_t count = hex.size() / 2;
  // Validate everything first so a bad request writes nothing.
  for (size_t i = 0; i < count; ++i) {
    if (nib(hex[2 * i]) < 0 || nib(hex[2 * i + 1]) < 0)
      return SendLine(sock, "400- bad hex");
    if (!GuestPageReadable(mem, addr + static_cast<uint32_t>(i)))
      return SendLine(sock, "404- unmapped");
  }
  for (size_t i = 0; i < count; ++i) {
    *mem->TranslateVirtual<uint8_t*>(addr + static_cast<uint32_t>(i)) =
        static_cast<uint8_t>(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
  }
  // TODO: invalidate JIT code for [addr, addr+count) when patching code.
  return SendLine(sock, fmt::format("200- set {} bytes", count));
}

bool XbdmServer::CmdModules(uintptr_t sock) {
  std::string out;
  for (auto* m : emulator_->processor()->GetModules()) {
    auto* xex = dynamic_cast<cpu::XexModule*>(m);
    if (!xex || !xex->base_address()) continue;
    out += fmt::format(
        "name=\"{}\" base=0x{:08x} size=0x{:08x} check=0x0 timestamp=0x0 "
        "pdata=0x0 psize=0x0 thread=0x0 osize=0x0\r\n",
        xex->name(), xex->base_address(), xex->image_size());
  }
  return SendMultiline(sock, out);
}

bool XbdmServer::CmdThreads(uintptr_t sock) {
  std::string out;
  for (auto* t : emulator_->processor()->QueryThreadDebugInfos()) {
    if (t->state == cpu::ThreadDebugInfo::State::kExited ||
        t->state == cpu::ThreadDebugInfo::State::kZombie)
      continue;
    out += fmt::format("{}\r\n", t->thread_id);
  }
  return SendMultiline(sock, out);
}

}  // namespace xbdm
}  // namespace debug
}  // namespace xe
