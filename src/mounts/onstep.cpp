#include "mounts/onstep.hpp"

#include "mounts/lx200.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <thread>

namespace slopcap::mounts {

namespace {

constexpr int kReplyMs = 1500;
constexpr double kSiderealSecond = 0.99726957;   // SI seconds

// as the other drivers name speeds: multiples of sidereal with "×"
const char* const kRateNames[OnStepDriver::kRates] = {"0.25\xC3\x97", "0.5\xC3\x97", "1\xC3\x97",  "2\xC3\x97",  "4\xC3\x97",
                                                      "8\xC3\x97",    "20\xC3\x97",  "48\xC3\x97", "Half max", "Max"};
const char* const kRateText[] = {"0.25\xC3\x97", "0.5\xC3\x97", "1\xC3\x97", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97",
                                 "20\xC3\x97",   "48\xC3\x97",  "half max", "max",       "custom"};

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    s.erase(0, s.find_first_not_of(' '));
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool printableText(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= 32 && c < 127; });
}

/// "12.5" -> 12.5, only when the whole text is the number
std::optional<double> number(std::string_view s) {
    const std::string t(s);
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (t.empty() || end != t.c_str() + t.size()) return std::nullopt;
    return v;
}

/// "sDDD*MM", "sDD*MM:SS", "-10*" -> degrees (up to 360, unlike parseLx200Dec)
std::optional<double> degrees(std::string_view s) {
    bool neg = false, any = false;
    double v[3] = {0, 0, 0};
    int i = 0;
    std::string tok;
    auto flush = [&] {
        if (!tok.empty() && i < 3) v[i++] = std::atof(tok.c_str()), any = true;
        tok.clear();
    };
    for (const char c : s) {
        if ((c >= '0' && c <= '9') || c == '.') tok += c;
        else {
            if (c == '-' && !any && tok.empty()) neg = true;
            flush();
        }
    }
    flush();
    if (!any) return std::nullopt;
    const double d = v[0] + v[1] / 60 + v[2] / 3600;
    return neg ? -d : d;
}

/// Days from 1970-01-01 to a date (proleptic Gregorian; Howard Hinnant's days_from_civil).
long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

std::string dms(double deg, char pos, char neg) {
    const long s = std::lround(std::abs(deg) * 60);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%ld\xC2\xB0%02ld'%c", s / 60, s % 60, deg < 0 ? neg : pos);
    return buf;
}

std::string busyHint(const std::string& why) {
    const auto l = lower(why);
    if (l.find("denied") == std::string::npos && l.find("busy") == std::string::npos) return why;
    return why + ": another program has the port open (the OnStep ASCOM driver, for NINA or PHD2?). Disconnect the mount "
                 "there, or reach OnStep over its Wi-Fi (onstep:tcp://HOST)";
}

}  // namespace

std::optional<OnStepStatus> parseOnStepStatus(std::string_view gu) {
    if (gu.empty()) return std::nullopt;
    OnStepStatus s;
    std::string_view flags = gu;
    // OnStepX ends :GU# with the pier side (o, T or W), then the pulse-guide rate, the guide rate and the error code,
    // each as '0' + n (so 10, the custom rate, is ':')
    const auto n = gu.size();
    auto code = [](char c) { return c >= '0' && c <= '0' + 15 ? c - '0' : -1; };
    if (n >= 4 && std::string_view("oTW").find(gu[n - 4]) != std::string_view::npos && code(gu[n - 3]) >= 0 &&
        code(gu[n - 2]) >= 0 && code(gu[n - 1]) >= 0) {
        const char p = gu[n - 4];
        s.pier = p == 'T' ? PierSide::East : p == 'W' ? PierSide::West : PierSide::Unknown;
        s.pulseRate = code(gu[n - 3]);
        s.guideRate = code(gu[n - 2]);
        s.error = code(gu[n - 1]);
        flags = gu.substr(0, n - 4);
    }
    s.tracking = flags.find('n') == std::string_view::npos;
    s.slewing = flags.find('N') == std::string_view::npos;
    for (const char c : flags) {
        switch (c) {
            case 'p': case 'I': case 'P': case 'F': s.park = c; break;
            case 'H': s.atHome = true; break;
            case 'h': s.homing = true; break;
            case 'G': s.pulseGuiding = true; break;
            case 'g': s.guiding = true; break;
            case 'E': case 'K': case 'A': case 'L': s.mountType = c; break;
            case '(': s.rate = TrackRate::Lunar; break;
            case 'O': s.rate = TrackRate::Solar; break;
            case 'k': s.rate = TrackRate::King; break;
            case 'r': case 't': s.compensation = c; break;
            case 's': s.singleAxis = true; break;
            case 'z': s.buzzer = true; break;
            case 'a': s.autoFlip = true; break;
            case 'u': s.pauseAtHome = true; break;
            case 'v': s.visitHome = true; break;
            default: break;
        }
    }
    return s;
}

