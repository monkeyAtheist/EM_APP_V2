#include "qsig/app/field_lines.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace qsig::app
{
namespace
{
class DirectionField
{
public:
    DirectionField(int columns, int rows, const std::vector<float> &fc, const std::vector<float> &fr,
                   const std::vector<unsigned char> &valid)
        : m_columns(columns), m_rows(rows), m_dc(fc.size(), 0.0), m_dr(fr.size(), 0.0), m_valid(valid)
    {
        // Unit vectors at the nodes: interpolating directions (not raw values) keeps lines smooth
        // across several orders of magnitude of the field.
        for (std::size_t i = 0; i < fc.size(); ++i)
        {
            const double c = static_cast<double>(fc[i]), r = static_cast<double>(fr[i]);
            const double n = std::hypot(c, r);
            if (m_valid[i] && n > 0.0 && std::isfinite(n))
            {
                m_dc[i] = c / n;
                m_dr[i] = r / n;
            }
            else
            {
                m_valid[i] = 0;
            }
        }
    }

    bool inside(const GridPoint &p) const
    {
        return p.c >= 0.0 && p.r >= 0.0 && p.c <= m_columns - 1.0 && p.r <= m_rows - 1.0;
    }

    // Unit direction at p, false where undefined (off the grid, next to a source, null point).
    bool direction(const GridPoint &p, double &dc, double &dr) const
    {
        if (!inside(p)) return false;
        const int c0 = std::min(static_cast<int>(p.c), m_columns - 2);
        const int r0 = std::min(static_cast<int>(p.r), m_rows - 2);
        const double fc = p.c - c0, fr = p.r - r0;
        double sc = 0.0, sr = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            const int c = c0 + (k & 1), r = r0 + (k >> 1);
            const auto i =
                static_cast<std::size_t>(r) * static_cast<std::size_t>(m_columns) + static_cast<std::size_t>(c);
            if (!m_valid[i]) return false;
            const double w = ((k & 1) ? fc : 1.0 - fc) * ((k >> 1) ? fr : 1.0 - fr);
            sc += w * m_dc[i];
            sr += w * m_dr[i];
        }
        const double n = std::hypot(sc, sr);
        if (n < 0.2) return false; // directions disagree: a source or a null point is in the cell
        dc = sc / n;
        dr = sr / n;
        return true;
    }

private:
    int m_columns, m_rows;
    std::vector<double> m_dc, m_dr;
    std::vector<unsigned char> m_valid;
};

// Points of the accepted lines, bucketed for distance queries.
class Occupancy
{
public:
    Occupancy(int columns, int rows, double cell)
        : m_cell(cell), m_width(static_cast<int>(columns / cell) + 1), m_height(static_cast<int>(rows / cell) + 1),
          m_buckets(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height))
    {
    }

    void add(const GridPoint &p) { bucket(p).push_back(p); }

    bool near(const GridPoint &p, double distance) const
    {
        const int bc = static_cast<int>(p.c / m_cell), br = static_cast<int>(p.r / m_cell);
        const int reach = static_cast<int>(std::ceil(distance / m_cell));
        const double d2 = distance * distance;
        for (int r = std::max(0, br - reach); r <= std::min(m_height - 1, br + reach); ++r)
            for (int c = std::max(0, bc - reach); c <= std::min(m_width - 1, bc + reach); ++c)
                for (const GridPoint &q : m_buckets[static_cast<std::size_t>(r * m_width + c)])
                    if ((q.c - p.c) * (q.c - p.c) + (q.r - p.r) * (q.r - p.r) < d2) return true;
        return false;
    }

private:
    std::vector<GridPoint> &bucket(const GridPoint &p)
    {
        const int c = std::clamp(static_cast<int>(p.c / m_cell), 0, m_width - 1);
        const int r = std::clamp(static_cast<int>(p.r / m_cell), 0, m_height - 1);
        return m_buckets[static_cast<std::size_t>(r * m_width + c)];
    }

    double m_cell;
    int m_width, m_height;
    std::vector<std::vector<GridPoint>> m_buckets;
};

