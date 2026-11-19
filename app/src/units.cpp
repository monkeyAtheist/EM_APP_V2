#include "qsig/app/units.h"

#include "qsig/constants.h"
#include "qsig/em_units_calculator_model.h"
#include "unit_prefixes.h"

#include <array>
#include <cmath>
#include <set>

namespace qsig::app
{
namespace
{
struct Prefix
{
    double factor;
    const char *symbol;
};
constexpr std::array<Prefix, 10> kPrefixes{{{1e12, "T"},
                                            {1e9, "G"},
                                            {1e6, "M"},
                                            {1e3, "k"},
                                            {1.0, ""},
                                            {1e-3, "m"},
                                            {1e-6, "µ"},
                                            {1e-9, "n"},
                                            {1e-12, "p"},
                                            {1e-15, "f"}}};

bool isAngle(const QString &unit)
{
    return unit == QStringLiteral("°");
}

// Logarithmic ratios take no prefix ("-0,5 dB", never "-500 mdB"); dBi: gain over the isotropic antenna.
bool isDecibel(const QString &unit)
{
    return unit == QStringLiteral("dB") || unit == QStringLiteral("dBi");
}

const Prefix &choosePrefix(double value)
{
    const double a = std::abs(value);
    for (const auto &p : kPrefixes)
        if (a >= p.factor * 0.9995) return p;
    return kPrefixes.back();
}

// Spelling understood by the calculator's parser.
QString parserText(QString text)
{
    text = text.trimmed();
    text.replace(QLatin1Char(','), QLatin1Char('.'));
    text.replace(QStringLiteral("²"), QStringLiteral("^2"));
    text.replace(QStringLiteral("³"), QStringLiteral("^3"));
    text.replace(QStringLiteral("°"), QStringLiteral(" deg"));
    text.replace(QStringLiteral("−"), QStringLiteral("-"));
    text.replace(QStringLiteral("·"), QStringLiteral("*"));
    return text;
}

EmCalculator::QuantityResult evaluate(const QString &text)
{
    static const std::set<std::string> known = []
    {
        const auto symbols = EmCalculator::supportedUnitSymbols();
        return std::set<std::string>(symbols.begin(), symbols.end());
    }();
    return EmCalculator::evaluateQuantity(detail::expandUnitPrefixes(parserText(text).toStdString(), known));
}

bool dimensionless(const EmCalculator::Quantity &q)
{
    for (double d : q.dimensions)
        if (std::abs(d) > 1e-9) return false;
    return true;
}

// Areas and volumes: a prefix applies to the metre before the power, so they get their own units.
struct PowerUnit
{
    double factor;
    const char *symbol;
};

const PowerUnit &powerUnit(double value, const QString &unit)
{
    static constexpr PowerUnit areas[] = {{1.0, "m²"}, {1e-4, "cm²"}, {1e-6, "mm²"}};
    static constexpr PowerUnit volumes[] = {{1.0, "m³"}, {1e-6, "cm³"}, {1e-9, "mm³"}};
    const bool volume = unit == QStringLiteral("m³");
    const PowerUnit *table = volume ? volumes : areas;
    // Keep m² down to 0.01 m² and m³ down to 0.001 m³ (a litre), then switch to the smaller units.
    const double a = std::abs(value);
    if (a >= (volume ? 1e-3 : 1e-2) || a == 0.0) return table[0];
    if (a >= table[1].factor * 0.9995) return table[1];
    return table[2];
}

bool isPowerUnit(const QString &unit)
{
    return unit == QStringLiteral("m²") || unit == QStringLiteral("m³");
}
} // namespace

QString Units::formatQuantity(double siValue, const QString &unit, int significantDigits)
{
    if (!std::isfinite(siValue)) return QStringLiteral("—");
    if (unit.isEmpty()) // a pure number (number of turns...): no prefix
    {
        QString number = QString::number(siValue, 'g', significantDigits);
        number.replace(QLatin1Char('.'), QLatin1Char(','));
        number.replace(QLatin1Char('-'), QStringLiteral("−"));
        return number;
    }
    if (isDecibel(unit))
    {
        QString number = QString::number(siValue, 'f', 2);
        number.replace(QLatin1Char('.'), QLatin1Char(','));
        number.replace(QLatin1Char('-'), QStringLiteral("−"));
        return number + QLatin1Char(' ') + unit;
    }
    if (isPowerUnit(unit))
    {
        const PowerUnit &u = powerUnit(siValue, unit);
        QString number = QString::number(siValue / u.factor, 'g', significantDigits);
        number.replace(QLatin1Char('.'), QLatin1Char(','));
        number.replace(QLatin1Char('-'), QStringLiteral("−"));
        return number + QLatin1Char(' ') + QString::fromUtf8(u.symbol);
    }
    QString number;
    QString prefix;
    const bool percent = unit == QStringLiteral("%"); // a ratio: no prefix ("0,5 %", never "500 m%")
    if (isAngle(unit) || percent || siValue == 0.0)
    {
        number = QString::number(siValue, 'g', significantDigits);
    }
    else
    {
        const Prefix &p = choosePrefix(siValue);
        number = QString::number(siValue / p.factor, 'g', significantDigits);
        prefix = QString::fromUtf8(p.symbol);
    }
    number.replace(QLatin1Char('.'), QLatin1Char(','));
    number.replace(QLatin1Char('-'), QStringLiteral("−"));
    return isAngle(unit) ? number + unit : number + QLatin1Char(' ') + prefix + unit;
}

QString Units::preferredUnitFor(double siValue, const QString &unit)
{
    if (unit.isEmpty()) return {};
    if (isPowerUnit(unit)) return QString::fromUtf8(powerUnit(siValue, unit).symbol);
    if (isAngle(unit) || isDecibel(unit) || unit == QStringLiteral("%") || siValue == 0.0) return unit;
    return QString::fromUtf8(choosePrefix(siValue).symbol) + unit;
}

QVariantMap Units::parseQuantity(const QString &text, const QString &unit, const QString &defaultUnit)
{
    auto failure = [](const QString &message)
    { return QVariantMap{{"ok", false}, {"value", 0.0}, {"error", message}}; };
    if (text.trimmed().isEmpty()) return failure(tr("Valeur vide"));
    auto result = evaluate(text);
    if (!result.ok) return failure(tr("Expression invalide : %1").arg(QString::fromStdString(result.error)));

    if (unit.isEmpty())
    {
        if (!dimensionless(result.quantity)) return failure(tr("Un nombre sans unité est attendu"));
        return {{"ok", true}, {"value", result.quantity.siValue}, {"error", QString()}};
    }
    if (isAngle(unit))
    {
        if (!dimensionless(result.quantity)) return failure(tr("Un angle est attendu"));
        const bool explicitUnit = text.contains(QStringLiteral("deg")) || text.contains(QStringLiteral("rad")) ||
                                  text.contains(QStringLiteral("°"));
        const double degrees = explicitUnit ? constants::radToDeg(result.quantity.siValue) : result.quantity.siValue;
        return {{"ok", true}, {"value", degrees}, {"error", QString()}};
    }

    if (dimensionless(result.quantity))
    {
        // The parser has no implicit product between ")" and a unit, hence "(x)*(1 unit)".
        result = evaluate(QStringLiteral("(%1)*(1 %2)").arg(text, defaultUnit.isEmpty() ? unit : defaultUnit));
        if (!result.ok) return failure(tr("Expression invalide : %1").arg(QString::fromStdString(result.error)));
    }
    const auto expected = evaluate(QStringLiteral("1 %1").arg(unit));
    if (expected.ok)
        for (std::size_t i = 0; i < expected.quantity.dimensions.size(); ++i)
            if (std::abs(expected.quantity.dimensions[i] - result.quantity.dimensions[i]) > 1e-9)
                return failure(tr("Unité incompatible : %1 attendu").arg(unit));
    if (!std::isfinite(result.quantity.siValue)) return failure(tr("Valeur non finie"));
    return {{"ok", true}, {"value", result.quantity.siValue}, {"error", QString()}};
}
} // namespace qsig::app
