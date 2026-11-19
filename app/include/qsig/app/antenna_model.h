#pragma once

// "Antennes" module, computation side: elements (wires, dipoles, loops, helices) expanded into the
// straight spans and feeds of the wire MoM (NumericalEM::solveWireNetworkMom, Galerkin rooftop
// currents), the far field radiated by the solved currents (any cut, 3D grid, gain in dBi, front
// to back ratio), frequency sweeps and presets. Plain C++ (no Qt): AntennaScene runs it on worker
// threads, the tests drive it directly.

#include "qsig/vec3.h"

#include <complex>
#include <string>
#include <vector>

namespace qsig::app
{
// A geometric relation of the designer. The element that holds the constraint is the one that moves,
// turns or changes length to meet it; the element it refers to (its reference) is left alone.
struct AntennaConstraint
{
    enum class Type
    {
        FixPosition,   // the centre keeps the coordinates of 'fixed' on the axes of 'axes'
        Length,        // length = value [m]
        EqualLength,   // same length as the reference
        Parallel,      // axis parallel to that of the reference
        Perpendicular, // axis at a right angle to that of the reference
        Angle,         // axis at 'value' degrees (0 to 180) from that of the reference
        Coincident,    // 'anchor' of the element on 'referenceAnchor' of the reference
        Distance,      // 'anchor' at 'value' metres from 'referenceAnchor' of the reference
    };
    Type type = Type::FixPosition;
    int reference = 0; // id of the reference element (none for FixPosition and Length)
    double value = 0.0;
    int anchor = 0, referenceAnchor = 0; // 0 centre, 1 first end, 2 second end of the wire
    int axes = 7;                        // FixPosition: bit 0 x, bit 1 y, bit 2 z
    qsig::Vec3 fixed;                    // FixPosition: the position kept
    bool enabled = true;
    bool operator==(const AntennaConstraint &) const = default;
};

// A source: a voltage across a gap of a wire. The first source of an element is described by the element
// itself (fed, feedFraction, strength, phaseDeg); a plain wire may carry further ones, listed in
// 'moreFeeds'. The functions feedCount, feedAt, setFeedAt, addFeed and removeFeed present them
// all alike, as entities placed on their wire.
struct AntennaFeed
{
    std::string name;      // empty: named after its element
    double fraction = 0.5; // place along the wire, 0..1; at 0 or 1, at that end, between the wire and the surface it stands on
    double strength = 1.0; // [V]
    double phaseDeg = 0.0;
    bool operator==(const AntennaFeed &) const = default;
};

struct AntennaElement
{
    enum class Type
    {
        Wire,            // straight wire, passive (parasitic element) or fed somewhere along it
        Dipole,          // two arms fed at the centre; an apex angle below 180 degrees makes a V
        FoldedDipole,    // two parallel wires joined at the ends, fed at the centre of one of them
        CircularLoop,    // regular polygon, fed at its first vertex
        RectangularLoop, // fed at the middle of its first side
        Helix,           // fed at its middle (normal mode) or at its foot on a ground plane (axial mode)
        Plate,           // conducting rectangle across the axis: ground plane, reflector
        Disc,            // conducting disc across the axis
        // Conducting solids, closed skins centred on 'position'.
        Box,      // 'width' and 'length' across the axis as a plate, 'height' along it
        Cylinder, // 'radius', 'height' along the axis
        Cone,     // base of 'radius' at the foot of the axis, apex at its tip, 'height' between them
        Sphere,   // 'radius'
        // Curved conducting surfaces, open. 'position' is the vertex, the axis the direction of the opening.
        Paraboloid,        // dish: aperture of 'radius', focal length 'focalLength'
        ParabolicCylinder, // aperture 'width' across (along e2), 'length' along the focal line (e3)
        // Curved wires, in the plane across the axis.
        Arc,    // arc of a circle of 'radius' centred on 'position', 'sweepDeg' wide; passive, or one source along it
        Spiral, // two-arm Archimedean spiral from 'innerRadius' to 'radius' in 'turns', fed at its centre
    };

    std::string name;
    Type type = Type::Dipole;
    qsig::Vec3 position;   // centre [m]
    double strength = 1.0; // source voltage [V] (fed elements)