std::string onStepFault(int code) {
    // Limits.cpp, Limits::errorCode()
    static const char* const kFaults[] = {"",
                                          "a motor fault",
                                          "below the horizon limit",
                                          "a limit switch was hit",
                                          "at the Dec limit",
                                          "at the azimuth limit",
                                          "at the under-the-pole limit",
                                          "at the meridian limit",
                                          "a sync error",
                                          "a park error",
                                          "a goto-sync error",
                                          "an unspecified error",
                                          "above the overhead limit",
                                          "the weather sensor didn't start",
                                          "the site and time weren't set at start-up",
                                          "its settings memory didn't start"};
    if (code <= 0) return {};
    if (code < static_cast<int>(std::size(kFaults))) return kFaults[code];
    return "error " + std::to_string(code);
}

std::string onStepCommandError(int code) {
    // CommandErrors.h
    static const char* const kErrors[] = {"no error",
                                          "no error",
                                          "it doesn't know the command",
                                          "an unexpected reply",
                                          "a value out of range",
                                          "a value in the wrong form",
                                          "the alignment failed",
                                          "no alignment is under way",
                                          "it isn't parked or at home",
                                          "it is parked",
                                          "the park failed",
                                          "it isn't parked",
                                          "no park position is set",
                                          "the goto failed",
                                          "its object library is full",
                                          "below the horizon limit",
                                          "above the overhead limit",
                                          "the controller is in standby",
                                          "it is parked",
                                          "a goto is under way",
                                          "outside the mount's limits",
                                          "a hardware fault",
                                          "it is already moving",
                                          "an unspecified error"};
    if (code >= 0 && code < static_cast<int>(std::size(kErrors))) return kErrors[code];
    return "error " + std::to_string(code);
}

OnStepDriver::OnStepDriver(std::vector<TransportFactory> opens, std::optional<Site> site, SystemClock now)
    : opens_(std::move(opens)), site_(site), now_(std::move(now)) {
    if (!now_) now_ = [] { return std::chrono::system_clock::now(); };
}

CommandChannel& OnStepDriver::ch() {
    if (!ch_) throw MountError("the mount isn't connected");
    return *ch_;
}

bool OnStepDriver::numeric(std::string_view cmd, int ms) {
    // OnStepX's 0 / 1 replies come without a '#'
    const auto r = ch().tryCount(cmd, 1, ms);
    if (!r) throw MountError("the mount didn't answer " + printable(cmd) + " (" + ch().transport().name() + ")");
    return *r == "1";
}

std::string OnStepDriver::lastError() {
    if (const auto e = ch().tryUntil(":GE#", '#', 800))
        if (const auto c = number(trim(*e))) return onStepCommandError(static_cast<int>(*c));
    return "it didn't say why";
}

