#pragma once

// Physical quantities as text, for the input fields: engineering formatting with a French decimal
// comma, and parsing through the calculator's unit-aware evaluator ("2 nC", "10 cm", "3,5 kV").

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace qsig::app
{
class Units : public QObject
{
    Q_OBJECT

public:
    explicit Units(QObject *parent = nullptr) : QObject(parent) {}

    // unit: an SI unit as shown to the user ("C", "m", "C/m", "C/m²", "V", "V/m", "°").
    static QString formatQuantity(double siValue, const QString &unit, int significantDigits = 4);
    // Returns {ok: bool, value: double (SI), error: string}. A bare number is read in
    // 'defaultUnit' (for example "nC"); a value whose dimension differs from 'unit' is refused.
    static QVariantMap parseQuantity(const QString &text, const QString &unit, const QString &defaultUnit);
    // Unit the field shows by default for a value (for example 1e-9 C -> "nC").
    static QString preferredUnitFor(double siValue, const QString &unit);

    // QML entry points.
    Q_INVOKABLE QString format(double siValue, const QString &unit, int significantDigits = 4) const
    {
        return formatQuantity(siValue, unit, significantDigits);
    }
    Q_INVOKABLE QVariantMap parse(const QString &text, const QString &unit, const QString &defaultUnit) const
    {
        return parseQuantity(text, unit, defaultUnit);
    }
    Q_INVOKABLE QString preferredUnit(double siValue, const QString &unit) const
    {
        return preferredUnitFor(siValue, unit);
    }
};
} // namespace qsig::app