// One half-line from the seed (sense +1 along the field, -1 against it). Second-order Runge-Kutta.
// 'closed' is set when the line comes back to its seed.
std::vector<GridPoint> integrate(const DirectionField &field, const Occupancy &occupied, GridPoint seed, double sense,
                                 const FieldLineSettings &s, double test, int maxSteps, bool &closed)
{
    std::vector<GridPoint> points{seed};
    closed = false;
    const double h = s.step;
    const auto minClosureSteps = static_cast<std::size_t>(std::ceil(2.0 * s.separation / h));
    GridPoint p = seed;
    for (int step = 0; step < maxSteps; ++step)
    {
        double c1 = 0.0, r1 = 0.0, c2 = 0.0, r2 = 0.0;
        if (!field.direction(p, c1, r1)) break;
        const GridPoint mid{p.c + 0.5 * h * sense * c1, p.r + 0.5 * h * sense * r1};
        if (!field.direction(mid, c2, r2)) break;
        const GridPoint next{p.c + h * sense * c2, p.r + h * sense * r2};
        if (!field.inside(next) || occupied.near(next, test)) break;
        points.push_back(next);
        p = next;
        if (points.size() > minClosureSteps && std::hypot(p.c - seed.c, p.r - seed.r) < 1.2 * h)
        {
            points.push_back(seed);
            closed = true;
            break;
        }
    }
    return points;
}
} // namespace

std::vector<FieldLine> traceFieldLines(int columns, int rows, const std::vector<float> &fc,
                                       const std::vector<float> &fr, const std::vector<unsigned char> &valid,
                                       const FieldLineSettings &settings)
{
    std::vector<FieldLine> lines;
    const auto n = static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows);
    if (columns < 3 || rows < 3 || fc.size() != n || fr.size() != n || valid.size() != n) return lines;
    const double sep = std::max(settings.separation, 2.0 * settings.step);
    const double test = 0.5 * sep; // a line stops when it comes this close to another one
    const DirectionField field(columns, rows, fc, fr, valid);
    Occupancy occupied(columns, rows, sep);
    const int maxSteps = static_cast<int>(4.0 * (columns + rows) / settings.step);

    std::deque<GridPoint> queue; // seeds next to accepted lines come first: they keep the spacing even
    double latticeC = 0.5 * sep, latticeR = 0.5 * sep;
    auto nextLatticeSeed = [&](GridPoint &seed)
    {
        if (latticeR > rows - 1.0) return false;
        seed = {latticeC, latticeR};
        latticeC += sep;
        if (latticeC > columns - 1.0)
        {
            latticeC = 0.5 * sep;
            latticeR += sep;
        }
        return true;
    };

    while (static_cast<int>(lines.size()) < settings.maxLines)
    {
        GridPoint seed;
        if (!queue.empty())
        {
            seed = queue.front();
            queue.pop_front();
        }
        else if (!nextLatticeSeed(seed))
        {
            break;
        }
        double dc = 0.0, dr = 0.0;
        if (!field.direction(seed, dc, dr) || occupied.near(seed, sep * 0.95)) continue;

        bool closed = false;
        std::vector<GridPoint> forward = integrate(field, occupied, seed, 1.0, settings, test, maxSteps, closed);
        FieldLine line;
        line.closed = closed;
        if (closed)
        {
            line.points = std::move(forward);
        }
        else
        {
            bool unused = false;
            std::vector<GridPoint> backward = integrate(field, occupied, seed, -1.0, settings, test, maxSteps, unused);
            line.points.assign(backward.rbegin(), backward.rend());
            line.points.insert(line.points.end(), forward.begin() + 1, forward.end());
        }
        if (static_cast<double>(line.points.size()) * settings.step < 2.0 * sep) continue; // too short to read

        for (const GridPoint &p : line.points) occupied.add(p);
        // Candidate seeds one separation away on both sides, every separation along the line.
        const auto every = std::max<std::size_t>(1, static_cast<std::size_t>(sep / settings.step));
        for (std::size_t i = 0; i + 1 < line.points.size(); i += every)
        {
            const GridPoint &a = line.points[i], &b = line.points[i + 1];
            const double len = std::hypot(b.c - a.c, b.r - a.r);
            if (len <= 0.0) continue;
            const double nc = -(b.r - a.r) / len, nr = (b.c - a.c) / len;
            queue.push_back({a.c + sep * nc, a.r + sep * nr});
            queue.push_back({a.c - sep * nc, a.r - sep * nr});
        }
        lines.push_back(std::move(line));
    }
    return lines;
}
} // namespace qsig::app
