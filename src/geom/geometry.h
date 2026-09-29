// CPU-side mesh geometry: a port of three.js BufferGeometry (float attributes + optional
// uint32 index) with the generators and utilities the scene uses. Attribute data is stored as
// float (Float32Array semantics: values are computed in double and rounded on store), and the
// attribute order is insertion order, like the keys of a JS object.
#pragma once

#include "core/vec.h"
#include "math/vmath.h"

enum { GEO_MAX_ATTR = 16, GEO_NAME_LEN = 24 };

typedef struct GeoAttr {
  char name[GEO_NAME_LEN];
  int size;          // itemSize (1..4)
  float *data;       // count * size floats
} GeoAttr;

typedef struct Geometry {
  int count;                   // vertex count (the position attribute's count)
  int nattr;
  GeoAttr attr[GEO_MAX_ATTR];
  uint32_t *index;             // null: non-indexed
  int index_count;
  bool has_bbox, has_bsphere;  // computeBoundingBox/Sphere results (null in three until computed)
  Box3 bbox;
  Sphere bsphere;
  // InstancedBufferAttribute (divisor 1): one item per instance, its own count
  int niattr;
  struct { char name[GEO_NAME_LEN]; int size, count; float *data; } iattr[4];
} Geometry;

Geometry *geo_new(void);
void geo_free(Geometry *g);
Geometry *geo_clone(const Geometry *g);

GeoAttr *geo_attr(const Geometry *g, const char *name);          // null if absent
float *geo_data(const Geometry *g, const char *name);            // null if absent
// setAttribute: creates or replaces (keeping its position in the order); returns the zeroed data.
// The first attribute set fixes the vertex count; later ones must match it.
float *geo_set_attr(Geometry *g, const char *name, int size, int count);
float *geo_set_attr_copy(Geometry *g, const char *name, int size, int count, const float *src);
float *geo_set_attr_d(Geometry *g, const char *name, int size, int count, const double *src);
Geometry *geo_delete_attr(Geometry *g, const char *name);
// setAttribute(name, new InstancedBufferAttribute(data, size)): copies count * size floats
void geo_set_iattr(Geometry *g, const char *name, int size, int count, const float *src);
void geo_set_index(Geometry *g, const uint32_t *idx, int n);
static inline int geo_tri_count(const Geometry *g) { return (g->index ? g->index_count : g->count) / 3; }

// transforms (mutate and return g, like three's chained calls)
Geometry *geo_apply_m4(Geometry *g, M4 m);
Geometry *geo_rotate_x(Geometry *g, double a);
Geometry *geo_rotate_y(Geometry *g, double a);
Geometry *geo_rotate_z(Geometry *g, double a);
Geometry *geo_translate(Geometry *g, double x, double y, double z);
Geometry *geo_scale(Geometry *g, double x, double y, double z);
Geometry *geo_compute_vertex_normals(Geometry *g);
Geometry *geo_normalize_normals(Geometry *g);
Box3 geo_compute_bbox(Geometry *g);
Sphere geo_compute_bsphere(Geometry *g);

// new geometries
Geometry *geo_to_non_indexed(const Geometry *g);   // a copy when already non-indexed (see .c)
// BufferGeometryUtils.mergeGeometries (no groups). Consumes nothing; fatal on mismatch.
Geometry *geo_merge(Geometry *const *list, int n);
// merge then free the inputs (the common `mergeGeometries(parts)` + dispose pattern)
Geometry *geo_merge_free(Geometry **list, int n);
Geometry *geo_merge_vertices(const Geometry *g, double tolerance);

// ---- generators (three.js constructors, same parameter defaults) ------------------------
Geometry *geo_box(double w, double h, double d, int ws, int hs, int ds);
Geometry *geo_plane(double w, double h, int ws, int hs);
Geometry *geo_cylinder(double rt, double rb, double h, int radial, int height_segs, bool open,
                       double theta_start, double theta_len);
Geometry *geo_cone(double r, double h, int radial, int height_segs, bool open, double theta_start, double theta_len);
Geometry *geo_sphere(double r, int ws, int hs, double phi_start, double phi_len, double theta_start, double theta_len);
Geometry *geo_torus(double r, double tube, int radial, int tubular, double arc, double theta_start, double theta_len);
Geometry *geo_circle(double r, int segs, double theta_start, double theta_len);
Geometry *geo_lathe(const V2 *pts, int n, int segs, double phi_start, double phi_len);
Geometry *geo_capsule(double r, double h, int cap_segs, int radial, int height_segs);
Geometry *geo_polyhedron(const double *verts, int nverts, const int *idx, int nidx, double radius, int detail);
Geometry *geo_icosahedron(double radius, int detail);
Geometry *geo_rounded_box(double w, double h, double d, int segs, double radius);

