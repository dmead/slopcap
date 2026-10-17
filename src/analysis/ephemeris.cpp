#include "analysis/ephemeris.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace slopcap::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kJ2000 = 2451545.0;
constexpr double kLightDaysPerAu = 1.0 / 173.1446327;   // days for light to cross 1 AU
constexpr double kTtMinusUtcDays = 69.184 / 86400.0;    // TT - UTC (37 leap seconds + 32.184 s), close enough here

/// Keplerian elements at J2000 and their rates per Julian century (JPL table 1): a (AU), e, I, L, long. perihelion,
/// long. ascending node (degrees).
struct Elements {
    double a, e, i, l, peri, node;
    double da, de, di, dl, dperi, dnode;
};

constexpr Elements kEarthMoon{1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
                              0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0};
constexpr std::array<Elements, 5> kElements{{
    {1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,   // Mars
     0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343},
    {5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,   // Jupiter
     -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106},
    {9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,   // Saturn
     -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794},
    {19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,   // Uranus
     -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589},
    {30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,   // Neptune
     0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664},
}};

/// Equatorial diameter at 1 AU, arcseconds (Meeus, table 55.A, doubled).
constexpr std::array<double, 5> kDiameterAt1Au{9.36, 196.88, 165.46, 70.48, 68.30};

struct Vec {
    double x, y, z;
};

/// Heliocentric position, J2000 ecliptic, AU, at `jdTt`.
Vec heliocentric(const Elements& el, double jdTt) {
    const double t = (jdTt - kJ2000) / 36525.0;
    const double a = el.a + el.da * t, e = el.e + el.de * t, inc = (el.i + el.di * t) * kDeg;
    const double l = el.l + el.dl * t, peri = el.peri + el.dperi * t, node = el.node + el.dnode * t;
    const double w = (peri - node) * kDeg, om = node * kDeg;
    double m = std::remainder(l - peri, 360.0) * kDeg;
    double ecc = m + e * std::sin(m);   // Kepler's equation, Newton's method
    for (int k = 0; k < 10; ++k) {
        const double d = (ecc - e * std::sin(ecc) - m) / (1 - e * std::cos(ecc));
        ecc -= d;
        if (std::abs(d) < 1e-12) break;
    }
    const double xp = a * (std::cos(ecc) - e), yp = a * std::sqrt(1 - e * e) * std::sin(ecc);
    const double cw = std::cos(w), sw = std::sin(w), co = std::cos(om), so = std::sin(om), ci = std::cos(inc), si = std::sin(inc);
    return {(cw * co - sw * so * ci) * xp + (-sw * co - cw * so * ci) * yp,
            (cw * so + sw * co * ci) * xp + (-sw * so + cw * co * ci) * yp,
            (sw * si) * xp + (cw * si) * yp};
}

/// J2000 ecliptic -> J2000 equatorial.
Vec toEquatorial(const Vec& v) {
    const double eps = 23.43928 * kDeg;
    return {v.x, v.y * std::cos(eps) - v.z * std::sin(eps), v.y * std::sin(eps) + v.z * std::cos(eps)};
}

/// Precess J2000 RA/Dec (radians) to the equinox of `jdTt` (Meeus 21.2-21.4).
void precess(double& ra, double& dec, double jdTt) {
    const double t = (jdTt - kJ2000) / 36525.0;
    const double as = kDeg / 3600.0;
    const double zeta = (2306.2181 * t + 0.30188 * t * t + 0.017998 * t * t * t) * as;
    const double z = (2306.2181 * t + 1.09468 * t * t + 0.018203 * t * t * t) * as;
    const double theta = (2004.3109 * t - 0.42665 * t * t - 0.041833 * t * t * t) * as;
    const double a = std::cos(dec) * std::sin(ra + zeta);
    const double b = std::cos(theta) * std::cos(dec) * std::cos(ra + zeta) - std::sin(theta) * std::sin(dec);
    const double c = std::sin(theta) * std::cos(dec) * std::cos(ra + zeta) + std::cos(theta) * std::sin(dec);
    ra = std::atan2(a, b) + z;
    dec = std::asin(std::clamp(c, -1.0, 1.0));
}

