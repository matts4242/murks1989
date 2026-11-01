// Murks 1989 — geographic helpers for georeferenced maps.
//
// EPSG:3035 (ETRS89 / LAEA Europe) inverse projection, ellipsoidal form
// (Snyder 1987, "Map Projections — A Working Manual", pp. 187-190).
#pragma once

#include "map.hpp"

#include <cmath>

namespace murks {

// Projected easting/northing (metres) -> longitude/latitude (degrees).
inline void laea3035_inverse(double easting, double northing, double &lon_deg, double &lat_deg) {
    const double PI = 3.14159265358979323846;
    const double a = 6378137.0, e2 = 0.00669438002290, e = std::sqrt(e2);
    const double lat0 = 52.0 * PI / 180.0, lon0 = 10.0 * PI / 180.0;
    const double FE = 4321000.0, FN = 3210000.0;
    auto q = [&](double phi) {
        double s = std::sin(phi);
        return (1 - e2) * (s / (1 - e2 * s * s) - (1 / (2 * e)) * std::log((1 - e * s) / (1 + e * s)));
    };
    const double qp = q(PI / 2), q1 = q(lat0);
    const double beta1 = std::asin(q1 / qp);
    const double Rq = a * std::sqrt(qp / 2);
    const double D = a * std::cos(lat0) / (std::sqrt(1 - e2 * std::sin(lat0) * std::sin(lat0)) * Rq * std::cos(beta1));
    double x = easting - FE, y = northing - FN;
    double rho = std::sqrt((x / D) * (x / D) + (D * y) * (D * y));
    if (rho < 1e-9) {
        lon_deg = 10.0;
        lat_deg = 52.0;
        return;
    }
    double C = 2 * std::asin(rho / (2 * Rq));
    double beta = std::asin(std::cos(C) * std::sin(beta1) + (D * y * std::sin(C) * std::cos(beta1)) / rho);
    double lam = lon0 + std::atan2(x * std::sin(C), D * rho * std::cos(beta1) * std::cos(C) -
                                                        D * D * y * std::sin(beta1) * std::sin(C));
    double e4 = e2 * e2, e6 = e4 * e2;
    double phi = beta + (e2 / 3 + 31 * e4 / 180 + 517 * e6 / 5040) * std::sin(2 * beta) +
                 (23 * e4 / 360 + 251 * e6 / 3780) * std::sin(4 * beta) + (761 * e6 / 45360) * std::sin(6 * beta);
    lon_deg = lam * 180.0 / PI;
    lat_deg = phi * 180.0 / PI;
}

// World position (metres on the map) -> longitude/latitude. False if the map
// is not georeferenced in EPSG:3035.
inline bool world_to_lonlat(const Map &map, double wx, double wy, double &lon, double &lat) {
    if (map.epsg != 3035) return false;
    laea3035_inverse(map.geo_origin_e + wx, map.geo_origin_n - wy, lon, lat);
    return true;
}

} // namespace murks