MountInfo OnStepDriver::connect() {
    ch_.reset();
    std::vector<std::string> tried;
    std::string product;
    for (const auto& open : opens_) {
        std::unique_ptr<ITransport> t;
        try {
            t = open();
        } catch (const MountError& e) {
            throw MountError(busyHint(e.what()));
        }
        tried.push_back(t->name());
        auto c = std::make_unique<CommandChannel>(std::move(t), "onstep");
        const auto p = c->tryUntil(":GVP#", '#', 600, true);
        if (!p || !printableText(*p)) continue;   // nothing, or noise: not this speed
        product = trim(*p);
        const auto l = lower(product);
        if (l.find("on-step") == std::string::npos && l.find("onstep") == std::string::npos)
            throw MountError(tried.back() + " answers as \"" + product + "\", not OnStep (lx200:PORT for a Meade)");
        ch_ = std::move(c);
        break;
    }
    if (!ch_) {
        std::string where;
        for (const auto& t : tried) where += (where.empty() ? "" : ", ") + t;
        throw MountError("no OnStep answers on " + (where.empty() ? std::string("the link") : where));
    }
    if (const auto v = ch_->tryUntil(":GVN#", '#', 800)) firmware_ = trim(*v);
    const int major = std::atoi(firmware_.c_str());

    // precision: HH:MM:SS.SSSS where the firmware has it (:GRH#; an older one answers a bare 0), else at least
    // HH:MM:SS (:U# toggles, so only when it talks short)
    const auto h = ch_->tryUntil(":GRH#", '#', 500);
    highPrecision_ = h && std::count(h->begin(), h->end(), ':') == 2 && parseLx200Ra(*h);
    if (!highPrecision_ && ch_->until(":GR#", '#', kReplyMs, true).find('.') != std::string::npos) {
        ch_->send(":U#");
        ch_->until(":GR#", '#', kReplyMs, true);
    }
    moveRate_ = -1;
    moving_[0] = moving_[1] = ratesChanged_ = false;
    trackRate_ = TrackRate::Sidereal;

    MountInfo info;
    info.backend = "onstep";
    info.model = (major >= 10 ? "OnStepX " : "OnStep ") + (firmware_.empty() ? product : firmware_);
    for (const char* r : kRateNames) info.rates.emplace_back(r);
    info.caps.pulse = info.caps.park = info.caps.unpark = info.caps.tracking = info.caps.trackRates = info.caps.sync = true;
    info.caps.settings = true;
    if (const auto tr = ch_->tryUntil(":GXTR#", '#', 500)) info.caps.rateOffsets = number(trim(*tr)).has_value();

    std::string details = "on " + ch_->transport().name();
    if (const auto u = ch_->tryUntil(":GU#", '#', kReplyMs, true)) {
        if (const auto s = parseOnStepStatus(*u)) {
            pulseRate_ = s->pulseRate, guideRate_ = s->guideRate;
            if (s->rate) trackRate_ = *s->rate;
            mountType_ = s->mountType;
            details += s->mountType == 'E' ? ", German equatorial" : s->mountType == 'K' ? ", fork" : s->mountType == 'A' ? ", alt-az" : "";
            if (s->pulseRate >= 0 && s->pulseRate <= 10) details += std::string(", pulse guides at ") + kRateText[s->pulseRate];
        }
    }
    if (const auto g = ch_->tryUntil(":Gh#", '#', 800)) details += ", horizon limit " + trim(*g);
    if (const auto o = ch_->tryUntil(":Go#", '#', 800)) details += ", overhead limit " + trim(*o);
    const auto e = ch_->tryUntil(":GXE9#", '#', 800), w = ch_->tryUntil(":GXEA#", '#', 800);
    if (e && w) details += ", meridian limits " + trim(*e) + " min east, " + trim(*w) + " min west";
    details += highPrecision_ ? ", high precision" : "";
    details += info.caps.rateOffsets ? ", rate offsets" : "";
    info.details = details;
    checkClock(info);
    checkSite(info);
    return info;
}

void OnStepDriver::checkClock(MountInfo& info) {
    const auto date = ch().tryUntil(":GC#", '#', 800), time = ch().tryUntil(":GL#", '#', 800), off = ch().tryUntil(":GG#", '#', 800);
    if (!date || !time || !off) return;
    int mo = 0, d = 0, y = 0, hh = 0, mm = 0, ss = 0, oh = 0, om = 0;
    char sign = '+';
    if (std::sscanf(date->c_str(), "%d/%d/%d", &mo, &d, &y) != 3 || std::sscanf(time->c_str(), "%d:%d:%d", &hh, &mm, &ss) != 3 ||
        std::sscanf(off->c_str(), " %c%d:%d", &sign, &oh, &om) < 2 || mo < 1 || mo > 12)
        return;
    if (y < 100) y += 2000;
    const double offsetH = (sign == '-' ? -1 : 1) * (oh + om / 60.0);   // hours to add to local time for UTC
    const double mountUtc = daysFromCivil(y, mo, d) * 86400.0 + hh * 3600 + mm * 60 + ss + offsetH * 3600;
    const double pc = std::chrono::duration<double>(now_().time_since_epoch()).count();
    const double diff = mountUtc - pc;
    if (std::abs(diff) < 90) return;
    const std::string way = diff > 0 ? "ahead of" : "behind";
    if (std::abs(std::abs(diff) - 3600) < 300)
        info.notes.push_back("The mount's clock is an hour " + way + " this computer's. Its UTC offset is " + trim(*off) +
                             ": daylight saving? Gotos land about 15 degrees off until it's put right (Mount > Settings and log: Set date, time and UTC offset from this computer)");
    else {
        const long s = std::lround(std::abs(diff));
        char buf[64];
        if (s < 3600) std::snprintf(buf, sizeof buf, "%ld min %ld s", s / 60, s % 60);
        else std::snprintf(buf, sizeof buf, "%.1f hours", s / 3600.0);
        info.notes.push_back(std::string("The mount's clock is ") + buf + " " + way + " this computer's: gotos miss by about 15\" for each second");
    }
}