/// The Moon, geocentric, equinox of date: the Astronomical Almanac's low-precision series (~0.3° in position).
BodyPosition moonPosition(double jdTt) {
    const double t = (jdTt - kJ2000) / 36525.0;
    auto sn = [](double deg) { return std::sin(deg * kDeg); };
    auto cs = [](double deg) { return std::cos(deg * kDeg); };
    const double lon = 218.32 + 481267.881 * t + 6.29 * sn(135.0 + 477198.87 * t) - 1.27 * sn(259.3 - 413335.36 * t) +
                       0.66 * sn(235.7 + 890534.22 * t) + 0.21 * sn(269.9 + 954397.74 * t) - 0.19 * sn(357.5 + 35999.05 * t) -
                       0.11 * sn(186.5 + 966404.03 * t);
    const double lat = 5.13 * sn(93.3 + 483202.02 * t) + 0.28 * sn(228.2 + 960400.89 * t) - 0.28 * sn(318.3 + 6003.15 * t) -
                       0.17 * sn(217.6 - 407332.21 * t);
    const double parallax = 0.9508 + 0.0518 * cs(135.0 + 477198.87 * t) + 0.0095 * cs(259.3 - 413335.36 * t) +
                            0.0078 * cs(235.7 + 890534.22 * t) + 0.0028 * cs(269.9 + 954397.74 * t);
    const double l = lon * kDeg, b = lat * kDeg, eps = (23.439291 - 0.0130042 * t) * kDeg;
    const double x = std::cos(b) * std::cos(l), y = std::cos(b) * std::sin(l), z = std::sin(b);
    const double ye = y * std::cos(eps) - z * std::sin(eps), ze = y * std::sin(eps) + z * std::cos(eps);
    BodyPosition out;
    out.ra = std::fmod(std::atan2(ye, x) / kDeg / 15.0 + 48.0, 24.0);
    out.dec = std::asin(std::clamp(ze, -1.0, 1.0)) / kDeg;
    const double km = 6378.14 / std::sin(parallax * kDeg);
    out.distanceAu = km / 149597870.7;
    out.diameterArcsec = 2 * std::asin(1737.4 / km) / kDeg * 3600.0;

    // Phase: the Sun's ecliptic longitude (the Earth's, reversed; precessed to date) against the Moon's.
    const Vec earth = heliocentric(kEarthMoon, jdTt);
    const double sunAu = std::sqrt(earth.x * earth.x + earth.y * earth.y + earth.z * earth.z);
    const double sunLon = std::atan2(-earth.y, -earth.x) + 1.3969713 * t * kDeg;
    const double elong = std::acos(std::clamp(std::cos(b) * std::cos(l - sunLon), -1.0, 1.0));   // Moon-Earth-Sun
    const double phase = std::atan2(sunAu * std::sin(elong), out.distanceAu - sunAu * std::cos(elong));   // Sun-Moon-Earth
    out.illuminated = (1 + std::cos(phase)) / 2;
    return out;
}

}  // namespace

const char* bodyName(Body b) {
    switch (b) {
        case Body::Moon: return "Moon";
        case Body::Mars: return "Mars";
        case Body::Jupiter: return "Jupiter";
        case Body::Saturn: return "Saturn";
        case Body::Uranus: return "Uranus";
        case Body::Neptune: return "Neptune";
    }
    return "";
}

double julianDate(std::chrono::system_clock::time_point t) {
    const double s = std::chrono::duration<double>(t.time_since_epoch()).count();
    return 2440587.5 + s / 86400.0;   // the Unix epoch
}

BodyPosition bodyPosition(Body b, double jdUtc) {
    const double jd = jdUtc + kTtMinusUtcDays;
    if (b == Body::Moon) return moonPosition(jd);
    const auto pi = static_cast<std::size_t>(b) - 1;   // index among the planets
    const auto& el = kElements[pi];
    const Vec earth = heliocentric(kEarthMoon, jd);   // the Earth-Moon barycentre: within 5000 km of the Earth
    // Light-time: where the planet was when the light we see left it.
    Vec g{}, pl{};
    double dist = 0, tau = 0;
    for (int k = 0; k < 3; ++k) {
        pl = heliocentric(el, jd - tau);
        g = {pl.x - earth.x, pl.y - earth.y, pl.z - earth.z};
        dist = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
        tau = dist * kLightDaysPerAu;
    }
    const Vec q = toEquatorial(g);
    double ra = std::atan2(q.y, q.x), dec = std::asin(q.z / dist);

    BodyPosition out;
    out.distanceAu = dist;
    out.diameterArcsec = kDiameterAt1Au[pi] / dist;
    // Phase angle (Sun-planet-Earth) from the triangle's sides.
    const double r = std::sqrt(pl.x * pl.x + pl.y * pl.y + pl.z * pl.z);
    const double re = std::sqrt(earth.x * earth.x + earth.y * earth.y + earth.z * earth.z);
    out.illuminated = (1 + std::clamp((r * r + dist * dist - re * re) / (2 * r * dist), -1.0, 1.0)) / 2;
    if (b == Body::Saturn) {
        // Sub-Earth latitude on Saturn = the rings' tilt (Saturn's pole, J2000: RA 40.589°, Dec 83.537°).
        const double pra = 40.589 * kDeg, pdec = 83.537 * kDeg;
        const double s = -std::sin(pdec) * std::sin(dec) - std::cos(pdec) * std::cos(dec) * std::cos(pra - ra);
        out.ringTiltDeg = std::asin(std::clamp(s, -1.0, 1.0)) / kDeg;
    }
    precess(ra, dec, jd);
    out.ra = std::fmod(ra / kDeg / 15.0 + 48.0, 24.0);
    out.dec = dec / kDeg;
    return out;
}

double separationDeg(double ra1, double dec1, double ra2, double dec2) {
    const double a1 = ra1 * 15 * kDeg, d1 = dec1 * kDeg, a2 = ra2 * 15 * kDeg, d2 = dec2 * kDeg;
    // haversine: stable for small angles
    const double h = std::pow(std::sin((d2 - d1) / 2), 2) + std::cos(d1) * std::cos(d2) * std::pow(std::sin((a2 - a1) / 2), 2);
    return 2 * std::asin(std::min(1.0, std::sqrt(h))) / kDeg;
}

double altitudeDeg(double ra, double dec, double jdUtc, double latitude, double longitude) {
    const double t = (jdUtc - kJ2000) / 36525.0;
    const double gmst = 280.46061837 + 360.98564736629 * (jdUtc - kJ2000) + 0.000387933 * t * t - t * t * t / 38710000.0;
    const double ha = (gmst + longitude - ra * 15.0) * kDeg;
    const double lat = latitude * kDeg, d = dec * kDeg;
    return std::asin(std::clamp(std::sin(lat) * std::sin(d) + std::cos(lat) * std::cos(d) * std::cos(ha), -1.0, 1.0)) / kDeg;
}

}  // namespace slopcap::analysis
