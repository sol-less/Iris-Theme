#pragma once

#include <QJsonObject>
#include <QString>

namespace iris {

// Small TOML subset: [tables] and [a.b] tables, key = value with strings ("..." and '...'),
// numbers, booleans, arrays (also multi-line) and inline tables { a = 1 }.
// Not supported: [[array tables]], multi-line strings, dates.
QJsonObject parseToml(const QString& text, QString* error);

}  // namespace iris