// convenience defaults matching three's constructors
#define GEO_TAU (2 * PI_D)
static inline Geometry *geo_box1(double w, double h, double d) { return geo_box(w, h, d, 1, 1, 1); }
static inline Geometry *geo_cyl(double rt, double rb, double h, int radial) {
  return geo_cylinder(rt, rb, h, radial, 1, false, 0, GEO_TAU);
}
static inline Geometry *geo_sphere3(double r, int ws, int hs) { return geo_sphere(r, ws, hs, 0, GEO_TAU, 0, PI_D); }

// ---- curves -----------------------------------------------------------------------------
typedef enum CurveType { CURVE_CENTRIPETAL, CURVE_CHORDAL, CURVE_CATMULLROM } CurveType;
typedef struct CatmullRom3 {
  V3 *pts;
  int n;
  bool closed;
  CurveType type;
  double tension;
  int arc_divisions;     // arcLengthDivisions (200)
  double *lengths;       // cached getLengths(arc_divisions), arc_divisions + 1 values
} CatmullRom3;

// new THREE.CatmullRomCurve3(points, closed = false, curveType = 'centripetal', tension = 0.5)
CatmullRom3 curve_catmull(const V3 *pts, int n, bool closed, CurveType type, double tension);
void curve_free(CatmullRom3 *c);
V3 curve_point(const CatmullRom3 *c, double t);
double curve_length(CatmullRom3 *c);
V3 curve_point_at(CatmullRom3 *c, double u);
void curve_points(const CatmullRom3 *c, int divisions, V3 *out);   // getPoints: divisions + 1 points
typedef struct Frames { V3 *tangents, *normals, *binormals; } Frames;
Frames curve_frenet(CatmullRom3 *c, int segments, bool closed);
void frames_free(Frames *f);

// new THREE.TubeGeometry(path, tubularSegments = 64, radius = 1, radialSegments = 8, closed = false)
Geometry *geo_tube(CatmullRom3 *path, int tubular, double radius, int radial, bool closed);

// ---- 2D paths and shapes ----------------------------------------------------------------
typedef struct PathCurve {
  enum { PC_LINE, PC_ELLIPSE } kind;
  V2 v1, v2;                                       // line
  double ax, ay, xr, yr, a0, a1, rot;              // ellipse
  bool clockwise;
} PathCurve;

typedef struct Path {
  Vec(PathCurve) curves;
  V2 current;
  bool auto_close;
} Path;

typedef struct Shape {
  Path path;
  Vec(Path) holes;
} Shape;

typedef Vec(V2) V2Vec;

Path path_new(void);
Path path_from_points(const V2 *pts, int n);
void path_free(Path *p);
void path_move_to(Path *p, double x, double y);
void path_line_to(Path *p, double x, double y);
void path_absarc(Path *p, double x, double y, double r, double a0, double a1, bool cw);
void path_absellipse(Path *p, double x, double y, double xr, double yr, double a0, double a1, bool cw, double rot);
V2Vec path_get_points(const Path *p, int divisions);   // CurvePath.getPoints

Shape shape_new(void);
Shape shape_from_points(const V2 *pts, int n);
void shape_free(Shape *s);
Path *shape_add_hole(Shape *s);                        // returns the new (empty) hole path

double shape_area(const V2 *c, int n);                 // ShapeUtils.area
static inline bool shape_is_clockwise(const V2 *c, int n) { return shape_area(c, n) < 0; }
// ShapeUtils.triangulateShape: may drop a duplicated end point from contour/holes (in place)
U32Vec shape_triangulate(V2Vec *contour, V2Vec *holes, int nholes);
// earcut(data, holeIndices, dim = 2)
U32Vec earcut(const double *data, int ncoords, const int *hole_idx, int nholes);

Geometry *geo_shape(const Shape *s, int curve_segments);
typedef struct ExtrudeOptions { double depth; int steps, curve_segments; } ExtrudeOptions;   // bevelEnabled: false
Geometry *geo_extrude(const Shape *s, ExtrudeOptions o);