void OnStepDriver::checkSite(MountInfo& info) {
    if (!site_) return;
    const auto lat = ch().tryUntil(":Gt#", '#', 800), lon = ch().tryUntil(":Gg#", '#', 800);
    if (!lat || !lon) return;
    const auto la = degrees(*lat), lo = degrees(*lon);
    if (!la || !lo) return;
    const double east = -*lo;   // OnStep's longitude is west positive
    const double dLon = std::remainder(east - site_->longitude, 360.0) * std::cos(*la * 3.14159265358979 / 180);
    if (std::abs(*la - site_->latitude) < 0.1 && std::abs(dLon) < 0.1) return;
    info.notes.push_back("The mount's site (" + dms(*la, 'N', 'S') + " " + dms(east, 'E', 'W') + ") isn't the one in Settings (" +
                         dms(site_->latitude, 'N', 'S') + " " + dms(site_->longitude, 'E', 'W') +
                         "): the mount's own is what its gotos and limits use");
}

MountStatus OnStepDriver::poll() {
    MountStatus st;
    const auto ra = parseLx200Ra(ch().until(highPrecision_ ? ":GRH#" : ":GR#", '#', kReplyMs, true));
    const auto dec = parseLx200Dec(ch().until(highPrecision_ ? ":GDH#" : ":GD#", '#', kReplyMs, true));
    st.haveCoords = ra && dec;
    st.ra = ra.value_or(0), st.dec = dec.value_or(0);
    if (const auto s = parseOnStepStatus(ch().until(":GU#", '#', kReplyMs, true))) {
        st.tracking = s->tracking;
        st.slewing = s->slewing || s->homing || s->park == 'I';
        st.parked = s->park == 'P';
        st.pier = s->pier;
        st.atHome = s->atHome;
        st.fault = s->park == 'F' ? "the last park failed" : onStepFault(s->error);
        // learn the mount's own rates (PHD2's pulses go at them), except while moves of ours have changed them
        if (!ratesChanged_) pulseRate_ = s->pulseRate, guideRate_ = s->guideRate;
    }
    return st;
}

void OnStepDriver::setTarget(double ra, double dec) {
    if (!numeric(":Sr" + lx200RaText(ra) + "#")) throw MountError("the mount refused the target's RA");
    if (!numeric(":Sd" + lx200DecText(dec) + "#")) throw MountError("the mount refused the target's Dec");
}

void OnStepDriver::slewTo(double ra, double dec) {
    setTarget(ra, dec);
    // a move that has just ended still slows down, and OnStepX refuses a goto until it has (8, "already moving"):
    // ask again for a few seconds. (A refused goto isn't a goto, so asking again can't start two.)
    for (int tries = 0;; ++tries) {
        const auto r = ch().count(":MS#", 1, 3000);   // a digit, no '#'
        if (r == "0") return;
        if (r != "8" || tries >= kGotoRetries)
            throw MountError(std::string("the mount refused the goto: ") + onStepGotoError(r[0]));
        std::this_thread::sleep_for(std::chrono::milliseconds(kGotoRetryMs));
    }
}

void OnStepDriver::abort() {
    ch().send(":Q#");
    moving_[0] = moving_[1] = false;
    restoreRates();
}

void OnStepDriver::move(Dir d, int rate) {
    rate = std::clamp(rate, 0, kRates - 1);
    if (rate != moveRate_) ch().send(":R" + std::to_string(rate) + "#");
    moveRate_ = rate;
    ratesChanged_ = true;   // put the mount's own rates back when the moves end
    ch().send(std::string(":M") + "nsew"[static_cast<int>(d)] + "#");
    moving_[isNorthSouth(d) ? 0 : 1] = true;
}

void OnStepDriver::stop(Dir d) {
    ch().send(std::string(":Q") + "nsew"[static_cast<int>(d)] + "#");
    moving_[isNorthSouth(d) ? 0 : 1] = false;
    if (!moving_[0] && !moving_[1]) restoreRates();
}

void OnStepDriver::restoreRates() {
    // :R# sets the rate guide pulses go at too: on some builds the guide rate itself, on others (with
    // GUIDE_SEPARATE_PULSE_RATE) a pulse-guide rate that :R0# to :R2# also overwrite. Either way PHD2's pulses would
    // change speed, so the pulse-guide rate goes back first, then the guide rate where :R# can say it (not custom).
    if (!ratesChanged_) return;
    ratesChanged_ = false;
    moveRate_ = -1;
    if (pulseRate_ >= 0 && pulseRate_ <= 2) ch().send(":R" + std::to_string(pulseRate_) + "#");
    if (guideRate_ > 2 && guideRate_ <= 9) ch().send(":R" + std::to_string(guideRate_) + "#");
}

void OnStepDriver::pulse(Dir d, int ms) {
    char buf[40];
    std::snprintf(buf, sizeof buf, ":Mg%c%d#", "nsew"[static_cast<int>(d)], std::clamp(ms, 1, 30000));
    ch().send(buf);
}