    // Orientation: axis direction (azimuth from +x in the xy plane, elevation above it) and a
    // rotation about that axis. Wires and dipoles lie along the axis; loops and helices turn around it.
    double azimuthDeg = 0.0, elevationDeg = 90.0, rollDeg = 0.0;
    double length = 0.5;      // wire, dipole, folded dipole; height of a rectangular loop or plate [m]
    double width = 0.02;      // spacing of a folded dipole, width of a rectangular loop or plate [m]
    double radius = 0.16;     // circular loop, helix, disc, cylinder, cone, sphere [m]
    double height = 0.2;      // box, cylinder, cone: size along the axis [m]
    double focalLength = 0.5; // paraboloid, parabolic cylinder: from the vertex to the focus [m]
    double sweepDeg = 180.0;  // arc: angle it spans
    double innerRadius = 0.01; // spiral: where its arms start [m]
    // What a solid is made of. 0: a perfect conductor (its skin alone carries currents). 1: a
    // homogeneous dielectric of relative permittivity 'permittivity' and loss tangent 'lossTangent'
    // (the field goes through it). Ignored for the other types.
    int medium = 0;
    double permittivity = 4.0;
    double lossTangent = 0.0;
    double apexDeg = 180.0;   // angle between the arms of a dipole
    double pitch = 0.22;      // helix advance per turn [m]
    double turns = 5.0;       // helix
    int sides = 24;           // polygon of a circular loop, segments per helix turn
    double wireRadius = 1e-3; // [m]
    int fed = 1;              // 0: passive (parasitic) element
    double phaseDeg = 0.0;    // source phase
    // Feed position along a plain wire or a helix, 0..1. At 0 or 1 the source is at that end, between
    // the wire and the surface the end stands on.
    double feedFraction = 0.5;
    // Further sources of a plain wire (ignored for the other types, and when the element is not fed).
    std::vector<AntennaFeed> moreFeeds;

