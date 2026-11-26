#include "Importers.h"

#include "Inei.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextCodec>
#include <QXmlStreamReader>

namespace zr {

static void addGroup(ImportResult &r, const QString &name)
{
    for (const GroupInfo &g : r.groups)
        if (g.name == name) return;
    GroupInfo g;
    g.name = name;
    r.groups.append(g);
}

ImportResult parseTextList(const QString &text, const QString &defaultGroup)
{
    ImportResult r;
    QString group = defaultGroup;
    int lineNo = 0;
    for (QString line : text.split(QLatin1Char('\n'))) {
        ++lineNo;
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QLatin1Char('#'))) {
            // «# Офис 101» — заголовок группы, как подписи в избранном vmPing
            const QString title = line.mid(1).trimmed();
            if (!title.isEmpty()) group = title;
            continue;
        }
        ParsedInput p = parseTargetInput(line);
        if (!p.ok) {
            r.warnings << QStringLiteral("Строка %1: %2").arg(lineNo).arg(p.error);
            continue;
        }
        p.target.group = group;
        addGroup(r, group);
        r.targets.append(p.target);
    }
    return r;
}

static QChar detectDelimiter(const QString &firstLine)
{
    const QList<QChar> cands{QLatin1Char(';'), QLatin1Char('\t'), QLatin1Char(',')};
    QChar best = QLatin1Char(';');
    int bestCount = -1;
    for (QChar c : cands) {
        const int n = firstLine.count(c);
        if (n > bestCount) { best = c; bestCount = n; }
    }
    return best;
}

static QStringList splitCsvLine(const QString &line, QChar sep)
{
    QStringList out;
    QString cur;
    bool quoted = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (quoted) {
            if (c == QLatin1Char('"')) {
                if (i + 1 < line.size() && line.at(i + 1) == QLatin1Char('"')) { cur += c; ++i; }
                else quoted = false;
            } else cur += c;
        } else if (c == QLatin1Char('"')) {
            quoted = true;
        } else if (c == sep) {
            out << cur.trimmed();
            cur.clear();
        } else cur += c;
    }
    out << cur.trimmed();
    return out;
}

ImportResult parseCsv(const QString &textIn, const QString &defaultGroup)
{
    ImportResult r;
    QString text = textIn;
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &l : lines) l = l.trimmed();
    lines.removeAll(QString());
    if (lines.isEmpty()) {
        r.error = QStringLiteral("Файл пуст");
        return r;
    }
    const QChar sep = detectDelimiter(lines.first());
    // столбцы по заголовку; если заголовка нет — адрес, имя, группа
    int cAddr = 0, cName = 1, cGroup = 2, cKind = -1, cPort = -1, cNotes = -1;
    const QStringList head = splitCsvLine(lines.first(), sep);
    bool hasHeader = false;
    for (int i = 0; i < head.size(); ++i) {
        const QString h = head[i].toLower();
        auto is = [&h](std::initializer_list<const char *> names) {
            for (const char *n : names) if (h == QString::fromUtf8(n)) return true;
            return false;
        };
        if (is({"address", "адрес", "ip", "host", "хост", "узел"})) { cAddr = i; hasHeader = true; }
        else if (is({"name", "имя", "название", "подпись", "alias"})) { cName = i; hasHeader = true; }
        else if (is({"group", "группа", "офис"})) { cGroup = i; hasHeader = true; }
        else if (is({"kind", "тип", "вид", "проверка"})) { cKind = i; hasHeader = true; }
        else if (is({"port", "порт"})) { cPort = i; hasHeader = true; }
        else if (is({"notes", "заметки", "комментарий", "примечание"})) { cNotes = i; hasHeader = true; }
    }
    for (int li = hasHeader ? 1 : 0; li < lines.size(); ++li) {
        const QStringList f = splitCsvLine(lines[li], sep);
        auto col = [&f](int c) { return c >= 0 && c < f.size() ? f[c] : QString(); };
        QString addr = col(cAddr);
        if (addr.isEmpty()) continue;
        const QString kind = col(cKind);
        const QString port = col(cPort);
        if (!kind.isEmpty() && !addr.contains(QStringLiteral("://"))) {
            bool okKind = false;
            const ProbeKind k = kindFromKey(kind, &okKind);
            if (okKind && k != ProbeKind::Icmp && k != ProbeKind::Http)
                addr = kindToKey(k) + QLatin1Char(':') + addr + (port.isEmpty() ? QString() : QLatin1Char(':') + port);
        } else if (!port.isEmpty() && !addr.contains(QLatin1Char(':'))) {
            addr += QLatin1Char(':') + port;
        }
        ParsedInput p = parseTargetInput(addr);
        if (!p.ok) {
            r.warnings << QStringLiteral("Строка %1: %2").arg(li + 1).arg(p.error);
            continue;
        }
        p.target.name = col(cName);
        const QString g = col(cGroup);
        p.target.group = g.isEmpty() ? defaultGroup : g;
        p.target.notes = col(cNotes);
        addGroup(r, p.target.group);
        r.targets.append(p.target);
    }
    return r;
}

