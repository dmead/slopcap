// Low-precision positions of the Moon, Mars and the outer planets: enough to tell which one a telescope points at and
// how big it should look.
//   planets  JPL's published Keplerian elements and rates ("Approximate Positions of the Major Planets", Standish,
//            table 1, valid 1800-2050), with light-time; within ~10' of libnova's VSOP87 over 2000-2045
//   Moon     the Astronomical Almanac's low-precision series (~0.3°), geocentric
// Both precessed to the equinox of date to match what mounts report (JNow). Nutation and aberration (under 1') are
// left out.
#pragma once

#include <chrono>

namespace slopcap::analysis {

enum class Body { Moon, Mars, Jupiter, Saturn, Uranus, Neptune };
inline constexpr Body kBodies[] = {Body::Moon, Body::Mars, Body::Jupiter, Body::Saturn, Body::Uranus, Body::Neptune};
const char* bodyName(Body b);

struct BodyPosition {
    double ra = 0;              ///< hours, geocentric, equinox of date
    double dec = 0;             ///< degrees
    double distanceAu = 0;      ///< from Earth
    double diameterArcsec = 0;  ///< equatorial diameter of the globe (Saturn without its rings)
    double ringTiltDeg = 0;     ///< Saturn: the rings' opening as seen from Earth (0 = edge-on); 0 for the others
    double illuminated = 1;     ///< fraction of the disc that is lit: the Moon's phase, Mars down to ~0.84
};

/// Julian date (UTC) of a time point.
double julianDate(std::chrono::system_clock::time_point t);

/// Where the body is at `jdUtc`.
BodyPosition bodyPosition(Body b, double jdUtc);

/// Angle between two directions (RA in hours, Dec in degrees), in degrees.
double separationDeg(double ra1, double dec1, double ra2, double dec2);

/// Altitude above the horizon (degrees) of RA/Dec of date, at a site (latitude, longitude east positive, degrees).
double altitudeDeg(double ra, double dec, double jdUtc, double latitude, double longitude);

}  // namespace slopcap::analysis
