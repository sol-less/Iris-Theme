#include <algorithm>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTextStream>

#include "color.h"
#include "render.h"
#include "toml.h"

namespace {

QString expand(QString p) {
    if (p == QLatin1String("~")) return QDir::homePath();
    if (p.startsWith(QLatin1String("~/"))) p = QDir::homePath() + p.mid(1);
    return p;
}

bool writeAtomic(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) >= 0 && f.commit();
}

QList<double> numbers(const QStringList& args) {
    QList<double> out;
    static const QRegularExpression re(QStringLiteral("-?\\d+(?:\\.\\d+)?"));
    auto it = re.globalMatch(args.join(' '));
    while (it.hasNext()) out.append(it.next().captured(0).toDouble());
    return out;
}

void runHook(const QString& hook, const QProcessEnvironment& env) {
    QProcess pr;
    pr.setProcessEnvironment(env);
    pr.setProcessChannelMode(QProcess::ForwardedChannels);
    pr.start(QStringLiteral("sh"), {QStringLiteral("-c"), hook});
    if (!pr.waitForFinished(15000)) {
        pr.kill();
        pr.waitForFinished(1000);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationVersion("0.2");
    QTextStream out(stdout), err(stderr);

    QCommandLineParser p;
    p.setApplicationDescription("Iris Theme: Material 3 color generator and template renderer");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("command", "image <path> | color hex|rgb|hsl <value...>");
    p.addOptions({
        {{"c", "config"}, "Config file (default ~/.config/iris-theme/config.toml).", "file"},
        {{"t", "type"}, "Scheme: scheme-expressive, scheme-tonal-spot, scheme-neutral, scheme-fruit-salad, scheme-rainbow.", "scheme"},
        {{"m", "mode"}, "dark | light", "mode"},
        {"contrast", "Contrast from -1.0 to 1.0 (not implemented yet).", "value", "0"},
        {"source-color-index", "Which color of the image to use (see --list-sources).", "n", "0"},
        {"prefer", "darkness | lightness | saturation | less-saturation | value | closest-to-fallback", "how"},
        {"fallback-color", "Used by --prefer closest-to-fallback.", "hex", "#4285f4"},
        {"json", "Print JSON to stdout: hex | hex_stripped | rgb | rgba | hsl | hsla", "format"},
        {{"o", "out"}, "Write the shell colors.json here (overrides json_out in the config).", "file"},
        {"dry-run", "Do not write files, set the wallpaper or run hooks."},
        {"list-sources", "Print the color candidates of the image and exit."},
        {"list-schemes", "Print the available schemes and exit."},
        {{"q", "quiet"}, "No output besides errors."},
    });
    p.process(app);

    if (p.isSet("list-schemes")) {
        out << "scheme-expressive\nscheme-tonal-spot\nscheme-neutral\nscheme-fruit-salad\nscheme-rainbow\n"
               "(not implemented yet: scheme-content, scheme-fidelity, scheme-monochrome)\n";
        return 0;
    }

    const QStringList pos = p.positionalArguments();
    const QString cmd = pos.value(0);
    if (cmd != QLatin1String("image") && cmd != QLatin1String("color")) {
        err << "usage: iris-theme image <path> | color hex|rgb|hsl <value...>   (--help for options)\n";
        return 2;
    }
    const bool quiet = p.isSet("quiet");
    const bool dry = p.isSet("dry-run");

    // ----- config.toml -----
    QString cfgPath = p.isSet("config") ? p.value("config") : qEnvironmentVariable("IRIS_THEME_CONFIG");
    if (cfgPath.isEmpty())
        cfgPath = qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + "/.config") + "/iris-theme/config.toml";
    cfgPath = expand(cfgPath);

    QJsonObject cfg;
    {
        QFile f(cfgPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString perr;
            cfg = iris::parseToml(QString::fromUtf8(f.readAll()), &perr);
            if (!perr.isEmpty()) {
                err << cfgPath << ": " << perr << "\n";
                return 1;
            }
        }
    }
    const QJsonObject conf = cfg.value("config").toObject();

    // ----- scheme and mode -----
    const QString type = p.isSet("type") ? p.value("type")
                         : conf.value("default_scheme").toString("scheme-expressive");
    iris::Variant variant;
    if (!iris::parseVariant(type, &variant)) {
        err << "scheme not available: " << type
            << "\n(content, fidelity and monochrome are not implemented yet; see --list-schemes)\n";
        return 2;
    }
    const QString mode = p.isSet("mode") ? p.value("mode") : conf.value("default_mode").toString("dark");
    if (mode != QLatin1String("dark") && mode != QLatin1String("light")) {
        err << "mode must be dark or light\n";
        return 2;
    }
    const bool dark = mode == QLatin1String("dark");
    if (p.value("contrast").toDouble() != 0.0 && !quiet)
        err << "note: --contrast is not implemented yet, using 0\n";

    // ----- source color -----
    QRgb source = 0;
    QString imagePath;
    int sourceIndex = 0, sourceCount = 1;

    if (cmd == QLatin1String("color")) {
        const QString kind = pos.value(1);
        const QStringList rest = pos.mid(2);
        QColor c;
        if (kind == QLatin1String("hex")) {
            QString h = rest.value(0);
            if (!h.startsWith('#')) h.prepend('#');
            c = QColor(h);
        } else if (kind == QLatin1String("rgb")) {
            const QList<double> n = numbers(rest);
            if (n.size() >= 3) c = QColor(int(n[0]), int(n[1]), int(n[2]));
        } else if (kind == QLatin1String("hsl")) {
            const QList<double> n = numbers(rest);
            if (n.size() >= 3) c = QColor::fromHslF(float(std::fmod(n[0], 360.0) / 360.0), float(n[1] / 100.0), float(n[2] / 100.0));
        } else {
            err << "usage: iris-theme color hex|rgb|hsl <value...>\n";
            return 2;
        }
        if (!c.isValid()) { err << "invalid color\n"; return 2; }
        source = c.rgb();
    } else {
        imagePath = expand(pos.value(1));
        const QImage img(imagePath);
        if (img.isNull()) { err << "cannot read image: " << imagePath << "\n"; return 1; }
        imagePath = QFileInfo(imagePath).absoluteFilePath();

        QList<QRgb> cands = iris::extractSources(img, 8);
        const QColor fallback(p.value("fallback-color"));

        if (p.isSet("prefer")) {
            const QString how = p.value("prefer");
            auto key = [&](QRgb c) -> double {
                const iris::Cam cam = iris::fromRgb(c);
                if (how == QLatin1String("darkness")) return cam.tone;
                if (how == QLatin1String("lightness")) return -cam.tone;
                if (how == QLatin1String("saturation")) return -cam.chroma;
                if (how == QLatin1String("less-saturation")) return cam.chroma;
                if (how == QLatin1String("value")) return -std::max({qRed(c), qGreen(c), qBlue(c)});
                if (how == QLatin1String("closest-to-fallback")) {
                    const double d = std::abs(cam.hue - iris::fromRgb(fallback.rgb()).hue);
                    return std::min(d, 360.0 - d);
                }
                return 0.0;
            };
            std::stable_sort(cands.begin(), cands.end(), [&](QRgb a, QRgb b) { return key(a) < key(b); });
        }

        if (p.isSet("list-sources")) {
            for (int i = 0; i < cands.size(); ++i) {
                const QRgb c = cands[i];
                out << i << "  \x1b[48;2;" << qRed(c) << ';' << qGreen(c) << ';' << qBlue(c)
                    << "m    \x1b[0m  " << iris::hex(c) << "\n";
            }
            return 0;
        }

        sourceCount = cands.size();
        sourceIndex = qBound(0, p.value("source-color-index").toInt(), sourceCount - 1);
        source = cands.at(sourceIndex);
    }

    // ----- generate -----
    const QJsonObject dk = iris::buildScheme(source, variant, true);
    const QJsonObject lt = iris::buildScheme(source, variant, false);
    const QJsonObject selected = dark ? dk : lt;

    iris::ThemeData data;
    data.mode = mode;
    data.image = imagePath;
    auto toMap = [](const QJsonObject& o) {
        QMap<QString, QRgb> m;
        for (auto it = o.begin(); it != o.end(); ++it) m[it.key()] = QColor(it.value().toString()).rgb();
        return m;
    };
    data.dark = toMap(dk);
    data.light = toMap(lt);

    const iris::PaletteSet ps = iris::makePalettes(source, variant);
    static const int tones[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 50, 60, 70, 80, 90, 95, 98, 99, 100};
    auto addPalette = [&](const QString& name, const iris::Tonal& t) {
        for (int tone : tones) data.palettes[name][tone] = t.tone(tone);
    };
    addPalette("primary", ps.primary);
    addPalette("secondary", ps.secondary);
    addPalette("tertiary", ps.tertiary);
    addPalette("neutral", ps.neutral);
    addPalette("neutral_variant", ps.neutralVariant);
    addPalette("error", ps.error);

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("IRIS_THEME_MODE", mode);
    env.insert("IRIS_THEME_IMAGE", imagePath);
    env.insert("IRIS_THEME_SOURCE", iris::hex(source));
    env.insert("IRIS_THEME_SCHEME", iris::variantName(variant));

    // ----- wallpaper (image only) -----
    const QJsonObject wp = conf.value("wallpaper").toObject();
    if (!imagePath.isEmpty() && wp.value("set").toBool(false) && !dry) {
        QStringList args;
        for (const QJsonValue& v : wp.value("arguments").toArray()) args << v.toString();
        args << imagePath;
        QProcess::startDetached(wp.value("command").toString(), args);
    }

    // ----- shell colors.json -----
    const QString fmt = p.isSet("json") ? p.value("json") : QStringLiteral("hex");
    auto convert = [&](const QJsonObject& o) {
        QJsonObject r;
        for (auto it = o.begin(); it != o.end(); ++it) {
            bool ok = false;
            const QString s = iris::formatColor(QColor(it.value().toString()), fmt, &ok);
            r[it.key()] = ok ? s : it.value().toString();
        }
        return r;
    };

    QJsonObject doc;
    doc["mode"] = mode;
    doc["scheme"] = iris::variantName(variant);
    doc["source"] = iris::hex(source);
    doc["image"] = imagePath;
    doc["md3"] = selected;                         // always #rrggbb: this is what Theme reads
    doc["colors"] = QJsonObject{{"dark", convert(dk)}, {"light", convert(lt)}};
    const QByteArray json = QJsonDocument(doc).toJson(QJsonDocument::Indented);

    if (!dry) {
        QString jsonOut = p.isSet("out") ? p.value("out")
                          : qEnvironmentVariable("IRIS_THEME_OUT");
        if (jsonOut.isEmpty() && conf.contains("json_out")) jsonOut = conf.value("json_out").toString();
        else if (jsonOut.isEmpty())
            jsonOut = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state")
                      + "/quickshell/generated/colors.json";
        jsonOut = expand(jsonOut);

        if (!jsonOut.isEmpty()) {
            if (!writeAtomic(jsonOut, json)) { err << "cannot write " << jsonOut << "\n"; return 1; }
            if (!quiet && !p.isSet("json")) err << "wrote " << jsonOut << "\n";
        }
    }

    // ----- templates -----
    const QJsonObject tpls = cfg.value("templates").toObject();
    for (auto it = tpls.begin(); it != tpls.end(); ++it) {
        const QString name = it.key();
        const QJsonObject t = it.value().toObject();
        if (!t.value("enabled").toBool(true)) continue;

        const QString inPath = expand(t.value("input_path").toString());
        const QString outPath = expand(t.value("output_path").toString());
        if (inPath.isEmpty() || outPath.isEmpty()) {
            err << "template " << name << ": input_path and output_path are required\n";
            continue;
        }
        QFile f(inPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            err << "template " << name << ": cannot read " << inPath << "\n";
            continue;
        }

        const QString m = t.value("mode").toString();
        const QString tplMode = (m == QLatin1String("dark") || m == QLatin1String("light")) ? m : mode;

        QStringList warnings;
        const QString rendered = iris::renderTemplate(QString::fromUtf8(f.readAll()), data, tplMode, &warnings);
        for (const QString& w : std::as_const(warnings)) err << "template " << name << ": " << w << "\n";

        if (dry) {
            if (!quiet) err << "would write " << outPath << "\n";
            continue;
        }
        if (!writeAtomic(outPath, rendered.toUtf8())) {
            err << "template " << name << ": cannot write " << outPath << "\n";
            continue;
        }
        if (!quiet && !p.isSet("json")) err << "wrote " << outPath << "\n";

        const QString hook = t.value("post_hook").toString();
        if (!hook.isEmpty()) runHook(hook, env);
    }

    // ----- show -----
    if (p.isSet("json")) {
        out << json;
    } else if (!quiet) {
        out << "source " << iris::hex(source);
        if (sourceCount > 1) out << "  (candidate " << sourceIndex << " of " << sourceCount << ")";
        out << "  " << iris::variantName(variant) << "  " << mode << "\n\n";
        for (auto it = selected.begin(); it != selected.end(); ++it) {
            const QColor c(it.value().toString());
            out << "\x1b[48;2;" << c.red() << ';' << c.green() << ';' << c.blue() << "m    \x1b[0m  "
                << it.key().leftJustified(28) << c.name() << "\n";
        }
    }
    return 0;
}