    // Designer. 'id' follows the element through the list, whatever its row (0: not given yet): the
    // constraints refer to it. Elements with the same group name are selected and transformed together.
    int id = 0;
    std::string group;
    std::vector<AntennaConstraint> constraints;
    bool operator==(const AntennaElement &) const = default;
};

struct AntennaSettings
{
    double frequency = 300e6;          // [Hz]
    double referenceOhm = 50.0;        // for the reflection coefficient, return loss and VSWR
    double segmentsPerWavelength = 100; // MoM mesh of the wires
    double facetsPerWavelength = 10;    // MoM mesh of the surfaces: triangle sides per wavelength
    double sweepStart = 200e6, sweepStop = 400e6;
    int sweepPoints = 61;
    bool operator==(const AntennaSettings &) const = default;
};

// Drawing of an element: polylines through its wire, and its feed point when fed. For a surface, the
// one polyline is its closed outline.
struct AntennaShape
{
    std::vector<std::vector<qsig::Vec3>> polylines;
    std::vector<qsig::Vec3> feeds;
    bool surface = false;
    // Surfaces and solids: the skin as closed polygons, for filling it and for picking it by its inside
    // (one for a plate or a disc; the six faces of a box; a coarse faceting of a curved solid).
    std::vector<std::vector<qsig::Vec3>> faces;
    // Points to mark in the view: the focus of a paraboloid, the focal line of a parabolic cylinder.
    std::vector<qsig::Vec3> marks;
};
AntennaShape antennaShape(const AntennaElement &element);
// Plates, discs and solids: conducting skins, never fed themselves.
bool elementIsSurface(AntennaElement::Type type);
// A conducting solid (box, cylinder, cone, sphere): a closed skin. It is a surface for everything
// said of surfaces (no source, a wire that lands on it is attached), so elementIsSurface is true too.
bool elementIsSolid(AntennaElement::Type type);
// A solid made of a dielectric. No wire is attached to it and no source stands on it; a conductor
// may not touch it yet.
bool elementIsDielectric(const AntennaElement &element);
// Row of the surface or solid on whose skin 'point' lies (within 'tolerance'), or -1.
int surfaceAt(const std::vector<AntennaElement> &elements, const qsig::Vec3 &point, double tolerance = 1e-6);
// The point of the plane of the surface under 'point' (its orthogonal projection); for a solid, the
// point of its skin nearest to 'point'.
qsig::Vec3 projectOnSurface(const AntennaElement &surface, const qsig::Vec3 &point);
// Unit vector of the element axis.
qsig::Vec3 antennaAxis(const AntennaElement &element);

// Drawing wires with the mouse (the designer). A plain wire (Type::Wire) runs from its first end
// 'a' to 'b' = a + length x axis. The solver joins the wires that touch: end to end, an end on the
// body of another wire (T) or two bodies crossing (X). A source cannot sit on such a node.
void wireEndpoints(const AntennaElement &wire, qsig::Vec3 &a, qsig::Vec3 &b);
// Lays the wire between two points (position, azimuth, elevation, length). False, and nothing
// changed, when they are closer than ten wire radii: the thin-wire solver has no use for such a stub.
bool setWireEndpoints(AntennaElement &wire, const qsig::Vec3 &a, const qsig::Vec3 &b);
// Where along the wire (0 at 'a', 1 at 'b') the point nearest to 'p' lies.
double wireFraction(const AntennaElement &wire, const qsig::Vec3 &p);
// Whether a source can be slid along the element (a plain wire, an arc), and where along it (0 to 1) the
// point nearest to 'p' lies.
bool feedSlides(AntennaElement::Type type);
double curveFraction(const AntennaElement &element, const qsig::Vec3 &p);
// Points the designer snaps to: the ends of every polyline and, with 'feeds', the sources (to measure
// from them; a wire must not end on a source). 'exclude': an element to leave out (the wire being
// reshaped).
std::vector<qsig::Vec3> antennaAnchors(const std::vector<AntennaElement> &elements, int exclude = -1, bool feeds = true);

// Transformations of the designer. The orientation follows: azimuth, elevation and roll are those of
// the turned or mirrored element.
// Rotation by 'angleDeg' (right-hand rule) about the line through 'pivot' along 'axis'.
void rotateElement(AntennaElement &element, const qsig::Vec3 &pivot, const qsig::Vec3 &axis, double angleDeg);
// Mirror image in the plane through 'point' with the normal 'normal': every point of the wire and the
// source go to their images. Exception: a helix keeps its winding sense (the model only has
// right-handed helices), its axis and position are mirrored.
void mirrorElement(AntennaElement &element, const qsig::Vec3 &point, const qsig::Vec3 &normal);
// Centre of the box that holds the wires of the elements at 'rows': the pivot of a selection.
qsig::Vec3 elementsCentre(const std::vector<AntennaElement> &elements, const std::vector<int> &rows);

// Constraints.
// Gives an id to the elements that have none. An element whose id is already taken is a copy: it gets
// a new id and neither the constraints nor the group of its original.
void ensureElementIds(std::vector<AntennaElement> &elements);
// Removes the constraints whose reference is no longer in the list (or is the element itself).
void pruneConstraints(std::vector<AntennaElement> &elements);
// Point of an element: 0 its centre, 1 and 2 the two ends of its wire (one point for a closed loop).
qsig::Vec3 elementAnchor(const AntennaElement &element, int anchor);
// Elements whose length is the 'length' field: wire, dipole, folded dipole, rectangular loop (height).
bool elementHasLength(AntennaElement::Type type);
bool constraintNeedsReference(AntennaConstraint::Type type);
// How far a constraint of 'element' is from being met: metres for positions, lengths and distances,
// radians for directions. 0 when it cannot apply (no length, reference missing).
double constraintError(const std::vector<AntennaElement> &elements, const AntennaElement &element,
                       const AntennaConstraint &constraint);
bool constraintMet(const std::vector<AntennaElement> &elements, const AntennaElement &element,
                   const AntennaConstraint &constraint);
// Applies the enabled constraints, element after element, until nothing moves (a constraint may undo
// what another one did: a few passes). Returns how many remain unmet: they contradict each other.
int applyConstraints(std::vector<AntennaElement> &elements);

// Cuts a plain wire at 'fraction' of its length (0 at its first end): 'first' is the part from the
// first end, 'second' the rest; the source stays on the part that holds it. The two parts touch, so
// the antenna is the same until one of them is moved or removed. False when the element is not a
// plain wire or when a part would be shorter than ten wire radii.
bool splitWire(const AntennaElement &wire, double fraction, AntennaElement &first, AntennaElement &second);

// Sources of an element, as entities placed on its wire. Number of sources (0 for a passive element or a
// surface); the source of index k (0: the first one); whether the element may take one more (a plain wire
// any number, the other wire elements one, at the place their type gives it).
int feedCount(const AntennaElement &element);
AntennaFeed feedAt(const AntennaElement &element, int index);
bool canAddFeed(const AntennaElement &element);
// Changes the source of index k; adds a source (its index, or -1 when refused); removes one (the next
// one becomes the first, or the element becomes passive). False for an index out of range.
bool setFeedAt(AntennaElement &element, int index, const AntennaFeed &source);
int addFeed(AntennaElement &element, const AntennaFeed &source);
bool removeFeed(AntennaElement &element, int index);

struct AntennaSegment
{
    qsig::Vec3 p0, p1;
    std::complex<double> i0, i1; // current at both ends [A], along p0 -> p1
    int element = -1;
};

// A triangle of a solved surface, with the modulus of its current density at three inner points, each
// near one corner (a, b, c) [A/m].
struct AntennaFacet
{
    qsig::Vec3 a, b, c;
    double ja = 0.0, jb = 0.0, jc = 0.0;
    int element = -1;
};

struct AntennaFeedResult
{
    std::string name;
    int element = -1;
    int feedIndex = 0; // index of the source on its element (0: the first one)
    std::complex<double> voltage, current, impedance, reflection;
    double returnLossDb = 0.0, vswr = 1.0;
    double powerW = 0.0; // delivered by this source
};

struct PatternCut
{
    std::vector<double> angleDeg; // 0..360
    std::vector<double> gainDbi;
};

struct AntennaResult
{
    bool valid = false;
    std::string error, note;
    double frequency = 0.0, wavelength = 0.0;
    int unknowns = 0;
    double segmentsPerWavelength = 0.0; // used (lowered when a segment would be shorter than 2 radii)
    double milliseconds = 0.0;
    std::vector<AntennaSegment> segments;
    double maxCurrent = 0.0;
    std::vector<AntennaFacet> facets; // scenes with surfaces
    double maxSurfaceCurrent = 0.0;
    std::vector<AntennaFeedResult> feeds;

