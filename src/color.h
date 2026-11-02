#pragma once

#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QRgb>
#include <QString>

namespace iris {

struct Cam {
    double hue = 0, chroma = 0, tone = 0;   // HCT
};

Cam fromRgb(QRgb rgb);                                // sRGB -> HCT
QRgb toRgb(double hue, double chroma, double tone);   // HCT -> sRGB (chroma is reduced if out of gamut)

struct Tonal {
    double hue = 0, chroma = 0;
    QRgb tone(double t) const;
};

struct PaletteSet {
    Tonal primary, secondary, tertiary, neutral, neutralVariant, error;
};

enum class Variant { TonalSpot, Expressive, Neutral, FruitSalad, Rainbow };
bool parseVariant(const QString& name, Variant* out);   // "expressive" or "scheme-expressive"
QString variantName(Variant v);                          // "scheme-expressive"

PaletteSet makePalettes(QRgb source, Variant variant);
QJsonObject buildScheme(QRgb source, Variant variant, bool dark);   // the md3 roles as #rrggbb

// ranked source color candidates for an image, best first
QList<QRgb> extractSources(const QImage& image, int limit = 8);

QString hex(QRgb color);

}  // namespace iris