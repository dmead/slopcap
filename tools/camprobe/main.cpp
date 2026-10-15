// slopcap-camprobe: list every connected ZWO / QHY camera with its formats, controls, readout modes and
// gain landmarks (unity, lowest read noise / HCG, highest dynamic range). Read-only.
//   slopcap-camprobe            human-readable report
//   slopcap-camprobe --json     machine-readable
#include "report.hpp"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <vector>

namespace camprobe {
#ifdef CAMPROBE_ASI
ProbeResult probeAsi();
#endif
#ifdef CAMPROBE_QHY
ProbeResult probeQhy();
#endif
}  // namespace camprobe

int main(int argc, char** argv) {
    bool json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
    if (argc > 1 && !json) {
        std::cerr << "usage: slopcap-camprobe [--json]\n";
        return 2;
    }
    // The vendor SDKs print debug chatter to stdout; send it to stderr while probing so --json stays parseable.
    std::fflush(stdout);
    std::cout.flush();
    int savedStdout = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    std::vector<camprobe::ProbeResult> results;
#ifdef CAMPROBE_ASI
    results.push_back(camprobe::probeAsi());
#else
    results.push_back({"ZWO ASI", false, "", {}, {"built without the ZWO SDK"}});
#endif
#ifdef CAMPROBE_QHY
    results.push_back(camprobe::probeQhy());
#else
    results.push_back({"QHY", false, "", {}, {"built without the QHY SDK"}});
#endif
    std::fflush(stdout);
    std::cout.flush();
    dup2(savedStdout, STDOUT_FILENO);
    close(savedStdout);

    if (json) std::cout << camprobe::toJson(results) << "\n";
    else for (const auto& r : results) camprobe::printText(r);
    size_t cams = 0;
    for (const auto& r : results) cams += r.cameras.size();
    return cams ? 0 : 1;
}
