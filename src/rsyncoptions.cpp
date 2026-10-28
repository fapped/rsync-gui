#include "rsyncoptions.h"
#include "localpath.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonValue>
#include <QObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace {

const QString kJsonFormat = QStringLiteral("rsync-gui-settings");

struct ProgressName
{
    RsyncOptions::Progress progress;
    const char *name;
};

constexpr ProgressName kProgressNames[] = {
    {RsyncOptions::Progress::None, "none"},
    {RsyncOptions::Progress::PerFile, "per-file"},
    {RsyncOptions::Progress::Overall, "overall"},
};

struct ExistingName
{
    RsyncOptions::Existing existing;
    const char *name;
};

constexpr ExistingName kExistingNames[] = {
    {RsyncOptions::Existing::Replace, "replace"},
    {RsyncOptions::Existing::SkipNewer, "skip-newer"},
    {RsyncOptions::Existing::IgnoreExisting, "ignore-existing"},
};

// Boolean options: JSON/QSettings key and member. The order is the order in the JSON file.
struct BoolOption
{
    const char *key;
    bool RsyncOptions::*member;
};

constexpr BoolOption kBoolOptions[] = {
    {"archive", &RsyncOptions::archive},
    {"recursive", &RsyncOptions::recursive},
    {"links", &RsyncOptions::links},
    {"perms", &RsyncOptions::perms},
    {"times", &RsyncOptions::times},
    {"group", &RsyncOptions::group},
    {"owner", &RsyncOptions::owner},
    {"devices", &RsyncOptions::devices},
    {"hardLinks", &RsyncOptions::hardLinks},
    {"acls", &RsyncOptions::acls},
    {"xattrs", &RsyncOptions::xattrs},
    {"verbose", &RsyncOptions::verbose},
    {"human", &RsyncOptions::human},
    {"partial", &RsyncOptions::partial},
    {"checksum", &RsyncOptions::checksum},
    {"compress", &RsyncOptions::compress},
    {"contentsOnly", &RsyncOptions::contentsOnly},
    {"deleteExtraneous", &RsyncOptions::deleteExtraneous},
    {"removeSourceFiles", &RsyncOptions::removeSourceFiles},
    {"removeSourceDirs", &RsyncOptions::removeSourceDirs},
    {"dryRun", &RsyncOptions::dryRun},
    {"verifyAfter", &RsyncOptions::verifyAfter},
    {"useBwLimit", &RsyncOptions::useBwLimit},
    {"useRemoteShell", &RsyncOptions::useRemoteShell},
};

struct StringOption
{
    const char *key;
    QString RsyncOptions::*member;
};

constexpr StringOption kStringOptions[] = {
    {"program", &RsyncOptions::program},
    {"bwLimit", &RsyncOptions::bwLimit},
    {"remoteShell", &RsyncOptions::remoteShell},
    {"extraArguments", &RsyncOptions::extraArguments},
};

template<typename Entry>
QString namesList(const Entry (&entries)[3])
{
    QStringList names;
    for (const Entry &entry : entries)
        names << QLatin1String(entry.name);
    return names.join(QStringLiteral(", "));
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

// Reads an optional key of the expected type; a present key of a different type is an error.
bool readBool(const QJsonObject &json, const char *key, bool *out, QString *error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isBool()) {
        setError(error, QObject::tr("\"%1\" must be true or false.").arg(QLatin1String(key)));
        return false;
    }
    *out = value.toBool();
    return true;
}

bool readString(const QJsonObject &json, const char *key, QString *out, QString *error)
{
    const QJsonValue value = json.value(QLatin1String(key));
    if (value.isUndefined())
        return true;
    if (!value.isString()) {
        setError(error, QObject::tr("\"%1\" must be a string.").arg(QLatin1String(key)));
        return false;
    }
    *out = value.toString();
    return true;
}

} // namespace

