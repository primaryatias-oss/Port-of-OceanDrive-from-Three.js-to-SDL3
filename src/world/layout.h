// Shared layout constants (metres, y up): port of src/world/layout.js.
// +x = east (ocean), -x = west (hotels), -z = north, +z = south.
#pragma once

#include "math/vmath.h"

typedef struct { double zMin, zMax; } ZRange;
static constexpr ZRange BLOCK = { -70, 70 };
static constexpr double WORLD_Z = 2500;   // how far street / beach visually continue

typedef struct { double frontX, maxSetback, patioX, backX; } HotelLayout;
static constexpr HotelLayout HOTEL = { -30, 3, -28, -55 };

static constexpr double CURB_HEIGHT = 0.15;

typedef struct { double x0, x1; } XRange;
static constexpr XRange SIDEWALK_W = { -30, -24 };   // hotel-side sidewalk
static constexpr XRange PARKING = { -24, -21.5 };
typedef struct { double x0, x1, centerX; } Lanes;
static constexpr Lanes LANES = { -21.5, -14.5, -18 };
static constexpr XRange SIDEWALK_E = { -14.5, -10 }; // park-side sidewalk
typedef struct { double x0, x1, promenadeX, wallX; } ParkLayout;
static constexpr ParkLayout PARK = { -10, 12, 0, 12 };
typedef struct { double x0, waterline; } SandLayout;
static constexpr SandLayout SAND = { 12, 90 };
// Mean sea level sits 1 m below the street.
static constexpr double SEA_LEVEL = -1.0;
typedef struct { double x0, y; } OceanLayout;
static constexpr OceanLayout OCEAN = { 86, SEA_LEVEL };

typedef struct { double x, z; } XZ;
static constexpr XZ CAR = { -22.75, 8 };
static constexpr double CROSSWALK_Z = -10;

static constexpr double ROAD_CROWN = 0.09;
double roadHeight(double x);

typedef enum TowerPalette { PALETTE_DEFAULT, PALETTE_LIME, PALETTE_SUNSET } TowerPalette;
typedef struct Tower { double x, z, deckHeight; TowerPalette palette; } Tower;
static constexpr Tower TOWER = { 45, 5, 2.7, PALETTE_DEFAULT };
extern const Tower TOWERS[3];

static constexpr ZRange DISTRICT = { -340, 340 };

typedef struct { double hw, gap, R, xw0, xw1, rampRun, crown; } CrossLayout;
static constexpr CrossLayout CROSS = { 5.5, 9, 1.8, -29.6, -26.0, 1.3, 0.06 };
typedef struct CrossStreet { double z; const char *name; bool far, signal; } CrossStreet;
extern const CrossStreet CROSS_STREETS[16];
enum { NCROSS_STREETS = 16 };
// Ocean Drive crosswalk legs north / south of a cross street: [z0, z1]
void crossLegs(double zc, double out[2][2]);
// the cross street within gap + pad of z, or null
const CrossStreet *crossStreetAt(double z, double pad);
double crossRoadHeight(double x, double dz);

static constexpr double EYE_HEIGHT = 1.7;

typedef struct { double azimuthDeg, elevationDeg; } SunLayout;
static constexpr SunLayout SUN = { 100, 7 };
V3 compassToDir(double azimuthDeg, double elevationDeg);

double sandHeight(double x);
extern double SHORE_X;        // x where the beach face meets mean sea level
extern double BREAK_X;        // SHORE_X + 2.5
static constexpr double SWASH_MAX = 5.5;
extern double WET_LINE_X;     // BREAK_X - SWASH_MAX - 0.6

static constexpr double SAND_DETAIL_Z = 340;
double sandDetail(double x, double z);
double groundHeight(double x, double z);

// computes SHORE_X / BREAK_X / WET_LINE_X (module-load constants in the JS); call once at start
void layout_init(void);
