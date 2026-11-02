#include "render.h"

#include <algorithm>

#include <QRegularExpression>

namespace iris {
namespace {

bool applyFilter(QColor* c, const QString& name, const QString& arg) {
    const double a = arg.toDouble();
    float h = 0, s = 0, l = 0, al = 1;
    c->getHslF(&h, &s, &l, &al);
    if (h < 0) h = 0;

    if (name == QLatin1String("lighten")) {
        c->setHslF(h, s, std::clamp(float(l + a / 100.0), 0.0f, 1.0f), al);
        return true;
    }
    if (name == QLatin1String("darken")) {
        c->setHslF(h, s, std::clamp(float(l - a / 100.0), 0.0f, 1.0f), al);
        return true;
    }
    if (name == QLatin1String("set_alpha")) {
        c->setAlphaF(std::clamp(a, 0.0, 1.0));
        return true;
    }
    if (name == QLatin1String("grayscale")) {
        c->setHslF(h, 0.0f, l, al);
        return true;
    }
    if (name == QLatin1String("invert")) {
        *c = QColor(255 - c->red(), 255 - c->green(), 255 - c->blue(), c->alpha());
        return true;
    }
    return false;
}

QString evalToken(const QString& expr, const ThemeData& d, const QString& defMode, QString* warn) {
    QStringList segs = expr.split('|');
    const QString path = segs.takeFirst().trimmed();

    if (path == QLatin1String("mode")) return defMode;
    if (path == QLatin1String("image")) return d.image;

    const QStringList parts = path.split('.');
    if (parts.size() != 4) {
        *warn = QStringLiteral("unknown value \"%1\"").arg(path);
        return {};
    }
    const QString fmt = parts[3];

    QColor c;
    if (parts[0] == QLatin1String("colors")) {
        const QString which = parts[2] == QLatin1String("default") ? defMode : parts[2];
        const QMap<QString, QRgb>& map = which == QLatin1String("light") ? d.light : d.dark;
        if (!map.contains(parts[1])) {
            *warn = QStringLiteral("unknown color \"%1\"").arg(parts[1]);
            return {};
        }
        c = QColor(map.value(parts[1]));
    } else if (parts[0] == QLatin1String("palettes")) {
        const auto pal = d.palettes.constFind(parts[1]);
        const int tone = parts[2].toInt();
        if (pal == d.palettes.constEnd() || !pal->contains(tone)) {
            *warn = QStringLiteral("unknown palette entry \"%1.%2\"").arg(parts[1], parts[2]);
            return {};
        }
        c = QColor(pal->value(tone));
    } else {
        *warn = QStringLiteral("unknown value \"%1\"").arg(path);
        return {};
    }

    for (const QString& f : std::as_const(segs)) {
        const QString filter = f.trimmed();
        const int colon = filter.indexOf(':');
        const QString name = (colon < 0 ? filter : filter.left(colon)).trimmed();
        const QString arg = colon < 0 ? QString() : filter.mid(colon + 1).trimmed();
        if (!applyFilter(&c, name, arg)) {
            *warn = QStringLiteral("unknown filter \"%1\"").arg(name);
            return {};
        }
    }

    bool ok = false;
    const QString out = formatColor(c, fmt, &ok);
    if (!ok) {
        *warn = QStringLiteral("unknown format \"%1\"").arg(fmt);
        return {};
    }
    return out;
}

}  // namespace

QString formatColor(const QColor& c, const QString& f, bool* ok) {
    *ok = true;
    const int h = std::max(0, c.hslHue());
    const int s = qRound(c.hslSaturationF() * 100.0);
    const int l = qRound(c.lightnessF() * 100.0);
    const QString alpha = QString::number(c.alphaF(), 'g', 3);
    const QString rgb = QString::number(c.red()) + ", " + QString::number(c.green()) + ", " + QString::number(c.blue());
    const QString hsl = QString::number(h) + ", " + QString::number(s) + "%, " + QString::number(l) + "%";

    if (f == QLatin1String("hex")) return c.name();
    if (f == QLatin1String("hex_stripped")) return c.name().mid(1);
    if (f == QLatin1String("rgb")) return "rgb(" + rgb + ")";
    if (f == QLatin1String("rgba")) return "rgba(" + rgb + ", " + alpha + ")";
    if (f == QLatin1String("hsl")) return "hsl(" + hsl + ")";
    if (f == QLatin1String("hsla")) return "hsla(" + hsl + ", " + alpha + ")";
    if (f == QLatin1String("red")) return QString::number(c.red());
    if (f == QLatin1String("green")) return QString::number(c.green());
    if (f == QLatin1String("blue")) return QString::number(c.blue());
    if (f == QLatin1String("alpha")) return alpha;
    if (f == QLatin1String("hue")) return QString::number(h);
    if (f == QLatin1String("saturation")) return QString::number(s);
    if (f == QLatin1String("lightness")) return QString::number(l);

    *ok = false;
    return {};
}

QString renderTemplate(const QString& text, const ThemeData& data, const QString& defaultMode,
                       QStringList* warnings) {
    static const QRegularExpression re(QStringLiteral("\\{\\{\\s*(.*?)\\s*\\}\\}"));

    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        last = m.capturedEnd();

        QString warn;
        const QString value = evalToken(m.captured(1), data, defaultMode, &warn);
        if (warn.isEmpty()) {
            out += value;
        } else {
            warnings->append(warn);
            out += m.captured(0);        // leave it visible in the output
        }
    }
    out += text.mid(last);
    return out;
}

}  // namespace iris