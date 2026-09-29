#include "world/layout.h"

#include <math.h>

const Tower TOWERS[3] = {
  TOWER,
  { 45, -200, 2.7, PALETTE_LIME },
  { 45, 200, 2.7, PALETTE_SUNSET },
};

const CrossStreet CROSS_STREETS[16] = {
  { -850, nullptr, true, false }, { -740, nullptr, true, false }, { -630, nullptr, true, false },
  { -520, nullptr, true, false }, { -410, nullptr, true, false },
  { -300, "12 ST", false, false }, { -190, "11 ST", false, true }, { -79, "10 ST", false, false },
  { 79, "8 ST", false, false }, { 190, "7 ST", false, false }, { 300, "6 ST", false, false },
  { 410, nullptr, true, false }, { 520, nullptr, true, false }, { 630, nullptr, true, false },
  { 740, nullptr, true, false }, { 850, nullptr, true, false },
};

double roadHeight(double x) {
  double c = LANES.centerX;
  double w = x < c ? c - PARKING.x0 : LANES.x1 - c;
  double t = fmin(1, fabs(x - c) / w);
  return ROAD_CROWN * (1 - t * t);
}

void crossLegs(double zc, double out[2][2]) {
  double e = CROSS.hw + CROSS.R;
  out[0][0] = zc - e - 4.2; out[0][1] = zc - e - 0.2;
  out[1][0] = zc + e + 0.2; out[1][1] = zc + e + 4.2;
}

const CrossStreet *crossStreetAt(double z, double pad) {
  for (int i = 0; i < NCROSS_STREETS; i++)
    if (fabs(z - CROSS_STREETS[i].z) < CROSS.gap + pad) return &CROSS_STREETS[i];
  return nullptr;
}

double crossRoadHeight(double x, double dz) {
  double t = fmin(1, fabs(dz) / CROSS.hw);
  double fade = fmin(1, fmax(0, (SIDEWALK_W.x1 - x) / 4));
  return CROSS.crown * (1 - t * t) * fade;
}

// Walk height on the hotel side where a cross street cuts the sidewalk; NAN elsewhere (JS: null).
static double crossGround(double x, double z) {
  const CrossStreet *c = crossStreetAt(z, 0);
  if (!c) return NAN;
  double dz = z - c->z, adz = fabs(dz);
  double hw = CROSS.hw, R = CROSS.R, xw0 = CROSS.xw0, xw1 = CROSS.xw1, rampRun = CROSS.rampRun;
  if (adz < hw) return crossRoadHeight(x, dz);
  if (adz < hw + R && x > SIDEWALK_W.x1 - R) {
    // curb return: road outside the quarter circle
    double cx = SIDEWALK_W.x1 - R, cz = c->z + js_sign(dz) * (hw + R);
    if (js_hypot2(x - cx, z - cz) > R) return 0;
  }
  if (x > xw0 && x < xw1 && adz < hw + rampRun) return CURB_HEIGHT * (adz - hw) / rampRun;   // curb ramp
  return NAN;
}

V3 compassToDir(double azimuthDeg, double elevationDeg) {
  double az = (azimuthDeg * PI_D) / 180;
  double el = (elevationDeg * PI_D) / 180;
  return v3(sin(az) * cos(el), sin(el), -cos(az) * cos(el));
}

double sandHeight(double x) {
  if (x < SAND.x0) return CURB_HEIGHT;
  if (x < SAND.waterline - 4) {
    double t = (x - SAND.x0) / (SAND.waterline - 4 - SAND.x0);
    return 0.55 - (0.55 - (SEA_LEVEL + 0.1)) * t;
  }
  // beach face steepens into the water
  double t = (x - (SAND.waterline - 4)) / 26;
  return SEA_LEVEL + 0.1 - 1.6 * fmin(1, t) * fmin(1, t) - 0.2 * fmax(0, t - 1);
}

double SHORE_X, BREAK_X, WET_LINE_X;

void layout_init(void) {
  double x = SAND.waterline - 10;
  while (sandHeight(x) > SEA_LEVEL && x < SAND.waterline + 30) x += 0.05;
  SHORE_X = x;
  BREAK_X = SHORE_X + 2.5;
  WET_LINE_X = BREAK_X - SWASH_MAX - 0.6;
}

static double smooth(double a, double b, double v) {
  double t = fmin(1, fmax(0, (v - a) / (b - a)));
  return t * t * (3 - 2 * t);
}

double sandDetail(double x, double z) {
  double fade = smooth(SAND.x0 + 0.6, SAND.x0 + 4, x) * (1 - smooth(WET_LINE_X - 6, WET_LINE_X - 1, x))
    * (1 - smooth(SAND_DETAIL_Z - 40, SAND_DETAIL_Z, fabs(z)));
  if (fade <= 0) return 0;
  double h = 0.05 * sin(x * 0.21 + sin(z * 0.05) * 1.7) * sin(z * 0.13 + x * 0.04)
    + 0.03 * sin(x * 0.61 + z * 0.23 + 1.3) * sin(z * 0.37 - x * 0.19)
    + 0.012 * sin(x * 1.7 + z * 0.9) * sin(z * 1.3 - x * 0.7 + 2.1);
  return h * fade;
}

double groundHeight(double x, double z) {
  if (x < SIDEWALK_W.x1) {
    double g = crossGround(x, z);
    return isnan(g) ? CURB_HEIGHT : g;   // crossGround(x, z) ?? CURB_HEIGHT
  }
  if (x < LANES.x1) return roadHeight(x);
  if (x < SAND.x0) return CURB_HEIGHT;
  return sandHeight(x) + sandDetail(x, z);   // (under the sea: the seabed you wade on)
}
