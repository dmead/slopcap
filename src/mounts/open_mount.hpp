// Built-in mount specs (docs/design/mounts.md §3.3), as for cameras: one string names a mount, in Settings and on the
// command line (--mount SPEC).
//   sim                         a simulated mount
//   lx200:PORT                  Meade LX200 / Autostar / LX200GPS or OnStep, told apart by asking (lx200-classic:,
//                               lx200-meade:, onstep: to say which)
//   onstep:PORT                 OnStep / OnStepX (USB, Bluetooth, or its Wi-Fi: tcp://HOST, port 9999 by default)
//   nexstar:PORT                a Celestron through its hand controller
//   synscan:PORT                a Sky-Watcher through its SynScan hand controller
//   skywatcher:PORT             a Sky-Watcher's motor controller itself: USB on the mount, an EQDIR cable, or its
//                               Wi-Fi as udp://HOST (192.168.4.1 when it's the access point; port 11880)
//   synscan-app[:udp://HOST]    the SynScan app, on this computer by default (port 11881)
// PORT is a serial port (COM3, /dev/ttyUSB0, /dev/cu.usbserial-…, with @BAUD to set the speed) or tcp:// / udp://.
// Anything else (empty, indi…) is INDI's.
#pragma once

#include "mounts/mount_driver.hpp"

#include <string>

namespace slopcap::mounts {

bool isBuiltInMount(const std::string& spec);

/// Makes the spec's driver (called on the mount's thread). Throws MountError for a spec it can't read.
std::function<std::unique_ptr<IMountDriver>()> mountFactory(const std::string& spec, const std::optional<Site>& site);

/// The specs above and this computer's serial ports, for --list-mounts.
std::string mountSpecHelp();

}  // namespace slopcap::mounts
