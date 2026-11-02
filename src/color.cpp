#include "color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

#include <QColor>
#include <QHash>

namespace iris {
namespace {

const double kPi = 3.14159265358979323846;

double sanitize(double d) {
    d = std::fmod(d, 360.0);
    return d < 0 ? d + 360.0 : d;
}
double signum(double v) { return v < 0 ? -1.0 : (v == 0 ? 0.0 : 1.0); }

// ---------- sRGB <-> linear, L* <-> Y ----------
double linearize(int c8) {
    const double c = c8 / 255.0;
    return (c <= 0.040449936 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4)) * 100.0;
}
int delinearize(double v) {
    const double n = v / 100.0;
    const double c = n <= 0.0031308 ? n * 12.92 : 1.055 * std::pow(n, 1.0 / 2.4) - 0.055;
    return std::clamp(int(std::lround(c * 255.0)), 0, 255);
}
double yFromLstar(double l) {
    const double e = 216.0 / 24389.0, k = 24389.0 / 27.0;
    const double ft = (l + 16.0) / 116.0, ft3 = ft * ft * ft;
    return (ft3 > e ? ft3 : (116.0 * ft - 16.0) / k) * 100.0;
}
double lstarFromY(double y) {
    const double yn = y / 100.0, e = 216.0 / 24389.0, k = 24389.0 / 27.0;
    return yn <= e ? k * yn : 116.0 * std::cbrt(yn) - 16.0;
}
QRgb grayFromLstar(double l) {
    const int v = delinearize(yFromLstar(l));
    return qRgb(v, v, v);
}

// ---------- 3x3 matrices (inverses are computed, not typed in) ----------
using M3 = std::array<std::array<double, 3>, 3>;

M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

M3 inverse(const M3& m) {
    const double a = m[0][0], b = m[0][1], c = m[0][2];
    const double d = m[1][0], e = m[1][1], f = m[1][2];
    const double g = m[2][0], h = m[2][1], i = m[2][2];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    return {{
        {{(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det}},
        {{(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det}},
        {{(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det}},
    }};
}

const M3 kRgbToXyz = {{
    {{0.41233895, 0.35762064, 0.18051042}},
    {{0.2126, 0.7152, 0.0722}},
    {{0.01932141, 0.11916382, 0.95034478}},
}};
const M3 kXyzToCam = {{
    {{0.401288, 0.650173, -0.051461}},
    {{-0.250268, 1.204414, 0.045854}},
    {{-0.002079, 0.048952, 0.953127}},
}};

// CAM16 cone responses -> linear sRGB
const M3& camToLinear() {
    static const M3 m = mul(inverse(kRgbToXyz), inverse(kXyzToCam));
    return m;
}

// ---------- CAM16 viewing conditions (Material's defaults) ----------
struct VC {
    double n, aw, nbb, ncb, c, nc, fl, flRoot, z;
    double rgbD[3];
};

VC makeVC() {
    const double wp[3] = {95.047, 100.0, 108.883};                    // D65
    const double adapting = (200.0 / kPi) * yFromLstar(50.0) / 100.0;
    const double bgY = yFromLstar(50.0);

    double w[3];
    for (int i = 0; i < 3; ++i)
        w[i] = wp[0] * kXyzToCam[i][0] + wp[1] * kXyzToCam[i][1] + wp[2] * kXyzToCam[i][2];

    const double f = 1.0;                                             // average surround
    VC v{};
    v.c = 0.69;
    v.nc = f;
    const double d = std::clamp(f * (1.0 - (1.0 / 3.6) * std::exp((-adapting - 42.0) / 92.0)), 0.0, 1.0);
    for (int i = 0; i < 3; ++i) v.rgbD[i] = d * (100.0 / w[i]) + 1.0 - d;

    const double k = 1.0 / (5.0 * adapting + 1.0);
    const double k4 = k * k * k * k, k4F = 1.0 - k4;
    v.fl = k4 * adapting + 0.1 * k4F * k4F * std::cbrt(5.0 * adapting);
    v.n = bgY / wp[1];
    v.z = 1.48 + std::sqrt(v.n);
    v.nbb = 0.725 / std::pow(v.n, 0.2);
    v.ncb = v.nbb;

    double rgbA[3];
    for (int i = 0; i < 3; ++i) {
        const double factor = std::pow(v.fl * v.rgbD[i] * w[i] / 100.0, 0.42);
        rgbA[i] = 400.0 * factor / (factor + 27.13);
    }
    v.aw = (40.0 * rgbA[0] + 20.0 * rgbA[1] + rgbA[2]) / 20.0 * v.nbb;
    v.flRoot = std::pow(v.fl, 0.25);
    return v;
}

const VC& vc() {
    static const VC v = makeVC();
    return v;
}

// find the sRGB color with this hue, chroma and luminance Y (Newton iteration on J)
std::optional<QRgb> solveJ(double hueRad, double chroma, double y) {
    const VC& v = vc();
    const M3& m = camToLinear();

    double j = std::sqrt(y) * 11.0;
    const double tInner = 1.0 / std::pow(1.64 - std::pow(0.29, v.n), 0.73);
    const double eHue = 0.25 * (std::cos(hueRad + 2.0) + 3.8);
    const double p1 = eHue * (50000.0 / 13.0) * v.nc * v.ncb;
    const double hs = std::sin(hueRad), hc = std::cos(hueRad);

    for (int it = 0; it < 5; ++it) {
        const double jn = j / 100.0;
        const double alpha = (chroma == 0.0 || j == 0.0) ? 0.0 : chroma / std::sqrt(jn);
        const double t = std::pow(alpha * tInner, 1.0 / 0.9);
        const double ac = v.aw * std::pow(jn, 1.0 / v.c / v.z);
        const double p2 = ac / v.nbb;
        const double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * hc + 108.0 * t * hs);
        const double a = gamma * hc, b = gamma * hs;

        const double adapted[3] = {
            (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0,
            (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0,
            (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0,
        };

        double cam[3];   // undo the chromatic adaptation
        for (int i = 0; i < 3; ++i) {
            const double abs = std::abs(adapted[i]);
            const double base = std::max(0.0, 27.13 * abs / (400.0 - abs));
            const double scaled = signum(adapted[i]) * std::pow(base, 1.0 / 0.42);   // fl * |rD| / 100
            cam[i] = scaled * 100.0 / v.fl / v.rgbD[i];
        }

        double lin[3];
        for (int i = 0; i < 3; ++i) lin[i] = m[i][0] * cam[0] + m[i][1] * cam[1] + m[i][2] * cam[2];

        if (lin[0] < 0 || lin[1] < 0 || lin[2] < 0) return std::nullopt;

        const double fnj = 0.2126 * lin[0] + 0.7152 * lin[1] + 0.0722 * lin[2];
        if (fnj <= 0) return std::nullopt;

        if (it == 4 || std::abs(fnj - y) < 0.002) {
            if (lin[0] > 100.01 || lin[1] > 100.01 || lin[2] > 100.01) return std::nullopt;
            return qRgb(delinearize(lin[0]), delinearize(lin[1]), delinearize(lin[2]));
        }
        j = j - (fnj - y) * j / (2.0 * fnj);
    }
    return std::nullopt;
}

// Material's "rotated hue": a different rotation per range of the source hue
double rotated(double hue, const std::array<double, 9>& hues, const std::array<double, 9>& rot) {
    for (int i = 0; i + 1 < 9; ++i)
        if (hues[i] < hue && hue < hues[i + 1]) return sanitize(hue + rot[i]);
    return hue;
}

}  // namespace

// ---------- public API ----------

Cam fromRgb(QRgb argb) {
    const VC& v = vc();
    const double r = linearize(qRed(argb)), g = linearize(qGreen(argb)), bl = linearize(qBlue(argb));

    const double x = kRgbToXyz[0][0] * r + kRgbToXyz[0][1] * g + kRgbToXyz[0][2] * bl;
    const double y = kRgbToXyz[1][0] * r + kRgbToXyz[1][1] * g + kRgbToXyz[1][2] * bl;
    const double zz = kRgbToXyz[2][0] * r + kRgbToXyz[2][1] * g + kRgbToXyz[2][2] * bl;

    auto adapt = [&](int i) {
        const double c = kXyzToCam[i][0] * x + kXyzToCam[i][1] * y + kXyzToCam[i][2] * zz;
        const double d = v.rgbD[i] * c;
        const double af = std::pow(v.fl * std::abs(d) / 100.0, 0.42);
        return signum(d) * 400.0 * af / (af + 27.13);
    };
    const double rA = adapt(0), gA = adapt(1), bA = adapt(2);

    const double a = (11.0 * rA - 12.0 * gA + bA) / 11.0;
    const double b = (rA + gA - 2.0 * bA) / 9.0;
    const double u = (20.0 * rA + 20.0 * gA + 21.0 * bA) / 20.0;
    const double p2 = (40.0 * rA + 20.0 * gA + bA) / 20.0;

    const double hue = sanitize(std::atan2(b, a) * 180.0 / kPi);
    const double ac = p2 * v.nbb;
    const double J = 100.0 * std::pow(ac / v.aw, v.c * v.z);

    const double huePrime = hue < 20.14 ? hue + 360.0 : hue;
    const double eHue = 0.25 * (std::cos(huePrime * kPi / 180.0 + 2.0) + 3.8);
    const double p1 = 50000.0 / 13.0 * eHue * v.nc * v.ncb;
    const double t = p1 * std::hypot(a, b) / (u + 0.305);
    const double alpha = std::pow(t, 0.9) * std::pow(1.64 - std::pow(0.29, v.n), 0.73);

    return {hue, alpha * std::sqrt(J / 100.0), lstarFromY(y)};
}

QRgb toRgb(double hue, double chroma, double tone) {
    if (chroma < 0.0001 || tone < 0.0001 || tone > 99.9999) return grayFromLstar(tone);

    const double rad = sanitize(hue) * kPi / 180.0;
    const double y = yFromLstar(tone);
    if (auto exact = solveJ(rad, chroma, y)) return *exact;

    // out of the sRGB gamut: keep hue and tone, lower the chroma until it fits
    double lo = 0.0, hi = chroma;
    QRgb best = grayFromLstar(tone);
    for (int i = 0; i < 24; ++i) {
        const double mid = (lo + hi) / 2.0;
        if (auto r = solveJ(rad, mid, y)) { best = *r; lo = mid; }
        else hi = mid;
    }
    return best;
}

QRgb Tonal::tone(double t) const { return toRgb(hue, chroma, t); }

QString hex(QRgb c) { return QColor(c).name(); }

bool parseVariant(const QString& name, Variant* out) {
    QString n = name;
    if (n.startsWith(QLatin1String("scheme-"))) n = n.mid(7);
    if (n == QLatin1String("tonal-spot")) { *out = Variant::TonalSpot; return true; }
    if (n == QLatin1String("expressive")) { *out = Variant::Expressive; return true; }
    if (n == QLatin1String("neutral")) { *out = Variant::Neutral; return true; }
    if (n == QLatin1String("fruit-salad")) { *out = Variant::FruitSalad; return true; }
    if (n == QLatin1String("rainbow")) { *out = Variant::Rainbow; return true; }
    return false;
}

QString variantName(Variant v) {
    switch (v) {
    case Variant::Expressive: return QStringLiteral("scheme-expressive");
    case Variant::Neutral: return QStringLiteral("scheme-neutral");
    case Variant::FruitSalad: return QStringLiteral("scheme-fruit-salad");
    case Variant::Rainbow: return QStringLiteral("scheme-rainbow");
    case Variant::TonalSpot: break;
    }
    return QStringLiteral("scheme-tonal-spot");
}

PaletteSet makePalettes(QRgb source, Variant variant) {
    const double h = fromRgb(source).hue;
    static const std::array<double, 9> hues = {0, 21, 51, 121, 151, 191, 271, 321, 360};

    PaletteSet s;
    s.error = {25.0, 84.0};

    switch (variant) {
    case Variant::Expressive:
        s.primary = {sanitize(h + 240.0), 40.0};
        s.secondary = {rotated(h, hues, {45, 95, 45, 20, 45, 90, 45, 45, 45}), 24.0};
        s.tertiary = {rotated(h, hues, {120, 120, 20, 45, 20, 15, 20, 120, 120}), 32.0};
        s.neutral = {sanitize(h + 15.0), 8.0};
        s.neutralVariant = {sanitize(h + 15.0), 12.0};
        break;
    case Variant::Neutral:
        s.primary = {h, 12.0}; s.secondary = {h, 8.0}; s.tertiary = {h, 16.0};
        s.neutral = {h, 2.0}; s.neutralVariant = {h, 2.0};
        break;
    case Variant::FruitSalad:
        s.primary = {sanitize(h - 50.0), 48.0}; s.secondary = {sanitize(h - 50.0), 36.0};
        s.tertiary = {h, 36.0}; s.neutral = {h, 10.0}; s.neutralVariant = {h, 16.0};
        break;
    case Variant::Rainbow:
        s.primary = {h, 48.0}; s.secondary = {h, 16.0}; s.tertiary = {sanitize(h + 60.0), 24.0};
        s.neutral = {h, 0.0}; s.neutralVariant = {h, 0.0};
        break;
    case Variant::TonalSpot:
        s.primary = {h, 36.0}; s.secondary = {h, 16.0}; s.tertiary = {sanitize(h + 60.0), 24.0};
        s.neutral = {h, 6.0}; s.neutralVariant = {h, 8.0};
        break;
    }
    return s;
}

QJsonObject buildScheme(QRgb source, Variant variant, bool dark) {
    const PaletteSet ps = makePalettes(source, variant);
    const Tonal* pal[] = {&ps.primary, &ps.secondary, &ps.tertiary, &ps.neutral, &ps.neutralVariant, &ps.error};
    // palette index: 0 primary, 1 secondary, 2 tertiary, 3 neutral, 4 neutral variant, 5 error

    struct Role { const char* key; int pal; double light; double dark; };
    static const Role roles[] = {
        {"background", 3, 98, 6},
        {"error", 5, 40, 80},
        {"error_container", 5, 90, 30},
        {"inverse_on_surface", 3, 95, 20},
        {"inverse_primary", 0, 80, 40},
        {"inverse_surface", 3, 20, 90},
        {"on_background", 3, 10, 90},
        {"on_error", 5, 100, 20},
        {"on_error_container", 5, 10, 90},
        {"on_primary", 0, 100, 20},
        {"on_primary_container", 0, 10, 90},
        {"on_primary_fixed", 0, 10, 10},
        {"on_primary_fixed_variant", 0, 30, 30},
        {"on_secondary", 1, 100, 20},
        {"on_secondary_container", 1, 10, 90},
        {"on_secondary_fixed", 1, 10, 10},
        {"on_secondary_fixed_variant", 1, 30, 30},
        {"on_surface", 3, 10, 90},
        {"on_surface_variant", 4, 30, 80},
        {"on_tertiary", 2, 100, 20},
        {"on_tertiary_container", 2, 10, 90},
        {"on_tertiary_fixed", 2, 10, 10},
        {"on_tertiary_fixed_variant", 2, 30, 30},
        {"outline", 4, 50, 60},
        {"outline_variant", 4, 80, 30},
        {"primary", 0, 40, 80},
        {"primary_container", 0, 90, 30},
        {"primary_fixed", 0, 90, 90},
        {"primary_fixed_dim", 0, 80, 80},
        {"scrim", 3, 0, 0},
        {"secondary", 1, 40, 80},
        {"secondary_container", 1, 90, 30},
        {"secondary_fixed", 1, 90, 90},
        {"secondary_fixed_dim", 1, 80, 80},
        {"shadow", 3, 0, 0},
        {"surface", 3, 98, 6},
        {"surface_bright", 3, 98, 24},
        {"surface_container", 3, 94, 12},
        {"surface_container_high", 3, 92, 17},
        {"surface_container_highest", 3, 90, 22},
        {"surface_container_low", 3, 96, 10},
        {"surface_container_lowest", 3, 100, 4},
        {"surface_dim", 3, 87, 6},
        {"surface_variant", 4, 90, 30},
        {"tertiary", 2, 40, 80},
        {"tertiary_container", 2, 90, 30},
        {"tertiary_fixed", 2, 90, 90},
        {"tertiary_fixed_dim", 2, 80, 80},
    };

    QJsonObject o;
    for (const Role& r : roles)
        o[QString::fromLatin1(r.key)] = hex(pal[r.pal]->tone(dark ? r.dark : r.light));
    o[QStringLiteral("source_color")] = hex(source);
    o[QStringLiteral("surface_tint")] = o.value(QStringLiteral("primary"));
    return o;
}

QList<QRgb> extractSources(const QImage& input, int limit) {
    const QImage img = input.scaled(112, 112, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_ARGB32);

    // 5-bit-per-channel histogram
    QHash<int, int> hist;
    int total = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(line[x]) < 255) continue;
            const int key = ((qRed(line[x]) >> 3) << 10) | ((qGreen(line[x]) >> 3) << 5) | (qBlue(line[x]) >> 3);
            ++hist[key];
            ++total;
        }
    }
    if (total == 0) return {qRgb(0x42, 0x85, 0xf4)};

    struct Entry { QRgb rgb; Cam cam; int pop; double score = 0; };
    QList<Entry> entries;
    std::array<double, 360> huePop{};
    for (auto it = hist.cbegin(); it != hist.cend(); ++it) {
        const int k = it.key();
        const QRgb rgb = qRgb((((k >> 10) & 31) << 3) | 4, (((k >> 5) & 31) << 3) | 4, ((k & 31) << 3) | 4);
        const Cam cam = fromRgb(rgb);
        entries.append({rgb, cam, it.value()});
        huePop[int(std::floor(sanitize(cam.hue))) % 360] += it.value();
    }

    // Material's scoring: reward colors whose hue is common, and chroma near 48
    QList<Entry> scored;
    for (Entry e : entries) {
        const int hue = int(std::floor(sanitize(e.cam.hue)));
        double excited = 0;
        for (int j = hue - 14; j <= hue + 15; ++j) excited += huePop[((j % 360) + 360) % 360];
        excited /= total;
        if (e.cam.chroma < 5.0 || excited <= 0.01) continue;
        e.score = excited * 100.0 * 0.7 + (e.cam.chroma - 48.0) * (e.cam.chroma < 48.0 ? 0.1 : 0.3);
        scored.append(e);
    }
    std::sort(scored.begin(), scored.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });

    QList<QRgb> out;
    QList<double> hues;
    for (const Entry& e : scored) {
        bool near = false;
        for (double h : hues) {
            double d = std::abs(e.cam.hue - h);
            d = std::min(d, 360.0 - d);
            if (d < 15.0) near = true;
        }
        if (near) continue;
        out.append(e.rgb);
        hues.append(e.cam.hue);
        if (out.size() >= limit) break;
    }
    if (out.isEmpty()) out.append(qRgb(0x42, 0x85, 0xf4));
    return out;
}

}  // namespace iris