QStringList RsyncOptions::arguments(int port) const
{
    // Single-letter options are grouped ("-avhP"), so the command looks like one typed by hand.
    QString flags;
    QStringList longOptions;
    if (archive) {
        flags += QLatin1Char('a');
    } else {
        const std::pair<bool, char> implied[] = {
            {recursive, 'r'}, {links, 'l'}, {perms, 'p'}, {times, 't'},
            {group, 'g'},     {owner, 'o'}, {devices, 'D'},
        };
        for (const auto &[enabled, flag] : implied) {
            if (enabled)
                flags += QLatin1Char(flag);
        }
    }
    if (hardLinks)
        flags += QLatin1Char('H');
    if (acls)
        flags += QLatin1Char('A');
    if (xattrs)
        flags += QLatin1Char('X');
    if (verbose)
        flags += QLatin1Char('v');
    if (human)
        flags += QLatin1Char('h');
    switch (progress) {
    case Progress::None:
        if (partial)
            longOptions << QStringLiteral("--partial");
        break;
    case Progress::PerFile:
        if (partial)
            flags += QLatin1Char('P'); // -P = --partial --progress
        else
            longOptions << QStringLiteral("--progress");
        break;
    case Progress::Overall:
        if (partial)
            longOptions << QStringLiteral("--partial");
        longOptions << QStringLiteral("--info=progress2");
        break;
    }
    if (checksum)
        flags += QLatin1Char('c');
    if (existing == Existing::SkipNewer)
        flags += QLatin1Char('u');
    if (compress)
        flags += QLatin1Char('z');
    if (dryRun)
        flags += QLatin1Char('n');
    if (existing == Existing::IgnoreExisting)
        longOptions << QStringLiteral("--ignore-existing");
    if (deleteExtraneous)
        longOptions << QStringLiteral("--delete");
    if (removeSourceFiles)
        longOptions << QStringLiteral("--remove-source-files");
    if (useBwLimit)
        longOptions << QStringLiteral("--bwlimit=") + bwLimit.trimmed();

    QStringList args;
    if (!flags.isEmpty())
        args << QLatin1Char('-') + flags;
    args << longOptions << remoteShellArguments(port) << extraArgumentList();
    return args;
}

QStringList RsyncOptions::verifyArguments(int port) const
{
    // Only the content and the presence of entries are compared: -c forces reading both sides,
    // -n guarantees nothing is changed, -i lists every entry that would be transferred.
    QString flags = QStringLiteral("n");
    if (archive || recursive)
        flags += QLatin1Char('r');
    if (archive || links)
        flags += QLatin1Char('l');
    flags += QStringLiteral("ci");
    return QStringList{QLatin1Char('-') + flags, QStringLiteral("--out-format=%i %n%L")}
        << remoteShellArguments(port) << extraArgumentList();
}

QString RsyncOptions::summaryFlags() const
{
    QString flags;
    if (archive) {
        flags += QLatin1Char('a');
    } else {
        const std::pair<bool, char> implied[] = {
            {recursive, 'r'}, {links, 'l'}, {perms, 'p'}, {times, 't'},
            {group, 'g'},     {owner, 'o'}, {devices, 'D'},
        };
        for (const auto &[enabled, flag] : implied) {
            if (enabled)
                flags += QLatin1Char(flag);
        }
    }
    const std::pair<bool, char> others[] = {
        {hardLinks, 'H'}, {acls, 'A'}, {xattrs, 'X'}, {checksum, 'c'},
        {existing == Existing::SkipNewer, 'u'}, {compress, 'z'},
    };
    for (const auto &[enabled, flag] : others) {
        if (enabled)
            flags += QLatin1Char(flag);
    }
    QStringList parts;
    if (!flags.isEmpty())
        parts << QLatin1Char('-') + flags;
    if (existing == Existing::IgnoreExisting)
        parts << QStringLiteral("--ignore-existing");
    if (deleteExtraneous)
        parts << QStringLiteral("--delete");
    return parts.join(QLatin1Char(' '));
}

QString RsyncOptions::label(const QString &name) const
{
    const QString flags = summaryFlags();
    return flags.isEmpty() ? name : QStringLiteral("%1 (%2)").arg(name, flags);
}

