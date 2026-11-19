#pragma once

// Optimisation of a Yagi-Uda array (straight parallel elements along a boom): the element lengths
// and the spacings, searched by a bounded Nelder-Mead simplex on a score that combines the gain, the
// front-to-back ratio and the match of the first source to the reference impedance. Each trial is a
// full MoM solve at the analysis frequency (coarse pattern grid, the main lobe refined). Plain C++:
// AntennaScene runs it on a worker thread, the tests drive it directly.

#include "qsig/app/antenna_model.h"

#include <functional>
#include <string>
#include <vector>

namespace qsig::app
{
struct YagiGoal
{
    enum class Kind
    {
        Gain,        // maximum gain, the back lobe barely weighted
        FrontToBack, // maximum front-to-back ratio, the gain kept in view
        Balanced,    // gain first, a fair share of front-to-back ratio
    };
    Kind kind = Kind::Balanced;
    bool match = true; // penalise a return loss under 10 dB (VSWR above 1.9) at the reference impedance

    // Score weights, from the kind (see yagiGoal()).
    double gainWeight = 1.0, frontToBackWeight = 0.25, matchWeight = 2.0;
    double frontToBackCapDb = 30.0;    // no reward beyond: a deep null is narrow-band and fragile
    double returnLossTargetDb = 10.0; // [dB]
};
YagiGoal yagiGoal(YagiGoal::Kind kind, bool match);

struct YagiOptimizerOptions
{
    YagiGoal goal;
    int maxEvaluations = 240;
    bool lengths = true, spacings = true;
    double minLengthLambda = 0.30, maxLengthLambda = 0.60;
    double minSpacingLambda = 0.05, maxSpacingLambda = 0.50;
};

// Score of one solved point (higher is better).
double yagiScore(const SweepPoint &point, const YagiGoal &goal);

struct YagiOptimizerResult
{
    bool valid = false;
    std::string error;
    bool cancelled = false;
    bool improved = false;                // the elements differ from the input
    std::vector<AntennaElement> elements; // best design found (the input when nothing was better)
    SweepPoint before, after;
    double scoreBefore = 0.0, scoreAfter = 0.0;
    int evaluations = 0, variables = 0; // solves of the search (the start and the result are solved again)
    double milliseconds = 0.0;
};

// Called after every evaluation; returning false stops the search (the best design so far is kept).
using YagiProgress = std::function<bool(int evaluation, int budget, const SweepPoint &best)>;

// Variables: the length of every straight element (wire, dipole, folded dipole), and the spacings
// between neighbours when the elements stand on one line (the first fed element stays in place).
YagiOptimizerResult optimiseYagi(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                                 const YagiOptimizerOptions &options = {}, const YagiProgress &progress = {});
} // namespace qsig::app