ImportResult parseVmPingXml(const QByteArray &xml)
{
    ImportResult r;
    QXmlStreamReader x(xml);
    QHash<QString, QString> aliases;
    struct Fav { QString title, group; QStringList hosts; };
    QVector<Fav> favs;
    Fav cur;
    bool inFav = false;
    bool sawRoot = false;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QString n = x.name().toString().toLower();
            if (n == QLatin1String("vmping")) sawRoot = true;
            else if (n == QLatin1String("favorite")) {
                inFav = true;
                cur = Fav();
                cur.title = x.attributes().value(QStringLiteral("title")).toString();
                cur.group = x.attributes().value(QStringLiteral("group")).toString();
            } else if (n == QLatin1String("host") && inFav) {
                cur.hosts << x.readElementText().trimmed();
            } else if (n == QLatin1String("alias")) {
                const QString host = x.attributes().value(QStringLiteral("host")).toString().toLower();
                const QString name = x.readElementText().trimmed();
                if (!host.isEmpty()) aliases.insert(host, name);
            }
        } else if (x.isEndElement() && x.name().toString().toLower() == QLatin1String("favorite")) {
            favs.append(cur);
            inFav = false;
        }
    }
    if (x.hasError() || !sawRoot) {
        r.error = x.hasError() ? QStringLiteral("Ошибка XML: %1").arg(x.errorString())
                               : QStringLiteral("Это не файл vmPing.xml");
        return r;
    }
    for (const Fav &f : favs) {
        const QString group = f.group.isEmpty() ? f.title : QStringLiteral("%1 / %2").arg(f.group, f.title);
        addGroup(r, group);
        for (QString h : f.hosts) {
            if (h.isEmpty() || h.startsWith(QLatin1Char('#')))
                continue;   // подписи из избранного
            QString label;
            const int hash = h.indexOf(QLatin1Char('#'));
            if (hash > 0) { label = h.mid(hash + 1).trimmed(); h = h.left(hash).trimmed(); }
            if (h.startsWith(QLatin1String("D/"), Qt::CaseInsensitive)) h = QStringLiteral("dns:") + h.mid(2);
            else if (h.startsWith(QLatin1String("T/"), Qt::CaseInsensitive)) h = h.mid(2);
            ParsedInput p = parseTargetInput(h);
            if (!p.ok) {
                r.warnings << QStringLiteral("%1: %2").arg(h, p.error);
                continue;
            }
            p.target.group = group;
            const QString alias = aliases.value(p.target.address.toLower());
            p.target.name = !label.isEmpty() ? label : alias;
            r.targets.append(p.target);
        }
    }
    // хосты из алиасов без избранного тоже пригодятся
    if (favs.isEmpty() && !aliases.isEmpty()) {
        const QString group = QStringLiteral("vmPing");
        addGroup(r, group);
        for (auto it = aliases.constBegin(); it != aliases.constEnd(); ++it) {
            ParsedInput p = parseTargetInput(it.key());
            if (!p.ok) continue;
            p.target.name = it.value();
            p.target.group = group;
            r.targets.append(p.target);
        }
    }
    return r;
}