QStringList RsyncOptions::remoteShellArguments(int port) const
{
    QString shell = useRemoteShell ? remoteShell.trimmed() : QString();
    if (port > 0)
        shell = (shell.isEmpty() ? QStringLiteral("ssh") : shell) + QStringLiteral(" -p ") + QString::number(port);
    if (shell.isEmpty())
        return {};
    return {QStringLiteral("-e"), shell};
}

QStringList RsyncOptions::extraArgumentList() const
{
    QStringList args;
    splitArguments(extraArguments, &args);
    return args;
}

QString RsyncOptions::validate() const
{
    if (program.trimmed().isEmpty())
        return QObject::tr("No rsync program specified.");
    const QString resolved = program.contains(QLatin1Char('/'))
        ? program
        : QStandardPaths::findExecutable(program);
    const QFileInfo programInfo(resolved);
    if (resolved.isEmpty() || !programInfo.isFile() || !programInfo.isExecutable())
        return QObject::tr("Program \"%1\" does not exist or is not executable.").arg(program);
    if (useBwLimit && !isValidBwLimit(bwLimit))
        return QObject::tr("Invalid bandwidth limit \"%1\" (examples: 500, 1.5M, 10m, 100K).").arg(bwLimit);
    if (useRemoteShell && remoteShell.trimmed().isEmpty())
        return QObject::tr("The remote shell command is empty.");
    QStringList extra;
    if (!splitArguments(extraArguments, &extra))
        return QObject::tr("Additional arguments: unbalanced quotes.");
    if (deleteExtraneous && removeSourceFiles)
        return QObject::tr("--delete and --remove-source-files cannot be used together here.");
    return {};
}

void RsyncOptions::save(QSettings &settings) const
{
    settings.beginGroup(QStringLiteral("options"));
    for (const BoolOption &option : kBoolOptions)
        settings.setValue(QLatin1String(option.key), this->*option.member);
    for (const StringOption &option : kStringOptions)
        settings.setValue(QLatin1String(option.key), this->*option.member);
    settings.setValue(QStringLiteral("progress"), progressName(progress));
    settings.setValue(QStringLiteral("existing"), existingName(existing));
    settings.endGroup();
}

RsyncOptions RsyncOptions::load(QSettings &settings)
{
    RsyncOptions o;
    settings.beginGroup(QStringLiteral("options"));
    for (const BoolOption &option : kBoolOptions)
        o.*option.member = settings.value(QLatin1String(option.key), o.*option.member).toBool();
    for (const StringOption &option : kStringOptions)
        o.*option.member = settings.value(QLatin1String(option.key), o.*option.member).toString();
    progressFromName(settings.value(QStringLiteral("progress")).toString(), &o.progress);
    existingFromName(settings.value(QStringLiteral("existing")).toString(), &o.existing);
    settings.endGroup();
    return o;
}

QJsonObject RsyncOptions::toJson() const
{
    QJsonObject json;
    for (const StringOption &option : kStringOptions)
        json.insert(QLatin1String(option.key), this->*option.member);
    for (const BoolOption &option : kBoolOptions)
        json.insert(QLatin1String(option.key), this->*option.member);
    json.insert(QStringLiteral("progress"), progressName(progress));
    json.insert(QStringLiteral("existing"), existingName(existing));
    return json;
}

RsyncOptions RsyncOptions::fromJson(const QJsonObject &json, QString *error)
{
    return fromJson(json, error, RsyncOptions());
}

RsyncOptions RsyncOptions::fromJson(const QJsonObject &json, QString *error, const RsyncOptions &base)
{
    RsyncOptions o = base;
    setError(error, QString());

    for (const BoolOption &option : kBoolOptions) {
        if (!readBool(json, option.key, &(o.*option.member), error))
            return {};
    }
    for (const StringOption &option : kStringOptions) {
        if (!readString(json, option.key, &(o.*option.member), error))
            return {};
    }

    const QJsonValue progress = json.value(QStringLiteral("progress"));
    if (!progress.isUndefined() && !progressFromName(progress.toString(), &o.progress)) {
        setError(error, QObject::tr("\"progress\" must be one of: %1.").arg(namesList(kProgressNames)));
        return {};
    }
    const QJsonValue existing = json.value(QStringLiteral("existing"));
    if (!existing.isUndefined() && !existingFromName(existing.toString(), &o.existing)) {
        setError(error, QObject::tr("\"existing\" must be one of: %1.").arg(namesList(kExistingNames)));
        return {};
    }
    return o;
}

