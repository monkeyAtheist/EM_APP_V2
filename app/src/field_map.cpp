#include "qsig/app/field_map.h"

#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace qsig::app
{
using qsig::Vec3;

struct FieldMapComputer::Row
{
    std::vector<float> eu, ev, magnitude, scalar;
    std::vector<unsigned char> fieldValid, scalarValid;
};

namespace
{
struct Stop
{
    double t;
    int r, g, b;
};

QColor interpolate(const Stop *stops, int count, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    for (int i = 0; i + 1 < count; ++i)
    {
        if (t <= stops[i + 1].t)
        {
            const double f = (t - stops[i].t) / (stops[i + 1].t - stops[i].t);
            auto mix = [f](int a, int b) { return static_cast<int>(std::lround(a + (b - a) * f)); };
            return {mix(stops[i].r, stops[i + 1].r), mix(stops[i].g, stops[i + 1].g), mix(stops[i].b, stops[i + 1].b)};
        }
    }
    const Stop &last = stops[count - 1];
    return {last.r, last.g, last.b};
}

double percentile(std::vector<double> values, double q)
{
    if (values.empty()) return 0.0;
    const auto k = static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

// Marching squares on a scalar grid; 'valid' masks undefined samples.
void contours(const std::vector<double> &s, const std::vector<unsigned char> &valid, int nu, int nv,
              const std::vector<double> &levels, std::vector<QLineF> &out)
{
    auto at = [&](int c, int r) { return s[static_cast<std::size_t>(r * nu + c)]; };
    auto ok = [&](int c, int r) { return valid[static_cast<std::size_t>(r * nu + c)] != 0; };
    for (double level : levels)
        for (int r = 0; r + 1 < nv; ++r)
            for (int c = 0; c + 1 < nu; ++c)
            {
                if (!ok(c, r) || !ok(c + 1, r) || !ok(c + 1, r + 1) || !ok(c, r + 1)) continue;
                const double a = at(c, r), b = at(c + 1, r), d = at(c, r + 1), e = at(c + 1, r + 1);
                const int index = (a > level ? 1 : 0) | (b > level ? 2 : 0) | (e > level ? 4 : 0) | (d > level ? 8 : 0);
                if (index == 0 || index == 15) continue;
                auto lerp = [level](double x0, double x1) { return (level - x0) / (x1 - x0); };
                const QPointF top(c + lerp(a, b), r), right(c + 1, r + lerp(b, e));
                const QPointF bottom(c + lerp(d, e), r + 1), left(c, r + lerp(a, d));
                switch (index)
                {
                case 1:
                case 14:
                    out.emplace_back(left, top);
                    break;
                case 2:
                case 13:
                    out.emplace_back(top, right);
                    break;
                case 3:
                case 12:
                    out.emplace_back(left, right);
                    break;
                case 4:
                case 11:
                    out.emplace_back(right, bottom);
                    break;
                case 6:
                case 9:
                    out.emplace_back(top, bottom);
                    break;
                case 7:
                case 8:
                    out.emplace_back(left, bottom);
                    break;
                case 5:
                case 10:
                {
                    const bool centreHigh = 0.25 * (a + b + d + e) > level;
                    if ((index == 5) == centreHigh)
                    {
                        out.emplace_back(left, bottom);
                        out.emplace_back(top, right);
                    }
                    else
                    {
                        out.emplace_back(left, top);
                        out.emplace_back(right, bottom);
                    }
                    break;
                }
                default:
                    break;
                }
            }
}
} // namespace

QColor FieldMapComputer::sequentialColour(double t)
{
    static constexpr Stop stops[] = {{0.00, 0, 0, 4},      {0.14, 40, 11, 84},   {0.29, 101, 21, 110},
                                     {0.43, 159, 42, 99},  {0.57, 212, 72, 66},  {0.71, 245, 125, 21},
                                     {0.86, 250, 193, 39}, {1.00, 252, 255, 164}};
    return interpolate(stops, 8, t);
}

QColor FieldMapComputer::divergingColour(double t)
{
    static constexpr Stop stops[] = {{0.00, 5, 48, 97},     {0.10, 33, 102, 172},  {0.25, 67, 147, 195},
                                     {0.40, 146, 197, 222}, {0.50, 247, 247, 247}, {0.60, 244, 165, 130},
                                     {0.75, 214, 96, 77},   {0.90, 178, 24, 43},   {1.00, 103, 0, 31}};
    return interpolate(stops, 9, t);
}

QColor FieldMapComputer::mapColour(FieldQuantity quantity, double t, bool lightGround)
{
    if (quantity == FieldQuantity::Scalar)
    {
        if (lightGround) return divergingColour(t);
        // Dark ground: black at zero, cold below, warm above, lighter as the magnitude grows.
        static constexpr Stop stops[] = {{0.00, 190, 225, 255}, {0.15, 96, 170, 245}, {0.32, 40, 100, 200},
                                         {0.45, 20, 40, 84},    {0.50, 15, 17, 20},   {0.55, 84, 24, 26},
                                         {0.68, 200, 60, 44},   {0.85, 245, 140, 70}, {1.00, 255, 225, 185}};
        return interpolate(stops, 9, t);
    }
    if (!lightGround) return sequentialColour(t);
    // Light ground: white for the weakest field, then yellow, orange, red and violet for the strongest.
    static constexpr Stop stops[] = {{0.00, 250, 251, 252}, {0.14, 254, 240, 180}, {0.29, 253, 212, 110},
                                     {0.43, 250, 168, 60},  {0.57, 240, 116, 52},  {0.71, 213, 66, 75},
                                     {0.86, 150, 36, 110},  {1.00, 80, 18, 105}};
    return interpolate(stops, 8, t);
}

FieldMapComputer::FieldMapComputer(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<std::shared_ptr<Row>>::finished, this, &FieldMapComputer::onFinished);
}

FieldMapComputer::~FieldMapComputer()
{
    m_watcher.cancel();
    m_watcher.waitForFinished();
}

void FieldMapComputer::dropRunning()
{
    if (!m_active) return;
    m_dropRunning = true;
    m_watcher.cancel(); // the rows not started yet are skipped
}

void FieldMapComputer::cancel()
{
    m_hasPending = false;
    dropRunning();
}

void FieldMapComputer::request(FieldMapRequest request)
{
    if (request.columns <= 1 || request.rows <= 1 || !request.sampler) return;
    if (m_active)
    {
        // The running map still shows what the view asks for when only the sources or the window moved.
        const bool sameView = request.plane == m_running.plane && request.quantity == m_running.quantity &&
                              request.offset == m_running.offset && request.lightGround == m_running.lightGround;
        auto samples = [](const FieldMapRequest &q) { return static_cast<long long>(q.columns) * q.rows; };
        const bool muchCheaper = 2 * samples(request) <= samples(m_running);
        if (!sameView || muchCheaper) dropRunning();
        m_pending = std::move(request);
        m_hasPending = true;
        return;
    }
    start(std::move(request));
    emit busyChanged();
}

// Starts the computation of a request; none may be running.
void FieldMapComputer::start(FieldMapRequest request)
{
    m_active = true;
    m_dropRunning = false;
    m_running = std::move(request);
    auto shared = std::make_shared<const FieldMapRequest>(m_running);
    std::vector<int> rowIndices(static_cast<std::size_t>(m_running.rows));
    std::iota(rowIndices.begin(), rowIndices.end(), 0);
    m_clock.start();
    m_watcher.setFuture(QtConcurrent::mapped(
        std::move(rowIndices),
        [shared](int r)
        {
            const FieldMapRequest &q = *shared;
            auto row = std::make_shared<Row>();
            const auto n = static_cast<std::size_t>(q.columns);
            row->eu.resize(n);
            row->ev.resize(n);
            row->magnitude.resize(n);
            row->scalar.resize(n);
            row->fieldValid.resize(n);
            row->scalarValid.resize(n);
            const double v = q.v0 - r * q.du;
            for (int c = 0; c < q.columns; ++c)
            {
                const auto k = static_cast<std::size_t>(c);
                const FieldSample f = q.sampler(planeToWorld(q.plane, q.u0 + c * q.du, v, q.offset));
                double eu = 0.0, ev = 0.0, ew = 0.0;
                worldToPlane(q.plane, f.field, eu, ev, ew);
                const double magnitude = f.field.norm();
                row->eu[k] = static_cast<float>(eu);
                row->ev[k] = static_cast<float>(ev);
                row->magnitude[k] = static_cast<float>(magnitude);
                row->scalar[k] = static_cast<float>(f.scalar);
                row->fieldValid[k] = (f.fieldValid && std::isfinite(magnitude) && magnitude > 0.0) ? 1 : 0;
                row->scalarValid[k] = (f.scalarValid && std::isfinite(f.scalar)) ? 1 : 0;
            }
            return row;
        }));
}

void FieldMapComputer::onFinished()
{
    // What the finished computation holds, taken before the waiting request takes the watcher over.
    const bool dropped = m_dropRunning || m_watcher.isCanceled();
    const FieldMapRequest q = std::move(m_running);
    const QElapsedTimer clock = m_clock;
    QList<std::shared_ptr<Row>> rows;
    if (!dropped) rows = m_watcher.future().results();
    m_active = false;
    m_dropRunning = false;
    if (m_hasPending)
    {
        // The workers start on the latest request while this map is finished here: busy stays true.
        m_hasPending = false;
        start(std::move(m_pending));
    }
    const int nu = q.columns, nv = q.rows;
    const auto total = static_cast<std::size_t>(nu) * static_cast<std::size_t>(nv);
    if (dropped || static_cast<int>(rows.size()) != nv)
    {
        if (!m_active) emit busyChanged();
        return;
    }

    auto result = std::make_shared<FieldMapResult>();
    result->request = q;
    const bool potential = q.quantity == FieldQuantity::Scalar;
    result->logarithmic = !potential;
    result->scalarReferenced = q.scalarReferenced;

    std::vector<double> scalar(total, 0.0);
    std::vector<unsigned char> valid(total, 0);
    std::vector<double> samples;
    samples.reserve(total);
    for (int r = 0; r < nv; ++r)
        for (int c = 0; c < nu; ++c)
        {
            const Row &row = *rows[static_cast<std::size_t>(r)];
            const auto k = static_cast<std::size_t>(c);
            const auto i = static_cast<std::size_t>(r * nu + c);
            if (potential ? row.scalarValid[k] : row.fieldValid[k])
            {
                scalar[i] =
                    potential ? static_cast<double>(row.scalar[k]) : std::log10(static_cast<double>(row.magnitude[k]));
                valid[i] = 1;
                samples.push_back(potential ? std::abs(scalar[i]) : scalar[i]);
            }
        }
    result->validSamples = static_cast<int>(samples.size());

    if (!samples.empty())
    {
        if (potential)
        {
            const double limit = std::max(percentile(samples, 0.97), 1e-30);
            result->displayMin = -limit;
            result->displayMax = limit;
        }
        else
        {
            result->displayMin = percentile(samples, 0.02);
            result->displayMax = percentile(samples, 0.98);
            if (result->displayMax - result->displayMin < 1e-6) result->displayMax = result->displayMin + 1.0;
        }
    }
    const double span = result->displayMax - result->displayMin;

    result->image = QImage(nu, nv, QImage::Format_ARGB32);
    result->image.fill(Qt::transparent);
    for (int r = 0; r < nv; ++r)
    {
        auto *line = reinterpret_cast<QRgb *>(result->image.scanLine(r));
        for (int c = 0; c < nu; ++c)
        {
            const auto i = static_cast<std::size_t>(r * nu + c);
            if (!valid[i]) continue;
            const double t = (scalar[i] - result->displayMin) / span;
            line[c] = mapColour(q.quantity, t, q.lightGround).rgba();
        }
    }

    std::vector<double> levels;
    const int n = std::max(2, q.contourLevels);
    for (int k = 1; k < n; ++k) levels.push_back(result->displayMin + span * k / n);
    contours(scalar, valid, nu, nv, levels, result->contours);

    const int stride = std::max(2, nu / 26);
    for (int r = stride / 2; r < nv; r += stride)
        for (int c = stride / 2; c < nu; c += stride)
        {
            const Row &row = *rows[static_cast<std::size_t>(r)];
            const auto k = static_cast<std::size_t>(c);
            const double eu = static_cast<double>(row.eu[k]), ev = static_cast<double>(row.ev[k]);
            const double inPlane = std::hypot(eu, ev);
            if (!row.fieldValid[k] || !(inPlane > 0.0)) continue;
            const double logE = std::log10(static_cast<double>(row.magnitude[k]));
            FieldArrow arrow;
            arrow.at = QPointF(c, r);
            arrow.direction = QPointF(eu / inPlane, ev / inPlane);
            arrow.strength = potential ? 0.6 : std::clamp((logE - result->displayMin) / span, 0.0, 1.0);
            result->arrows.push_back(arrow);
        }

    if (q.lineSeparation > 0.0)
    {
        // Field lines follow the in-plane field: grid columns go along u, rows against v.
        std::vector<float> fc(total), fr(total);
        std::vector<unsigned char> lineValid(total);
        for (int r = 0; r < nv; ++r)
        {
            const Row &row = *rows[static_cast<std::size_t>(r)];
            for (int c = 0; c < nu; ++c)
            {
                const auto i = static_cast<std::size_t>(r * nu + c), k = static_cast<std::size_t>(c);
                fc[i] = row.eu[k];
                fr[i] = -row.ev[k];
                lineValid[i] = row.fieldValid[k];
            }
        }
        FieldLineSettings settings;
        settings.separation = q.lineSeparation;
        result->fieldLines = traceFieldLines(nu, nv, fc, fr, lineValid, settings);
    }

    result->elapsedMs = static_cast<double>(clock.nsecsElapsed()) / 1e6;
    if (!m_active) emit busyChanged();
    emit ready(result);
}
} // namespace qsig::app
