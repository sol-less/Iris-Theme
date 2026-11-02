#pragma once

#include <QColor>
#include <QMap>
#include <QRgb>
#include <QString>
#include <QStringList>

namespace iris {

struct ThemeData {
    QMap<QString, QRgb> dark, light;               // role -> color
    QMap<QString, QMap<int, QRgb>> palettes;       // palette name -> tone -> color
    QString mode;                                  // selected mode: "dark" | "light"
    QString image;
};

// hex | hex_stripped | rgb | rgba | hsl | hsla | red | green | blue | alpha | hue | saturation | lightness
QString formatColor(const QColor& c, const QString& fmt, bool* ok);

// Replaces {{ ... }} in a Matugen-style template.
//   {{ colors.<role>.<default|dark|light>.<format> }}
//   {{ palettes.<primary|secondary|tertiary|neutral|neutral_variant|error>.<tone>.<format> }}
//   {{ mode }}  {{ image }}
//   filters:  | lighten: 10   | darken: 10   | set_alpha: 0.5   | grayscale   | invert
QString renderTemplate(const QString& text, const ThemeData& data, const QString& defaultMode,
                       QStringList* warnings);

}  // namespace iris