/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_DEBUG_XBDM_XBDM_SERVER_H_
#define XENIA_DEBUG_XBDM_XBDM_SERVER_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <thread>

namespace xe {
class Emulator;
namespace debug {
namespace xbdm {

// Host-side implementation of the XBDM wire protocol (TCP/UDP 730) so that
// Xbox 360 dev tools can talk to the emulator as if it were a devkit.
class XbdmServer {
 public:
  explicit XbdmServer(Emulator* emulator) : emulator_(emulator) {}
  ~XbdmServer() { Stop(); }

  bool Start();
  void Stop();

 private:
  struct Command {
    std::string name;
    std::map<std::string, std::string> args;  // values with quotes stripped
    bool Has(const std::string& k) const { return args.count(k) != 0; }
    bool GetU32(const std::string& k, uint32_t* out) const;
  };

  void TcpAcceptLoop();
  void UdpDiscoveryLoop();
  void ClientLoop(uintptr_t sock);
  static bool ParseLine(const std::string& line, Command* out);

  // Returns false when the connection should close.
  bool Dispatch(uintptr_t sock, const Command& cmd);

  bool CmdGetMem(uintptr_t sock, const Command& cmd);
  bool CmdSetMem(uintptr_t sock, const Command& cmd);
  bool CmdModules(uintptr_t sock);
  bool CmdThreads(uintptr_t sock);

  static bool SendRaw(uintptr_t sock, const std::string& s);
  static bool SendLine(uintptr_t sock, const std::string& s) {
    return SendRaw(sock, s + "\r\n");
  }
  static bool SendMultiline(uintptr_t sock, const std::string& body_lines);

  Emulator* emulator_;
  std::atomic<bool> running_{false};
  uintptr_t tcp_listen_ = ~uintptr_t(0);
  uintptr_t udp_sock_ = ~uintptr_t(0);
  std::thread tcp_thread_;
  std::thread udp_thread_;
};

}  // namespace xbdm
}  // namespace debug
}  // namespace xe

#endif  // XENIA_DEBUG_XBDM_XBDM_SERVER_H_