bool RsyncOptions::saveToFile(const QString &path, QString *error) const
{
    const QJsonObject root{
        {QStringLiteral("format"), kJsonFormat},
        {QStringLiteral("version"), JsonFormatVersion},
        {QStringLiteral("options"), toJson()},
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        setError(error, QObject::tr("Cannot open \"%1\" for writing: %2").arg(path, file.errorString()));
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        setError(error, QObject::tr("Cannot write \"%1\": %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

RsyncOptions RsyncOptions::loadFromFile(const QString &path, QString *error)
{
    return loadFromFile(path, error, RsyncOptions());
}

RsyncOptions RsyncOptions::loadFromFile(const QString &path, QString *error, const RsyncOptions &base,
                                        QString *name)
{
    setError(error, QString());
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QObject::tr("Cannot open \"%1\": %2").arg(path, file.errorString()));
        return {};
    }
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        setError(error, QObject::tr("\"%1\" is not valid JSON: %2 (offset %3)")
                            .arg(path, parseError.errorString())
                            .arg(parseError.offset));
        return {};
    }
    if (!doc.isObject()) {
        setError(error, QObject::tr("\"%1\" does not contain a JSON object.").arg(path));
        return {};
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != kJsonFormat) {
        setError(error, QObject::tr("\"%1\" is not an Rsync GUI settings file.").arg(path));
        return {};
    }
    const int version = root.value(QStringLiteral("version")).toInt(0);
    if (version < 1 || version > JsonFormatVersion) {
        setError(error, QObject::tr("Unsupported settings file version %1 (supported: %2).")
                            .arg(version)
                            .arg(JsonFormatVersion));
        return {};
    }
    const QJsonValue options = root.value(QStringLiteral("options"));
    if (!options.isObject()) {
        setError(error, QObject::tr("\"%1\" has no \"options\" object.").arg(path));
        return {};
    }
    QString fileName;
    if (!readString(root, "name", &fileName, error))
        return {};
    const RsyncOptions result = fromJson(options.toObject(), error, base);
    if (name)
        *name = fileName;
    return result;
}

QStringList RsyncOptions::presetSearchPaths()
{
    QStringList paths;
    // User and system data directories ($XDG_DATA_HOME, $XDG_DATA_DIRS).
    for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation))
        paths << dir + QStringLiteral("/rsync-gui/presets");
    // Relative to the executable: <prefix>/bin/../share/... for any install prefix, and the build directory.
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        paths << QDir::cleanPath(appDir + QStringLiteral("/../share/rsync-gui/presets"))
              << appDir + QStringLiteral("/presets");
    }
    paths.removeDuplicates();
    return paths;
}

QString RsyncOptions::locatePreset(const QString &name, const QStringList &searchPaths)
{
    if (name.isEmpty())
        return {};
    if (name.contains(QLatin1Char('/')))
        return QFileInfo(name).isFile() ? QFileInfo(name).absoluteFilePath() : QString();
    const QString fileName = name.endsWith(QStringLiteral(".json")) ? name : name + QStringLiteral(".json");
    for (const QString &dir : searchPaths) {
        const QFileInfo file(QDir(dir).filePath(fileName));
        if (file.isFile())
            return file.absoluteFilePath();
    }
    return {};
}

QMap<QString, QString> RsyncOptions::availablePresets(const QStringList &searchPaths)
{
    QMap<QString, QString> presets;
    for (const QString &dir : searchPaths) {
        const QFileInfoList files = QDir(dir).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
        for (const QFileInfo &file : files) {
            if (!presets.contains(file.completeBaseName()))
                presets.insert(file.completeBaseName(), file.absoluteFilePath());
        }
    }
    return presets;
}

