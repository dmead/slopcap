// Any Sky-Watcher mount through Sky-Watcher's SynScan app (phone, tablet or PC), which serves its "SynScan App
// Protocol" on UDP port 11881: one ASCOM ITelescopeV3 call per datagram, as text ("SlewToCoordinatesAsync,12.45,45.89"
// answered "Ok,SlewToCoordinatesAsync"). From Sky-Watcher's "SynScan App Protocol" (2025-09-30). The app does the
// pointing model, so this is the easiest way to a Wi-Fi AZ-GTi or a mount the app is already aligned on. UDP only:
// over TCP the protocol has no way to tell where a reply ends.
#pragma once

#include "mounts/command_channel.hpp"
#include "mounts/mount_driver.hpp"

namespace slopcap::mounts {

class SynScanAppDriver : public IMountDriver {
public:
    explicit SynScanAppDriver(TransportFactory open);
    MountInfo connect() override;
    void disconnect() override { ch_.reset(); }
    MountStatus poll() override;
    void slewTo(double ra, double dec) override;
    void abort() override;
    void move(Dir d, int rate) override;
    void stop(Dir d) override;
    void pulse(Dir d, int ms) override;
    void setTracking(bool on) override;
    void setTrackRate(TrackRate r) override;
    void park() override;
    void unpark() override;
    void sync(double ra, double dec) override;

private:
    /// "Name,args" -> the reply's values after "Ok,Name"; throws MountError with the app's status otherwise.
    std::vector<std::string> call(const std::string& name, const std::string& args = {});
    std::optional<std::vector<std::string>> tryCall(const std::string& name);   ///< nullopt: not supported
    bool flag(const std::string& name);
    TransportFactory open_;
    std::unique_ptr<CommandChannel> ch_;
    Caps caps_;
};

}  // namespace slopcap::mounts