ImportResult parseZarnitsaJson(const QByteArray &json)
{
    ImportResult r;
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError) {
        r.error = QStringLiteral("Ошибка JSON: %1").arg(pe.errorString());
        return r;
    }
    const QJsonObject o = d.object();
    for (const QJsonValue &v : o.value(QStringLiteral("groups")).toArray()) {
        GroupInfo g;
        g.name = v.toObject().value(QStringLiteral("name")).toString();
        r.groups.append(g);
    }
    for (const QJsonValue &v : o.value(QStringLiteral("targets")).toArray()) {
        TargetConfig t = TargetConfig::fromJson(v.toObject());
        t.id = newTargetId();
        t.sourceId.clear();
        t.sourceKey.clear();
        if (!t.address.isEmpty()) r.targets.append(t);
    }
    return r;
}

ImportResult importFile(const QString &path, const QString &defaultGroup)
{
    ImportResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = f.errorString();
        return r;
    }
    const QByteArray data = f.readAll();
    const QString ext = QFileInfo(path).suffix().toLower();
    const QByteArray head = data.left(512).trimmed();
    if (ext == QLatin1String("xml") || head.startsWith("<?xml") || head.startsWith("<vmping"))
        return parseVmPingXml(data);
    if (ext == QLatin1String("json") || head.startsWith('{') || head.startsWith('[')) {
        const QJsonDocument d = QJsonDocument::fromJson(data);
        if (d.isObject() && d.object().value(QStringLiteral("app")).toString() == QLatin1String("zarnitsa"))
            return parseZarnitsaJson(data);
        // phones_<user>.json Инея или ответ /phones
        return parseIneiPhones(d, defaultGroup.isEmpty() ? QStringLiteral("Иней") : defaultGroup,
                               QStringLiteral("icmp"), QString(), QString());
    }
    QTextCodec *codec = QTextCodec::codecForUtfText(data, QTextCodec::codecForName("UTF-8"));
    QString text = codec->toUnicode(data);
    if (text.contains(QChar(0xFFFD))) {
        // старые выгрузки из Windows бывают в cp1251
        if (QTextCodec *cp = QTextCodec::codecForName("Windows-1251"))
            text = cp->toUnicode(data);
    }
    if (ext == QLatin1String("csv") || ext == QLatin1String("tsv"))
        return parseCsv(text, defaultGroup);
    return parseTextList(text, defaultGroup);
}

QString exportCsv(const QVector<TargetConfig> &targets)
{
    auto q = [](QString s) {
        if (s.contains(QLatin1Char(';')) || s.contains(QLatin1Char('"')) || s.contains(QLatin1Char('\n'))) {
            s.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(s);
        }
        return s;
    };
    QString out = QStringLiteral("Имя;Адрес;Группа;Тип;Порт;Заметки\r\n");
    for (const TargetConfig &t : targets) {
        out += q(t.name) + QLatin1Char(';') + q(t.address) + QLatin1Char(';') + q(t.group) + QLatin1Char(';')
            + kindToKey(t.kind) + QLatin1Char(';') + (t.port ? QString::number(t.port) : QString()) + QLatin1Char(';')
            + q(t.notes) + QStringLiteral("\r\n");
    }
    return out;
}

QByteArray exportJson(const QVector<TargetConfig> &targets, const QVector<GroupInfo> &groups)
{
    QJsonObject o;
    o[QStringLiteral("app")] = QStringLiteral("zarnitsa");
    o[QStringLiteral("version")] = 1;
    QJsonArray ga;
    for (const GroupInfo &g : groups) {
        QJsonObject go;
        go[QStringLiteral("name")] = g.name;
        ga.append(go);
    }
    o[QStringLiteral("groups")] = ga;
    QJsonArray ta;
    for (const TargetConfig &t : targets) ta.append(t.toJson());
    o[QStringLiteral("targets")] = ta;
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

QString exportTextList(const QVector<TargetConfig> &targets)
{
    QString out;
    QString group = QStringLiteral("\x01");
    for (const TargetConfig &t : targets) {
        if (t.group != group) {
            group = t.group;
            if (!group.isEmpty()) out += QStringLiteral("# %1\n").arg(group);
        }
        out += t.kind == ProbeKind::Http ? t.address : t.endpointText();
        if (!t.name.isEmpty()) out += QStringLiteral(" # ") + t.name;
        out += QLatin1Char('\n');
    }
    return out;
}

} // namespace zr