QList<RsyncPreset> RsyncOptions::presets(const QStringList &searchPaths)
{
    QList<RsyncPreset> list;
    const QMap<QString, QString> files = availablePresets(searchPaths);
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        RsyncPreset preset;
        preset.id = it.key();
        preset.file = it.value();
        preset.options = loadFromFile(preset.file, &preset.error, RsyncOptions(), &preset.name);
        if (preset.name.isEmpty())
            preset.name = preset.id;
        preset.label = preset.error.isEmpty() ? preset.options.label(preset.name) : preset.name;
        list << preset;
    }
    std::sort(list.begin(), list.end(), [](const RsyncPreset &a, const RsyncPreset &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return list;
}

bool RsyncOptions::isValidBwLimit(const QString &limit)
{
    // rsync accepts a number (KiB/s by default) with an optional unit suffix.
    static const QRegularExpression re(QStringLiteral("^[0-9]+(\\.[0-9]+)?([bBkKmMgGtTpP](i?[bB])?)?$"));
    return re.match(limit.trimmed()).hasMatch();
}

QString RsyncOptions::progressName(Progress progress)
{
    for (const ProgressName &entry : kProgressNames) {
        if (entry.progress == progress)
            return QLatin1String(entry.name);
    }
    return {};
}

bool RsyncOptions::progressFromName(const QString &name, Progress *progress)
{
    for (const ProgressName &entry : kProgressNames) {
        if (name == QLatin1String(entry.name)) {
            *progress = entry.progress;
            return true;
        }
    }
    return false;
}

QString RsyncOptions::existingName(Existing existing)
{
    for (const ExistingName &entry : kExistingNames) {
        if (entry.existing == existing)
            return QLatin1String(entry.name);
    }
    return {};
}

bool RsyncOptions::existingFromName(const QString &name, Existing *existing)
{
    for (const ExistingName &entry : kExistingNames) {
        if (name == QLatin1String(entry.name)) {
            *existing = entry.existing;
            return true;
        }
    }
    return false;
}

bool RsyncOptions::operator==(const RsyncOptions &other) const
{
    for (const BoolOption &option : kBoolOptions) {
        if (this->*option.member != other.*option.member)
            return false;
    }
    for (const StringOption &option : kStringOptions) {
        if (this->*option.member != other.*option.member)
            return false;
    }
    return progress == other.progress && existing == other.existing;
}

QString shellQuote(const QString &arg)
{
    if (!LocalPath::isPlain(arg)) {
        // Bytes that are not valid UTF-8: bash's $'...' quoting with \xNN keeps them exact.
        QString quoted = LocalPath::display(QString(arg).replace(QLatin1Char('\\'), QStringLiteral("\\\\")));
        quoted.replace(QLatin1Char('\''), QStringLiteral("\\'"));
        return QStringLiteral("$'") + quoted + QLatin1Char('\'');
    }
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (!arg.isEmpty() && safe.match(arg).hasMatch())
        return arg;
    QString quoted = arg;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

QString commandLine(const QString &program, const QStringList &args)
{
    QStringList parts{shellQuote(program)};
    for (const QString &arg : args)
        parts << shellQuote(arg);
    return parts.join(QLatin1Char(' '));
}

bool splitArguments(const QString &text, QStringList *out)
{
    QStringList args;
    QString current;
    bool inArgument = false;
    QChar quote;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (quote == QLatin1Char('\'')) {
            if (c == QLatin1Char('\''))
                quote = QChar();
            else
                current += c;
        } else if (quote == QLatin1Char('"')) {
            if (c == QLatin1Char('"')) {
                quote = QChar();
            } else if (c == QLatin1Char('\\') && i + 1 < text.size()
                       && QStringLiteral("\"\\$`").contains(text.at(i + 1))) {
                current += text.at(++i);
            } else {
                current += c;
            }
        } else if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
            inArgument = true;
        } else if (c == QLatin1Char('\\')) {
            if (i + 1 < text.size())
                current += text.at(++i);
            inArgument = true;
        } else if (c.isSpace()) {
            if (inArgument)
                args << current;
            current.clear();
            inArgument = false;
        } else {
            current += c;
            inArgument = true;
        }
    }
    if (!quote.isNull())
        return false;
    if (inArgument)
        args << current;
    *out = args;
    return true;
}
