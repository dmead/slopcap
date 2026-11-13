// slopcap-sonyprobe's checks on one camera, apart from its command line (main.cpp) so the tests can run them against
// the fake camera. Each check is timed and recorded, and carries on past what fails where it can.
#pragma once

#include "camera/ptp.hpp"

#include <ostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sonyprobe {

/// A JSON value, built up as the probe goes.
class Json {
public:
    Json() = default;
    Json(bool v) : kind_(Kind::Bool), b_(v) {}
    template <class T>
        requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    Json(T v) : kind_(Kind::Number), n_(static_cast<double>(v)) {}
    Json(const char* s) : kind_(Kind::String), s_(s) {}
    Json(std::string s) : kind_(Kind::String), s_(std::move(s)) {}
    static Json array();
    static Json object();
    Json& operator[](const std::string& key);   ///< an object's member, added on first use
    void push(Json v);                          ///< onto an array
    bool isTrue() const { return kind_ == Kind::Bool && b_; }
    std::string dump(int indent = 0) const;

private:
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind_ = Kind::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> a_;
    std::vector<std::pair<std::string, Json>> o_;
};

std::string hex4(uint32_t v);   ///< "0x9201"

struct Options {
    bool shot = true;         ///< take pictures
    int shots = 1;
    bool setTest = true;      ///< change the shutter speed and ISO one notch and back
    int liveFrames = 10;      ///< live view frames to time
    bool keepSerial = false;  ///< the camera's serial number in the report
};

/// A file to write next to the report: its name's ending ("shot1.jpg") and contents.
struct File {
    std::string suffix;
    std::vector<uint8_t> data;
};

/// Every check on the camera behind `t`: progress lines to `log` as they happen, files (pictures) into `files`.
/// Returns the report's "camera" object.
Json run(slopcap::ptp::Transport& t, const Options& o, std::ostream& log, std::vector<File>& files);

}  // namespace sonyprobe