void OnStepDriver::setTracking(bool on) {
    if (!on) {
        if (!numeric(":Td#")) throw MountError("the mount couldn't stop tracking: " + lastError());
        return;
    }
    if (!numeric(":Te#")) throw MountError("the mount couldn't start tracking: " + lastError());
    // :Te# says 1 even when the tracking rate is 0 (after :ERESET#): check, and give it its rate back
    auto rateHz = [this] {
        const auto t = ch().tryUntil(":GT#", '#', 800);
        const auto v = t ? number(trim(*t)) : std::nullopt;
        return v.value_or(60.0);
    };
    if (rateHz() >= 1) return;
    setTrackRate(trackRate_);
    numeric(":Te#");
    if (rateHz() < 1)
        throw MountError("the mount says it's tracking, but at a rate of 0 (lost after a restart?): set sidereal tracking in its own app");
}

void OnStepDriver::setTrackRate(TrackRate r) {
    ch().send(r == TrackRate::Lunar ? ":TL#" : r == TrackRate::Solar ? ":TS#" : r == TrackRate::King ? ":TK#" : ":TQ#");   // no reply
    trackRate_ = r;
}

void OnStepDriver::setRateOffsets(double raArcsecPerSec, double decArcsecPerSec) {
    // OnStepX: arcseconds per sidereal second, at most +-1800
    char buf[48];
    std::snprintf(buf, sizeof buf, ":SXTR,%.5f#", std::clamp(raArcsecPerSec * kSiderealSecond, -1800.0, 1800.0));
    if (!numeric(buf)) throw MountError("the mount refused the RA rate offset: " + lastError());
    std::snprintf(buf, sizeof buf, ":SXTD,%.5f#", std::clamp(decArcsecPerSec * kSiderealSecond, -1800.0, 1800.0));
    if (!numeric(buf)) throw MountError("the mount refused the Dec rate offset: " + lastError());
}

void OnStepDriver::park() {
    if (!numeric(":hP#")) throw MountError("the mount couldn't park: " + lastError());
}

void OnStepDriver::unpark() {
    if (!numeric(":hR#")) throw MountError("the mount couldn't unpark: " + lastError());
}

void OnStepDriver::home() { ch().send(":hC#"); }

void OnStepDriver::sync(double ra, double dec) {
    setTarget(ra, dec);
    const auto r = trim(ch().until(":CM#", '#', 3000));   // N/A, or E and a :MS# code
    if (r.size() >= 2 && r[0] == 'E') throw MountError(std::string("the mount refused the sync: ") + onStepGotoError(r[1]));
}

// ---------------------------------------------------------------- settings (what OnStepX's web server sets)

