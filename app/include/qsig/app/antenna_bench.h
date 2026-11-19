#pragma once

// Validation bench of the "Antennes" module: the wire solver of the module (antenna_model.h, same
// mesh as the scene) against references that do not come from it. Each line states the quantity,
// the computed and the reference value, where the reference comes from, the deviation and the
// thresholds; a PASS means the deviation is within what the reference itself supports.
//   - Hallen's equation for the same thin wire, solved by an independent tool;
//   - closed forms of the electrically small antennas (Hertzian dipole, small loop);
//   - laws every solution must obey (power balance, reciprocity, mesh convergence).
// Plain C++: AntennaScene runs it on a worker thread, the tests drive it directly.

#include "qsig/app/antenna_model.h"

#include <functional>
#include <string>
#include <vector>

namespace qsig::app
{
struct AntennaBenchCase
{
    enum class Verdict
    {
        Pass,
        Warning,
        Fail,
        Error,
    };
    std::string group;    // antenna or law checked
    std::string quantity; // what is compared
    std::string unit;     // of value and reference ("" for a pure number)
    double value = 0.0, reference = 0.0;
    std::string source;   // where the reference comes from
    bool relative = true; // deviation in % of the reference, else absolute in 'unit'
    double deviation = 0.0;
    double passLimit = 0.0, warnLimit = 0.0;
    Verdict verdict = Verdict::Error;
    std::string note;
};

struct AntennaBenchReport
{
    std::vector<AntennaBenchCase> cases;
    int passed = 0, warnings = 0, failed = 0; // errors count as failed
    double segmentsPerWavelength = 0.0;
    double milliseconds = 0.0;
};

const char *benchVerdictName(AntennaBenchCase::Verdict verdict);
// Number of steps reported to the progress callback.
int antennaBenchSteps();
// Runs at 300 MHz (every case is given in wavelengths) with the mesh of 'settings'.
AntennaBenchReport runAntennaBench(const AntennaSettings &settings,
                                   const std::function<void(int done, int total)> &progress = {});
} // namespace qsig::app