    // Far field (lossless conductors: gain = directivity).
    bool hasPattern = false;
    double directivityDbi = 0.0;
    double radiatedPowerW = 0.0, acceptedPowerW = 0.0;
    double mainThetaDeg = 0.0, mainPhiDeg = 0.0; // theta from +z, phi from +x
    double frontToBackDb = 0.0;
    // Polarisation in the direction of the main lobe: ratio of the axes of the ellipse (0 dB: circular,
    // 40 dB and more: linear) and sense of rotation seen along the propagation (+1 right-hand, -1 left-hand).
    double axialRatioDb = 0.0;
    int polarisationSense = 0;
    // Gain on a theta-major grid: gainDbi[t * phiDeg.size() + p].
    std::vector<double> thetaDeg, phiDeg, gainDbi;
    PatternCut horizontal; // theta = 90 degrees, angle = phi
    PatternCut vertical;   // plane of the main lobe (phi = mainPhi), angle = theta over the full circle
    PatternCut vertical90; // perpendicular vertical plane (phi = mainPhi + 90)
};

struct AntennaSolveOptions
{
    bool pattern = true;
    double gridStepDeg = 3.0; // 3D grid and power integration
    double cutStepDeg = 1.0;
};

AntennaResult solveAntenna(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                           const AntennaSolveOptions &options = {});

// One point of a frequency sweep: first source, gain and front to back ratio on a coarse grid.
struct SweepPoint
{
    double frequency = 0.0;
    bool valid = false;
    std::complex<double> impedance, reflection;
    double returnLossDb = 0.0, vswr = 1.0;
    double gainDbi = 0.0, frontToBackDb = 0.0;
};
SweepPoint solveSweepPoint(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                           double frequency);
std::vector<double> sweepFrequencies(const AntennaSettings &settings);

// Admittance matrix between the sources: Y[i][j] is the current of source i when source j has 1 V
// and the others 0 V (one solve per source). Empty if a solve fails.
std::vector<std::vector<std::complex<double>>> admittanceMatrix(const std::vector<AntennaElement> &elements,
                                                                const AntennaSettings &settings);

struct AntennaPresetInfo
{
    const char *key;
    const char *name;
};
const std::vector<AntennaPresetInfo> &antennaPresets();
// Elements of a preset, dimensioned for 'frequency'. Empty for an unknown key.
std::vector<AntennaElement> antennaPreset(const std::string &key, double frequency);
} // namespace qsig::app