namespace {

std::string num(double v, int decimals) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

/// "+51*30:00" / "+000*06:00": OnStep's set form
std::string sexagesimal(double deg, int width) {
    const long s = std::lround(std::abs(deg) * 3600);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%c%0*ld*%02ld:%02ld", deg < 0 ? '-' : '+', width, s / 3600, s / 60 % 60, s % 60);
    return buf;
}

/// "+05:00" from "5", "+5", "-3:30", "+05:00"; nullopt for anything else
std::optional<std::string> utcOffset(const std::string& text) {
    char sign = '+';
    int h = 0, m = 0;
    const std::string t = trim(text);
    const int n = t.empty() || (t[0] != '+' && t[0] != '-') ? std::sscanf(t.c_str(), "%d:%d", &h, &m) + 1
                                                            : std::sscanf(t.c_str(), "%c%d:%d", &sign, &h, &m);
    if (n < 2 || h < 0 || h > 14 || (m != 0 && m != 30 && m != 45)) return std::nullopt;
    char buf[16];
    std::snprintf(buf, sizeof buf, "%c%02d:%02d", sign, h, m);
    return std::string(buf);
}

void tmOf(std::time_t t, std::tm& local, std::tm& utc) {
#ifdef _WIN32
    localtime_s(&local, &t);
    gmtime_s(&utc, &t);
#else
    localtime_r(&t, &local);
    gmtime_r(&t, &utc);
#endif
}

long secondsOf(const std::tm& tm) {
    return daysFromCivil(tm.tm_year + 1900, static_cast<unsigned>(tm.tm_mon + 1), static_cast<unsigned>(tm.tm_mday)) * 86400 +
           tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

const char* const kGroupSite = "Site and time";
const char* const kGroupTracking = "Tracking";
const char* const kGroupLimits = "Limits";
const char* const kGroupFlips = "Meridian flips";
const char* const kGroupSpeeds = "Guiding and gotos";
const char* const kGroupBacklash = "Backlash";
const char* const kGroupPark = "Home and park";
const char* const kGroupAbout = "About the mount";

}  // namespace

void OnStepDriver::setNumeric(const std::string& cmd, const std::string& what) {
    if (!numeric(cmd)) throw MountError("the mount refused " + what + ": " + lastError());
}

std::vector<MountSetting> OnStepDriver::settings() {
    using K = MountSetting::Kind;
    std::vector<MountSetting> out;
    auto get = [this](const char* cmd) -> std::optional<std::string> {
        if (const auto r = ch().tryUntil(cmd, '#', 600)) return trim(*r);
        return std::nullopt;   // (a bare 0: the firmware doesn't have it)
    };
    auto add = [&](MountSetting s) { out.push_back(std::move(s)); };
    const auto st = parseOnStepStatus(get(":GU#").value_or("")).value_or(OnStepStatus{});

    // site and time
    auto lat = get(":GtH#");
    if (!lat) lat = get(":Gt#");
    if (const auto v = lat ? degrees(*lat) : std::nullopt)
        add({"latitude", kGroupSite, "Latitude", K::Number, num(*v, 4), {}, -90, 90, 4, "\xC2\xB0", "North positive"});
    auto lon = get(":GgH#");
    if (!lon) lon = get(":Gg#");
    if (const auto v = lon ? degrees(*lon) : std::nullopt)
        add({"longitude", kGroupSite, "Longitude", K::Number, num(-*v, 4), {}, -180, 180, 4, "\xC2\xB0",
             "East positive, west negative (OnStep itself counts west as positive: SlopCap turns it round)"});
    if (const auto v = get(":Gv#"); v && number(*v))
        add({"elevation", kGroupSite, "Elevation", K::Number, num(*number(*v), 1), {}, -500, 9000, 1, "m", "Above sea level"});
    if (const auto v = get(":GG#"))
        add({"utcOffset", kGroupSite, "UTC offset", K::Text, *v, {}, 0, 0, 0, "",
             "Hours to add to the mount's local time to get UTC: +05:00 for US Eastern standard time, +04:00 for daylight "
             "time. OnStep doesn't change it for daylight saving by itself."});
    {
        const auto date = get(":GC#"), time = get(":GL#");
        if (date && time) {
            MountInfo tmp;
            checkClock(tmp);
            add({"clock", kGroupSite, "Mount's clock", K::Info,
                 *date + "  " + *time + (tmp.notes.empty() ? "  (agrees with this computer)" : "  (" + tmp.notes.front() + ")"), {}, 0, 0, 0, "", ""});
        }
    }
    add({"syncTime", kGroupSite, "Set date, time and UTC offset from this computer", K::Action, "", {}, 0, 0, 0, "",
         "Sends this computer's date, local time and UTC offset to the mount (:SG, :SC, :SL). Set the mount's clock now?"});

    // tracking
    add({"trackRate", kGroupTracking, "Rate", K::Choice,
         st.rate == TrackRate::Lunar ? "lunar" : st.rate == TrackRate::Solar ? "solar" : st.rate == TrackRate::King ? "king" : "sidereal",
         {{"sidereal", "Sidereal"}, {"lunar", "Lunar"}, {"solar", "Solar"}, {"king", "King"}}, 0, 0, 0, "", "The rate the mount tracks at"});
    add({"compensation", kGroupTracking, "Compensation", K::Choice, st.compensation == 'r' ? "refraction" : st.compensation == 't' ? "full" : "none",
         {{"none", "None"}, {"refraction", "Refraction"}, {"full", "Full (pointing model, OnTrack)"}}, 0, 0, 0, "",
         "Track faster or slower as refraction (and, with full, the pointing model) moves the object"});
    add({"dualAxis", kGroupTracking, "Compensate in Dec too", K::Toggle, st.compensation && !st.singleAxis ? "1" : "0", {}, 0, 0, 0, "",
         "With compensation on: adjust both axes, not just RA (:T2# / :T1#)"});

    // limits
    if (const auto v = get(":Gh#"); v && degrees(*v))
        add({"horizon", kGroupLimits, "Horizon limit", K::Number, num(*degrees(*v), 0), {}, -30, 30, 0, "\xC2\xB0",
             "The lowest altitude the mount goes to or tracks into"});
    if (const auto v = get(":Go#"); v && degrees(*v))
        add({"overhead", kGroupLimits, "Overhead limit", K::Number, num(*degrees(*v), 0), {}, 60, 90, 0, "\xC2\xB0",
             "The highest altitude a goto may go to"});
    if (mountType_ == 'E') {
        if (const auto v = get(":GXE9#"); v && number(*v))
            add({"meridianEast", kGroupLimits, "Past the meridian, east", K::Number, num(*number(*v), 0), {}, -180, 180, 0, "min",
                 "How far past the meridian the mount may go on the east side of the pier, in minutes of hour angle"});
        if (const auto v = get(":GXEA#"); v && number(*v))
            add({"meridianWest", kGroupLimits, "Past the meridian, west", K::Number, num(*number(*v), 0), {}, -180, 180, 0, "min",
                 "How far past the meridian the mount may track on the west side before it stops (or flips)"});

        // meridian flips
        if (const auto v = get(":GX95#"))
            add({"autoFlip", kGroupFlips, "Flip automatically", K::Toggle, *v == "1" ? "1" : "0", {}, 0, 0, 0, "",
                 "At the west meridian limit, flip and carry on rather than stop"});
        if (const auto v = get(":GX96#"); v && !v->empty())
            add({"pierSide", kGroupFlips, "Preferred pier side", K::Choice, v->substr(0, 1),
                 {{"E", "East"}, {"W", "West"}, {"B", "Best (stay on this side if it can)"}, {"A", "Auto"}}, 0, 0, 0, "",
                 "The side a goto chooses when both would do"});
        add({"flipHome", kGroupFlips, "A flip goes", K::Choice, st.pauseAtHome ? "2" : st.visitHome ? "1" : "0",
             {{"0", "Straight across"}, {"1", "Through the home position"}, {"2", "Through home, and waits there"}}, 0, 0, 0, "",
             "The way to the other side of the pier: directly, by way of the home position, or stopping at home until told to "
             "carry on. OnStepX before mid-2026 (10.28) has only straight across and waiting at home"});
    }

    // guiding and gotos
    if (st.pulseRate >= 0 && st.pulseRate <= 2)
        add({"pulseRate", kGroupSpeeds, "Guide pulse rate", K::Choice, std::to_string(st.pulseRate),
             {{"0", "0.25\xC3\x97 sidereal"}, {"1", "0.5\xC3\x97 sidereal"}, {"2", "1\xC3\x97 sidereal"}}, 0, 0, 0, "",
             "The speed of guide pulses (PHD2's): change PHD2's calibration after changing it"});
    {
        const auto now = get(":GX97#");
        add({"gotoSpeed", kGroupSpeeds, "Goto speed", K::Choice, "",
             {{"", "As it is" + (now ? " (" + *now + "\xC2\xB0/s)" : std::string())}, {"5", "50 %"}, {"4", "67 %"}, {"3", "100 % (as configured)"},
              {"2", "150 %"}, {"1", "200 %"}},
             0, 0, 0, "", "Gotos at this share of the speed the firmware was configured for (:SX93)"});
    }

    // backlash
    if (const auto v = get(":%BR#"); v && number(*v))
        add({"backlashRa", kGroupBacklash, "RA backlash", K::Number, num(*number(*v), 0), {}, 0, 3600, 0, "\xE2\x80\xB3",
             "Taken up at speed when the RA axis reverses. A friction drive has none: 0"});
    if (const auto v = get(":%BD#"); v && number(*v))
        add({"backlashDec", kGroupBacklash, "Dec backlash", K::Number, num(*number(*v), 0), {}, 0, 3600, 0, "\xE2\x80\xB3",
             "Taken up at speed when the Dec axis reverses. A friction drive has none: 0"});

    // home and park; the buzzer
    add({"buzzer", kGroupPark, "Buzzer", K::Toggle, st.buzzer ? "1" : "0", {}, 0, 0, 0, "", "Beeps on gotos, limits and faults"});
    add({"goHome", kGroupPark, "Go to the home position", K::Action, "", {}, 0, 0, 0, "",
         "Slews to home (pointing at the pole, counterweights down). Move the mount now?"});
    add({"setPark", kGroupPark, "Park here", K::Action, "", {}, 0, 0, 0, "",
         "Makes where the mount points now its park position (:hQ#). Replace the park position?"});

    // about
    std::string about = "OnStep" + std::string(std::atoi(firmware_.c_str()) >= 10 ? "X " : " ") + firmware_;
    about += st.mountType == 'E' ? ", German equatorial" : st.mountType == 'K' ? ", fork" : st.mountType == 'A' ? ", alt-az" : "";
    if (const auto a = get(":GXE4#"), b = get(":GXE5#"); a && b) about += ", " + *a + " / " + *b + " steps per degree";
    add({"about", kGroupAbout, "Firmware", K::Info, about, {}, 0, 0, 0, "", ""});
    return out;
}

void OnStepDriver::syncTime() {
    std::tm local{}, utc{};
    tmOf(std::chrono::system_clock::to_time_t(now_()), local, utc);
    const long ahead = secondsOf(local) - secondsOf(utc);   // local time minus UTC
    const long m = std::abs(ahead) / 60;
    char buf[48];
    std::snprintf(buf, sizeof buf, ":SG%c%02ld:%02ld#", ahead <= 0 ? '+' : '-', m / 60, m % 60);   // what to add to local for UTC
    setNumeric(buf, "the UTC offset");
    std::snprintf(buf, sizeof buf, ":SC%02d/%02d/%02d#", local.tm_mon + 1, local.tm_mday, local.tm_year % 100);
    setNumeric(buf, "the date");
    std::snprintf(buf, sizeof buf, ":SL%02d:%02d:%02d#", local.tm_hour, local.tm_min, local.tm_sec);
    setNumeric(buf, "the time");
}

void OnStepDriver::setFlipHome(int mode) {
    // :SX94,n# since OnStepX 10.28 (mid-2026); before it, :SX98 only turned the pause at home on and off. On the newer
    // firmware :SX98,0 turns a pause into going through home, so it's the fallback only, for a firmware without :SX94
    if (numeric(":SX94," + std::to_string(mode) + "#")) return;
    if (mode == 1) throw MountError("this OnStep can't flip through home without waiting there (OnStepX 10.28 and later can)");
    setNumeric(std::string(":SX98,") + (mode == 2 ? '1' : '0') + "#", "the way it flips");
}

void OnStepDriver::changeSetting(const std::string& key, const std::string& value) {
    const auto v = number(trim(value));
    auto need = [&](double lo, double hi) {
        if (!v || *v < lo || *v > hi) throw MountError("not a value for " + key + ": " + value);
        return *v;
    };
    auto flag = [&] { return value == "1" ? '1' : '0'; };
    char buf[64];
    if (key == "latitude") setNumeric(":St" + sexagesimal(need(-90, 90), 2) + "#", "the latitude");
    else if (key == "longitude") setNumeric(":Sg" + sexagesimal(-need(-180, 180), 3) + "#", "the longitude");   // west positive
    else if (key == "elevation") setNumeric(":Sv" + num(need(-500, 9000), 1) + "#", "the elevation");
    else if (key == "utcOffset") {
        const auto o = utcOffset(value);
        if (!o) throw MountError("a UTC offset is like +05:00 (minutes 00, 30 or 45): " + value);
        setNumeric(":SG" + *o + "#", "the UTC offset");
    } else if (key == "syncTime") syncTime();
    else if (key == "trackRate")
        setTrackRate(value == "lunar" ? TrackRate::Lunar : value == "solar" ? TrackRate::Solar : value == "king" ? TrackRate::King : TrackRate::Sidereal);
    else if (key == "compensation") setNumeric(value == "refraction" ? ":Tr#" : value == "full" ? ":To#" : ":Tn#", "the compensation");
    else if (key == "dualAxis") setNumeric(value == "1" ? ":T2#" : ":T1#", "dual-axis tracking");
    else if (key == "horizon") {
        std::snprintf(buf, sizeof buf, ":Sh%+03d#", static_cast<int>(std::lround(need(-30, 30))));
        setNumeric(buf, "the horizon limit");
    } else if (key == "overhead") {
        std::snprintf(buf, sizeof buf, ":So%02d#", static_cast<int>(std::lround(need(60, 90))));
        setNumeric(buf, "the overhead limit");
    } else if (key == "meridianEast" || key == "meridianWest") {
        std::snprintf(buf, sizeof buf, ":SXE%c,%d#", key == "meridianEast" ? '9' : 'A', static_cast<int>(std::lround(need(-180, 180))));
        setNumeric(buf, "the meridian limit");
    } else if (key == "autoFlip") setNumeric(std::string(":SX95,") + flag() + "#", "automatic flips");
    else if (key == "pierSide") {
        if (value.size() != 1 || std::string("EWBA").find(value[0]) == std::string::npos) throw MountError("not a pier side: " + value);
        setNumeric(":SX96," + value + "#", "the preferred pier side");
    } else if (key == "flipHome") setFlipHome(static_cast<int>(need(0, 2)));
    else if (key == "buzzer") setNumeric(std::string(":SX97,") + flag() + "#", "the buzzer setting");
    else if (key == "pulseRate") {
        const int r = static_cast<int>(need(0, 2));
        ch().send(":R" + std::to_string(r) + "#");   // no reply; with the moves' rate too
        pulseRate_ = guideRate_ = moveRate_ = r;
        ratesChanged_ = false;
    } else if (key == "gotoSpeed") {
        if (value.empty()) return;
        const int p = static_cast<int>(need(1, 5));
        ch().send(":SX93," + std::to_string(p) + "#");   // no reply
    } else if (key == "backlashRa" || key == "backlashDec") {
        std::snprintf(buf, sizeof buf, ":$B%c%d#", key == "backlashRa" ? 'R' : 'D', static_cast<int>(std::lround(need(0, 3600))));
        setNumeric(buf, "the backlash");
    } else if (key == "goHome") home();
    else if (key == "setPark") setNumeric(":hQ#", "the park position");
    else throw MountError("OnStep has no setting " + key);
}

}  // namespace slopcap::mounts
