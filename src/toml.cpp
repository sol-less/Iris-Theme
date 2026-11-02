#include "toml.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QStringList>

namespace iris {
namespace {

// drop "# comment" outside of strings
QString stripComment(const QString& line) {
    bool inD = false, inS = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line[i];
        if (inD) { if (c == '\\') ++i; else if (c == '"') inD = false; }
        else if (inS) { if (c == '\'') inS = false; }
        else if (c == '"') inD = true;
        else if (c == '\'') inS = true;
        else if (c == '#') return line.left(i);
    }
    return line;
}

// net nesting of [ ] and { } outside of strings
int depthOf(const QString& s) {
    int depth = 0;
    bool inD = false, inS = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (inD) { if (c == '\\') ++i; else if (c == '"') inD = false; }
        else if (inS) { if (c == '\'') inS = false; }
        else if (c == '"') inD = true;
        else if (c == '\'') inS = true;
        else if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') --depth;
    }
    return depth;
}

// first `ch` outside of strings, or -1
int findTop(const QString& s, QChar ch) {
    bool inD = false, inS = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (inD) { if (c == '\\') ++i; else if (c == '"') inD = false; }
        else if (inS) { if (c == '\'') inS = false; }
        else if (c == '"') inD = true;
        else if (c == '\'') inS = true;
        else if (c == ch) return i;
    }
    return -1;
}

// split at commas outside of strings, brackets and braces
QStringList splitTop(const QString& s) {
    QStringList parts;
    QString cur;
    int depth = 0;
    bool inD = false, inS = false;
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (inD) {
            cur += c;
            if (c == '\\' && i + 1 < s.size()) cur += s[++i];
            else if (c == '"') inD = false;
            continue;
        }
        if (inS) { cur += c; if (c == '\'') inS = false; continue; }
        if (c == '"') { inD = true; cur += c; }
        else if (c == '\'') { inS = true; cur += c; }
        else if (c == '[' || c == '{') { ++depth; cur += c; }
        else if (c == ']' || c == '}') { --depth; cur += c; }
        else if (c == ',' && depth == 0) { parts << cur; cur.clear(); }
        else cur += c;
    }
    if (!cur.trimmed().isEmpty()) parts << cur;
    return parts;
}

QString unescape(const QString& s) {
    QString out;
    for (int i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const QChar n = s[++i];
            if (n == 'n') out += '\n';
            else if (n == 't') out += '\t';
            else if (n == 'r') out += '\r';
            else out += n;
        } else {
            out += s[i];
        }
    }
    return out;
}

QString unquote(const QString& s) {
    if (s.size() >= 2 && s.startsWith('"') && s.endsWith('"')) return unescape(s.mid(1, s.size() - 2));
    if (s.size() >= 2 && s.startsWith('\'') && s.endsWith('\'')) return s.mid(1, s.size() - 2);
    return s;
}

QJsonValue parseValue(const QString& raw, bool* ok) {
    const QString t = raw.trimmed();
    if (t.startsWith('"') || t.startsWith('\'')) return unquote(t);
    if (t == QLatin1String("true")) return true;
    if (t == QLatin1String("false")) return false;

    if (t.startsWith('[') && t.endsWith(']')) {
        QJsonArray arr;
        for (const QString& part : splitTop(t.mid(1, t.size() - 2))) {
            if (part.trimmed().isEmpty()) continue;
            arr.append(parseValue(part, ok));
        }
        return arr;
    }

    if (t.startsWith('{') && t.endsWith('}')) {
        QJsonObject obj;
        for (const QString& part : splitTop(t.mid(1, t.size() - 2))) {
            const int eq = findTop(part, '=');
            if (eq < 0) { *ok = false; continue; }
            obj[unquote(part.left(eq).trimmed())] = parseValue(part.mid(eq + 1), ok);
        }
        return obj;
    }

    QString n = t;
    n.remove('_');
    bool isNum = false;
    const double d = n.toDouble(&isNum);
    if (isNum) return d;

    *ok = false;
    return QJsonValue();
}

void touch(QJsonObject& obj, const QStringList& path, int i) {
    if (i == path.size()) return;
    QJsonObject child = obj.value(path[i]).toObject();
    touch(child, path, i + 1);
    obj[path[i]] = child;
}

void setAt(QJsonObject& obj, const QStringList& path, int i, const QString& key, const QJsonValue& v) {
    if (i == path.size()) { obj[key] = v; return; }
    QJsonObject child = obj.value(path[i]).toObject();
    setAt(child, path, i + 1, key, v);
    obj[path[i]] = child;
}

}  // namespace

QJsonObject parseToml(const QString& text, QString* error) {
    QJsonObject root;
    QStringList path;
    const QStringList lines = text.split('\n');

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = stripComment(lines[i]).trimmed();
        if (line.isEmpty()) continue;

        if (line.startsWith(QLatin1String("[["))) {
            *error = QStringLiteral("line %1: [[array tables]] are not supported").arg(i + 1);
            return root;
        }

        if (line.startsWith('[')) {
            const int end = line.lastIndexOf(']');
            if (end < 0) {
                *error = QStringLiteral("line %1: missing ]").arg(i + 1);
                return root;
            }
            const QString name = line.mid(1, end - 1).trimmed();

            path.clear();
            QString cur;
            bool inD = false, inS = false;
            for (const QChar c : name) {
                if (inD) { cur += c; if (c == '"') inD = false; }
                else if (inS) { cur += c; if (c == '\'') inS = false; }
                else if (c == '"') { inD = true; cur += c; }
                else if (c == '\'') { inS = true; cur += c; }
                else if (c == '.') { path << unquote(cur.trimmed()); cur.clear(); }
                else cur += c;
            }
            path << unquote(cur.trimmed());
            touch(root, path, 0);
            continue;
        }

        const int eq = findTop(line, '=');
        if (eq <= 0) {
            *error = QStringLiteral("line %1: expected key = value").arg(i + 1);
            return root;
        }
        const QString key = unquote(line.left(eq).trimmed());
        QString value = line.mid(eq + 1).trimmed();

        // arrays and inline tables may continue on the next lines
        while (depthOf(value) > 0 && i + 1 < lines.size())
            value += QLatin1Char('\n') + stripComment(lines[++i]);

        bool ok = true;
        const QJsonValue v = parseValue(value, &ok);
        if (!ok) {
            *error = QStringLiteral("line %1: cannot read the value of \"%2\"").arg(i + 1).arg(key);
            return root;
        }
        setAt(root, path, 0, key, v);
    }
    return root;
}

}  // namespace iris