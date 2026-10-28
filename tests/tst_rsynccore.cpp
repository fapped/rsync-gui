#include "location.h"
#include "rsyncjob.h"
#include "rsyncoptions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <sys/stat.h>
#include <utime.h>

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(data), data.size());
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

void setMtime(const QString &path, time_t mtime)
{
    struct utimbuf times;
    times.actime = mtime;
    times.modtime = mtime;
    QCOMPARE(::utime(QFile::encodeName(path).constData(), &times), 0);
}

bool runJob(RsyncJob &job)
{
    QSignalSpy spy(&job, &RsyncJob::finished);
    job.start();
    return spy.count() > 0 || spy.wait(60000);
}

bool haveRsync()
{
    return !QStandardPaths::findExecutable(QStringLiteral("rsync")).isEmpty();
}

// Options for tests: quiet (no progress) and without remembered user values.
RsyncOptions testOptions()
{
    RsyncOptions o;
    o.progress = RsyncOptions::Progress::None;
    return o;
}

RsyncJob::Plan localPlan(const QStringList &sources, const QString &destination, const RsyncOptions &options)
{
    QList<Location> locations;
    for (const QString &source : sources)
        locations << Location::parse(source);
    return RsyncJob::plan(locations, Location::parse(destination), options);
}

} // namespace

class TestRsyncCore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void defaultArguments();
    void arguments_data();
    void arguments();
    void verifyArguments();
    void remoteShell();
    void splitArgs_data();
    void splitArgs();
    void quoting();
    void validateOptions();
    void parseLocation_data();
    void parseLocation();
    void locationRoundTrip_data();
    void locationRoundTrip();
    void planGroupsSources();
    void planErrors();
    void planContentsOnly();
    void parseProgress_data();
    void parseProgress();
    void parseDifference_data();
    void parseDifference();
    void copyLocal();
    void copyWithVerify();
    void copyRltKeepsTimesNotPermissions();
    void verifyDetectsDifference();
    void dryRunChangesNothing();
    void moveRemovesSources();
    void removeEmptyDirectories();
    void moveRemovesEmptiedDirectories();
    void missingProgramAborts();
    void jsonRoundTrip();
    void jsonMissingKeysKeepBase();
    void jsonRejectsInvalidValues_data();
    void jsonRejectsInvalidValues();
    void jsonFileRoundTrip();
    void jsonFileRejectsInvalidFiles_data();
    void jsonFileRejectsInvalidFiles();
    void settingsRoundTrip();
    void locatePresets();
    void shippedPresets_data();
    void shippedPresets();
    void presetNamesMatchDolphinMenu();
};

void TestRsyncCore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

void TestRsyncCore::defaultArguments()
{
    // The defaults reproduce the "Copy (rsync)" command used before: rsync -avhP.
    QCOMPARE(RsyncOptions().arguments(), QStringList{QStringLiteral("-avhP")});
}

void TestRsyncCore::arguments_data()
{
    QTest::addColumn<QString>("change");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("checksum") << "checksum" << QStringList{"-avhPc"};
    QTest::newRow("no partial") << "nopartial" << QStringList{"-avh", "--progress"};
    QTest::newRow("overall") << "overall" << QStringList{"-avh", "--partial", "--info=progress2"};
    QTest::newRow("no progress") << "noprogress" << QStringList{"-avh", "--partial"};
    // Without -a its parts are listed one by one; by default they equal -a.
    QTest::newRow("no archive") << "noarchive" << QStringList{"-rlptgoDvhP"};
    QTest::newRow("rlt") << "rlt" << QStringList{"-rltvhP"};
    QTest::newRow("parts ignored with archive") << "archiveparts" << QStringList{"-avhP"};
    QTest::newRow("no archive, nothing preserved") << "flat" << QStringList{"-vhP"};
    QTest::newRow("preserve all") << "preserve" << QStringList{"-aHAXvhP"};
    QTest::newRow("update") << "update" << QStringList{"-avhPu"};
    QTest::newRow("ignore existing") << "ignore" << QStringList{"-avhP", "--ignore-existing"};
    QTest::newRow("compress, dry run") << "zn" << QStringList{"-avhPzn"};
    QTest::newRow("delete") << "delete" << QStringList{"-avhP", "--delete"};
    QTest::newRow("move") << "move" << QStringList{"-avhP", "--remove-source-files"};
    QTest::newRow("bwlimit") << "bwlimit" << QStringList{"-avhP", "--bwlimit=10M"};
    QTest::newRow("extra") << "extra"
                           << QStringList{"-avhP", "--exclude=*.tmp", "--exclude", "a b", "--mkpath"};
    QTest::newRow("nothing") << "nothing" << QStringList{};
}

void TestRsyncCore::arguments()
{
    QFETCH(QString, change);
    QFETCH(QStringList, expected);

    RsyncOptions o;
    if (change == "checksum") {
        o.checksum = true;
    } else if (change == "nopartial") {
        o.partial = false;
    } else if (change == "overall") {
        o.progress = RsyncOptions::Progress::Overall;
    } else if (change == "noprogress") {
        o.progress = RsyncOptions::Progress::None;
    } else if (change == "noarchive") {
        o.archive = false;
    } else if (change == "rlt") {
        o.archive = false;
        o.perms = o.group = o.owner = o.devices = false;
    } else if (change == "archiveparts") {
        o.perms = o.group = o.owner = o.devices = false;
    } else if (change == "flat") {
        o.archive = false;
        o.recursive = o.links = o.perms = o.times = o.group = o.owner = o.devices = false;
    } else if (change == "preserve") {
        o.hardLinks = o.acls = o.xattrs = true;
    } else if (change == "update") {
        o.existing = RsyncOptions::Existing::SkipNewer;
    } else if (change == "ignore") {
        o.existing = RsyncOptions::Existing::IgnoreExisting;
    } else if (change == "zn") {
        o.compress = o.dryRun = true;
    } else if (change == "delete") {
        o.deleteExtraneous = true;
    } else if (change == "move") {
        o.removeSourceFiles = true;
    } else if (change == "bwlimit") {
        o.useBwLimit = true;
        o.bwLimit = " 10M ";
    } else if (change == "extra") {
        o.extraArguments = "--exclude='*.tmp' --exclude \"a b\" --mkpath";
    } else if (change == "nothing") {
        o.archive = o.recursive = o.links = o.perms = o.times = o.group = o.owner = o.devices = false;
        o.verbose = o.human = o.partial = false;
        o.progress = RsyncOptions::Progress::None;
    }
    QCOMPARE(o.arguments(), expected);
}

void TestRsyncCore::verifyArguments()
{
    RsyncOptions o;
    o.extraArguments = "--exclude=.cache";
    QCOMPARE(o.verifyArguments(), (QStringList{"-nrlci", "--out-format=%i %n%L", "--exclude=.cache"}));
    o.archive = false;
    QCOMPARE(o.verifyArguments().first(), QStringLiteral("-nrlci")); // -l from the parts of -a
    o.links = false;
    QCOMPARE(o.verifyArguments().first(), QStringLiteral("-nrci"));
    o.recursive = false;
    QCOMPARE(o.verifyArguments().first(), QStringLiteral("-nci"));

    o.verifyAfter = true;
    QVERIFY(o.canVerify());
    o.dryRun = true;
    QVERIFY(!o.canVerify());
    o.dryRun = false;
    o.removeSourceFiles = true;
    QVERIFY(!o.canVerify());
}

void TestRsyncCore::remoteShell()
{
    RsyncOptions o;
    QCOMPARE(o.remoteShellArguments(0), QStringList());
    QCOMPARE(o.remoteShellArguments(2222), (QStringList{"-e", "ssh -p 2222"}));
    o.useRemoteShell = true;
    o.remoteShell = "ssh -i ~/.ssh/key";
    QCOMPARE(o.remoteShellArguments(0), (QStringList{"-e", "ssh -i ~/.ssh/key"}));
    QCOMPARE(o.remoteShellArguments(2222), (QStringList{"-e", "ssh -i ~/.ssh/key -p 2222"}));
    QCOMPARE(o.arguments(2222), (QStringList{"-avhP", "-e", "ssh -i ~/.ssh/key -p 2222"}));
    // The shell text is ignored while the option is off.
    o.useRemoteShell = false;
    QCOMPARE(o.remoteShellArguments(0), QStringList());
}

void TestRsyncCore::splitArgs_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("empty") << "" << true << QStringList{};
    QTest::newRow("spaces") << "  -a   -b  " << true << QStringList{"-a", "-b"};
    QTest::newRow("single quotes") << "--exclude='a b' x" << true << QStringList{"--exclude=a b", "x"};
    QTest::newRow("double quotes") << "\"a \\\"b\\\" c\"" << true << QStringList{"a \"b\" c"};
    QTest::newRow("backslash") << "a\\ b c" << true << QStringList{"a b", "c"};
    QTest::newRow("empty quoted") << "'' x" << true << QStringList{"", "x"};
    QTest::newRow("unbalanced") << "'abc" << false << QStringList{};
}

void TestRsyncCore::splitArgs()
{
    QFETCH(QString, text);
    QFETCH(bool, ok);
    QFETCH(QStringList, expected);
    QStringList out;
    QCOMPARE(splitArguments(text, &out), ok);
    if (ok)
        QCOMPARE(out, expected);
}

void TestRsyncCore::quoting()
{
    QCOMPARE(shellQuote("/tmp/a.txt"), QStringLiteral("/tmp/a.txt"));
    QCOMPARE(shellQuote("host:/a b"), QStringLiteral("'host:/a b'"));
    QCOMPARE(shellQuote("it's"), QStringLiteral("'it'\\''s'"));
    QCOMPARE(shellQuote(""), QStringLiteral("''"));
    QCOMPARE(commandLine("rsync", {"-avhP", "--", "/a b", "/c/"}), QStringLiteral("rsync -avhP -- '/a b' /c/"));
}

void TestRsyncCore::validateOptions()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    RsyncOptions o;
    QVERIFY(o.validate().isEmpty());

    RsyncOptions bad = o;
    bad.program = "/nonexistent/rsync";
    QVERIFY(!bad.validate().isEmpty());
    bad = o;
    bad.program = " ";
    QVERIFY(!bad.validate().isEmpty());

    bad = o;
    bad.useBwLimit = true;
    for (const char *limit : {"", "abc", "10 M", "-5"}) {
        bad.bwLimit = limit;
        QVERIFY2(!bad.validate().isEmpty(), limit);
    }
    for (const char *limit : {"500", "1.5M", "10m", "100K", "1MiB", "2G"}) {
        bad.bwLimit = limit;
        QVERIFY2(bad.validate().isEmpty(), limit);
    }

    bad = o;
    bad.useRemoteShell = true;
    QVERIFY(!bad.validate().isEmpty());
    bad.remoteShell = "ssh";
    QVERIFY(bad.validate().isEmpty());

    bad = o;
    bad.extraArguments = "--exclude='x";
    QVERIFY(!bad.validate().isEmpty());

    bad = o;
    bad.deleteExtraneous = bad.removeSourceFiles = true;
    QVERIFY(!bad.validate().isEmpty());
}

void TestRsyncCore::parseLocation_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("host");
    QTest::addColumn<int>("port");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("spec");

    const QString home = QDir::homePath();
    QTest::newRow("absolute") << "/tmp/a//b/" << "" << "" << 0 << "/tmp/a/b" << "/tmp/a/b";
    QTest::newRow("quoted") << "  '/tmp/a b'  " << "" << "" << 0 << "/tmp/a b" << "/tmp/a b";
    QTest::newRow("file url") << "file:///tmp/a%20b" << "" << "" << 0 << "/tmp/a b" << "/tmp/a b";
    QTest::newRow("tilde") << "~/x" << "" << "" << 0 << home + "/x" << home + "/x";
    QTest::newRow("home") << "~" << "" << "" << 0 << home << home;
    QTest::newRow("sftp") << "sftp://nas/volume1/a%20b" << "" << "nas" << 0 << "/volume1/a b" << "nas:/volume1/a b";
    QTest::newRow("sftp user port") << "sftp://me@nas:2222/data/" << "me" << "nas" << 2222 << "/data/"
                                    << "me@nas:/data/";
    QTest::newRow("sftp home") << "sftp://nas/~/docs" << "" << "nas" << 0 << "docs" << "nas:docs";
    QTest::newRow("rsync remote") << "me@host:/srv/x" << "me" << "host" << 0 << "/srv/x" << "me@host:/srv/x";
    QTest::newRow("rsync relative") << "host:docs" << "" << "host" << 0 << "docs" << "host:docs";
    QTest::newRow("rsync home") << "host:" << "" << "host" << 0 << "." << "host:.";
    QTest::newRow("dot path with colon") << "./a:b" << "" << "" << 0 << QDir::cleanPath(QDir::currentPath() + "/a:b")
                                         << QDir::cleanPath(QDir::currentPath() + "/a:b");
    QTest::newRow("absolute with colon") << "/tmp/a:b" << "" << "" << 0 << "/tmp/a:b" << "/tmp/a:b";
}

void TestRsyncCore::parseLocation()
{
    QFETCH(QString, text);
    QFETCH(QString, user);
    QFETCH(QString, host);
    QFETCH(int, port);
    QFETCH(QString, path);
    QFETCH(QString, spec);

    const Location loc = Location::parse(text);
    QVERIFY(loc.isValid());
    QCOMPARE(loc.user, user);
    QCOMPARE(loc.host, host);
    QCOMPARE(loc.port, port);
    QCOMPARE(loc.path, path);
    QCOMPARE(loc.rsyncSpec(), spec);
    QCOMPARE(loc.isRemote(), !host.isEmpty());
}

void TestRsyncCore::locationRoundTrip_data()
{
    QTest::addColumn<QString>("text");
    QTest::newRow("local") << "/tmp/a b";
    QTest::newRow("remote") << "me@host:/a b";
    QTest::newRow("relative remote") << "host:docs";
    QTest::newRow("sftp port") << "sftp://me@nas:2222/data/x%20y";
    QTest::newRow("sftp port home") << "sftp://nas:2222/~/docs";
}

void TestRsyncCore::locationRoundTrip()
{
    QFETCH(QString, text);
    const Location loc = Location::parse(text);
    QVERIFY(loc.isValid());
    QCOMPARE(Location::parse(loc.toString()), loc);
    QVERIFY(!Location::parse(QString()).isValid());
    QVERIFY(!Location::parse(QStringLiteral("   ")).isValid());
}

void TestRsyncCore::planGroupsSources()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("a"), "a");
    writeFile(tmp.filePath("dir/b"), "b");
    const QString dest = tmp.filePath("dest");

    const RsyncJob::Plan plan = localPlan(
        {tmp.filePath("a"), "sftp://me@nas:2222/x", tmp.filePath("dir"), "nas:/y", "sftp://me@nas:2222/z",
         tmp.filePath("a"), tmp.filePath("missing")},
        dest, RsyncOptions());
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.size(), 3);
    QCOMPARE(plan.transfers.at(0).sources, (QStringList{tmp.filePath("a"), tmp.filePath("dir")}));
    QCOMPARE(plan.transfers.at(0).destination, dest + "/");
    QCOMPARE(plan.transfers.at(0).port, 0);
    QCOMPARE(plan.transfers.at(1).sources, (QStringList{"me@nas:/x", "me@nas:/z"}));
    QCOMPARE(plan.transfers.at(1).port, 2222);
    QCOMPARE(plan.transfers.at(2).sources, QStringList{"nas:/y"});
    QCOMPARE(plan.missing, QStringList{tmp.filePath("missing")});
    QCOMPARE(plan.notes.size(), 1); // the duplicate

    // A remote destination gets its port from the destination.
    const RsyncJob::Plan upload = localPlan({tmp.filePath("a")}, "sftp://nas:2200/backup", RsyncOptions());
    QVERIFY(upload.ok());
    QCOMPARE(upload.transfers.size(), 1);
    QCOMPARE(upload.transfers.first().destination, QStringLiteral("nas:/backup/"));
    QCOMPARE(upload.transfers.first().port, 2200);
}

void TestRsyncCore::planErrors()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("dir/b"), "b");

    QVERIFY(!localPlan({tmp.filePath("dir")}, QString(), RsyncOptions()).ok());
    QVERIFY(!localPlan({}, tmp.filePath("dest"), RsyncOptions()).ok());
    QVERIFY(!localPlan({tmp.filePath("missing")}, tmp.filePath("dest"), RsyncOptions()).ok());
    // Remote to remote is not supported by rsync.
    QVERIFY(!localPlan({"host1:/a"}, "host2:/b", RsyncOptions()).ok());
    // Copying a directory into itself or below itself.
    QVERIFY(!localPlan({tmp.filePath("dir")}, tmp.filePath("dir"), RsyncOptions()).ok());
    QVERIFY(!localPlan({tmp.filePath("dir")}, tmp.filePath("dir/sub/x"), RsyncOptions()).ok());
    // A sibling with a common prefix is fine.
    QVERIFY(localPlan({tmp.filePath("dir")}, tmp.filePath("dir2"), RsyncOptions()).ok());
    // Copying a file into its own directory is a no-op for rsync, not an error.
    QVERIFY(localPlan({tmp.filePath("dir/b")}, tmp.filePath("dir"), RsyncOptions()).ok());
}

void TestRsyncCore::planContentsOnly()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("dir/b"), "b");
    writeFile(tmp.filePath("file"), "f");
    RsyncOptions o;
    o.contentsOnly = true;
    const RsyncJob::Plan plan = localPlan({tmp.filePath("dir"), tmp.filePath("file"), "host:/remote"},
                                          "/backup", o);
    QVERIFY(plan.ok());
    QCOMPARE(plan.transfers.size(), 2);
    QCOMPARE(plan.transfers.at(0).sources, (QStringList{tmp.filePath("dir") + "/", tmp.filePath("file")}));
    QCOMPARE(plan.transfers.at(1).sources, QStringList{"host:/remote"}); // remote paths are passed as typed
}

void TestRsyncCore::parseProgress_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("ok");
    QTest::addColumn<int>("percent");
    QTest::addColumn<int>("remaining");
    QTest::addColumn<int>("total");
    QTest::addColumn<bool>("final");

    QTest::newRow("start") << "         32,77K   1%    0,00kB/s    0:00:00" << true << 1 << -1 << -1 << false;
    QTest::newRow("file done") << "          3,00M  99%    2,76GB/s    0:00:00 (xfr#1, to-chk=3/5)"
                               << true << 99 << 3 << 5 << true;
    QTest::newRow("incremental") << "  1,048,576 100%   50.00MB/s    0:00:00 (xfr#12, ir-chk=1000/2500)"
                                 << true << 100 << 1000 << 2500 << false;
    QTest::newRow("progress2") << "    123.45M  45%   12.34MB/s    0:01:23  " << true << 45 << -1 << -1 << false;
    QTest::newRow("file name") << "dir/file 1% 2" << false << 0 << 0 << 0 << false;
    QTest::newRow("summary") << "sent 3,00M bytes  received 69 bytes  6,00M bytes/sec" << false << 0 << 0 << 0 << false;
    QTest::newRow("name with percent") << "50% 1 2 0:00:00 x" << false << 0 << 0 << 0 << false;
}

void TestRsyncCore::parseProgress()
{
    QFETCH(QString, line);
    QFETCH(bool, ok);
    RsyncJob::ProgressInfo info;
    QCOMPARE(RsyncJob::parseProgressLine(line, &info), ok);
    if (!ok)
        return;
    QFETCH(int, percent);
    QFETCH(int, remaining);
    QFETCH(int, total);
    QFETCH(bool, final);
    QCOMPARE(info.percent, percent);
    QCOMPARE(info.remaining, remaining);
    QCOMPARE(info.total, total);
    QCOMPARE(info.totalFinal, final);
}

void TestRsyncCore::parseDifference_data()
{
    QTest::addColumn<QString>("line");
    QTest::addColumn<bool>("differs");
    QTest::addColumn<QString>("name");

    QTest::newRow("missing file") << ">f+++++++++ src/big" << true << "src/big";
    QTest::newRow("content") << ">fc.T...... src/sub/a b" << true << "src/sub/a b";
    QTest::newRow("missing dir") << "cd+++++++++ src/new/" << true << "src/new/";
    QTest::newRow("symlink") << "cL+++++++++ src/link -> target" << true << "src/link -> target";
    QTest::newRow("attributes only") << ".d..t...... src/" << false << "";
    QTest::newRow("deleting") << "*deleting   src/old" << false << "";
    QTest::newRow("other output") << "sending incremental file list" << false << "";
}

void TestRsyncCore::parseDifference()
{
    QFETCH(QString, line);
    QFETCH(bool, differs);
    QFETCH(QString, name);
    QString parsed;
    QCOMPARE(RsyncJob::parseDifferenceLine(line, &parsed), differs);
    if (differs)
        QCOMPARE(parsed, name);
}

void TestRsyncCore::copyLocal()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/dir/a.txt"), "aaa");
    writeFile(tmp.filePath("src/dir/sub/b c.txt"), "bbb");
    writeFile(tmp.filePath("src/single"), "single");
    QVERIFY(QFile::link("a.txt", tmp.filePath("src/dir/link")));
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.progress = RsyncOptions::Progress::PerFile; // exercise the progress parsing
    const RsyncJob::Plan plan = localPlan({tmp.filePath("src/dir"), tmp.filePath("src/single")}, dest, options);
    QVERIFY(plan.ok());
    RsyncJob job(plan.transfers, options);
    QSignalSpy progress(&job, &RsyncJob::progress);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().transfersOk, 1);
    QCOMPARE(progress.last().first().toInt(), 1000);

    QCOMPARE(readFile(dest + "/dir/a.txt"), QByteArray("aaa"));
    QCOMPARE(readFile(dest + "/dir/sub/b c.txt"), QByteArray("bbb"));
    QCOMPARE(readFile(dest + "/single"), QByteArray("single"));
    QVERIFY(QFileInfo(dest + "/dir/link").isSymLink());
    QCOMPARE(QFileInfo(dest + "/dir/link").symLinkTarget(), dest + "/dir/a.txt");
}

void TestRsyncCore::copyWithVerify()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");
    writeFile(tmp.filePath("src/sub/b"), "bbb");
    const QString dest = tmp.filePath("dest");

    RsyncOptions options = testOptions();
    options.checksum = true;
    options.verifyAfter = true;
    options.extraArguments = "--mkpath";
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 1);
    QCOMPARE(job.summary().differences, 0);
    QCOMPARE(readFile(dest + "/src/sub/b"), QByteArray("bbb"));
}

void TestRsyncCore::copyRltKeepsTimesNotPermissions()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");
    QVERIFY(QFile::link("a", tmp.filePath("src/link")));
    QVERIFY(::chmod(QFile::encodeName(tmp.filePath("src/a")).constData(), 0777) == 0);
    setMtime(tmp.filePath("src/a"), 1600000000);
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.archive = false;
    options.perms = options.group = options.owner = options.devices = false;
    // Without -p new files get the source permissions masked by the umask; with -p exactly 0777.
    const mode_t oldMask = ::umask(022);
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    const bool finished = runJob(job);
    ::umask(oldMask);
    QVERIFY(finished);
    QVERIFY(job.summary().ok());
    const QFileInfo copy(dest + "/src/a");
    QCOMPARE(copy.lastModified().toSecsSinceEpoch(), 1600000000);
    struct stat st;
    QCOMPARE(::stat(QFile::encodeName(copy.filePath()).constData(), &st), 0);
    QCOMPARE(st.st_mode & 07777, mode_t(0755));
    QVERIFY(QFileInfo(dest + "/src/link").isSymLink());
}

void TestRsyncCore::verifyDetectsDifference()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/same"), "same");
    writeFile(tmp.filePath("src/changed"), "original");
    const QString dest = tmp.filePath("dest");
    // A corrupted copy with the same size and modification time: the quick check of rsync skips it,
    // only the checksum verification notices it.
    writeFile(dest + "/src/changed", "CORRUPT!");
    setMtime(tmp.filePath("src/changed"), 1700000000);
    setMtime(dest + "/src/changed", 1700000000);

    RsyncOptions options = testOptions();
    options.verifyAfter = true;
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QVERIFY(runJob(job));
    QCOMPARE(job.summary().transfersOk, 1);
    QCOMPARE(job.summary().differences, 1);
    QVERIFY(!job.summary().ok());
    QCOMPARE(job.differingFiles(), QStringList{"src/changed"});
    QCOMPARE(readFile(dest + "/src/changed"), QByteArray("CORRUPT!"));

    // With -c the copy itself fixes the file and the verification passes.
    options.checksum = true;
    RsyncJob fix(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QVERIFY(runJob(fix));
    QVERIFY(fix.summary().ok());
    QCOMPARE(readFile(dest + "/src/changed"), QByteArray("original"));
}

void TestRsyncCore::dryRunChangesNothing()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.dryRun = true;
    options.verifyAfter = true; // ignored for a dry run
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 0);
    QVERIFY(!QFileInfo::exists(dest + "/src"));
}

void TestRsyncCore::moveRemovesSources()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");
    writeFile(tmp.filePath("file"), "f");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.removeSourceFiles = true;
    RsyncJob job(localPlan({tmp.filePath("src"), tmp.filePath("file")}, dest, options).transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(readFile(dest + "/src/a"), QByteArray("aaa"));
    QCOMPARE(readFile(dest + "/file"), QByteArray("f"));
    QVERIFY(!QFileInfo::exists(tmp.filePath("src/a")));
    QVERIFY(!QFileInfo::exists(tmp.filePath("file")));
    QVERIFY(QFileInfo(tmp.filePath("src")).isDir()); // rsync leaves directories in place
}

void TestRsyncCore::removeEmptyDirectories()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = tmp.filePath("root");
    QVERIFY(QDir().mkpath(root + "/a/b/c"));
    QVERIFY(QDir().mkpath(root + "/e"));
    writeFile(root + "/d/file", "keep");
    QVERIFY(QDir().mkpath(tmp.filePath("outside/empty")));
    QVERIFY(QFile::link(tmp.filePath("outside"), root + "/a/link")); // keeps "a", is not followed

    QStringList left;
    QCOMPARE(RsyncJob::removeEmptyDirectories(root, false, &left), 3); // c, b, e
    QVERIFY(!QFileInfo::exists(root + "/a/b"));
    QVERIFY(!QFileInfo::exists(root + "/e"));
    QVERIFY(QFileInfo(root + "/a/link").isSymLink());
    QVERIFY(QFileInfo::exists(tmp.filePath("outside/empty")));
    QCOMPARE(readFile(root + "/d/file"), QByteArray("keep"));
    QCOMPARE(left, QStringList{root}); // only the outermost directory that stays is reported

    QVERIFY(QFile::remove(root + "/a/link"));
    QVERIFY(QFile::remove(root + "/d/file"));
    QCOMPARE(RsyncJob::removeEmptyDirectories(root, true, &left), 2); // a, d - the root is kept
    QVERIFY(QFileInfo(root).isDir());
    QCOMPARE(RsyncJob::removeEmptyDirectories(root, false), 1);
    QVERIFY(!QFileInfo::exists(root));
    QCOMPARE(RsyncJob::removeEmptyDirectories(tmp.filePath("missing"), false), 0);
}

void TestRsyncCore::moveRemovesEmptiedDirectories()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a/1"), "1");
    writeFile(tmp.filePath("src/a/b/2"), "2");
    writeFile(tmp.filePath("src/keep/skip.tmp"), "excluded");
    writeFile(tmp.filePath("contents/x/3"), "3");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    options.extraArguments = "--exclude=*.tmp";
    const RsyncJob::Plan plan = localPlan({tmp.filePath("src")}, dest, options);
    QCOMPARE(plan.transfers.first().sourceDirs, QStringList{tmp.filePath("src")});
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(readFile(dest + "/src/a/b/2"), QByteArray("2"));
    QCOMPARE(job.summary().sourceDirsRemoved, 2); // a/b, a
    QVERIFY(!QFileInfo::exists(tmp.filePath("src/a")));
    QCOMPARE(readFile(tmp.filePath("src/keep/skip.tmp")), QByteArray("excluded")); // not moved, stays

    // Contents only ("dir/"): the source directory itself stays.
    options.contentsOnly = true;
    options.extraArguments.clear();
    RsyncJob contents(localPlan({tmp.filePath("contents")}, dest, options).transfers, options);
    QVERIFY(runJob(contents));
    QVERIFY(contents.summary().ok());
    QCOMPARE(readFile(dest + "/x/3"), QByteArray("3"));
    QVERIFY(!QFileInfo::exists(tmp.filePath("contents/x")));
    QVERIFY(QFileInfo(tmp.filePath("contents")).isDir());

    // Without the option rsync's behaviour is unchanged: directories stay.
    writeFile(tmp.filePath("plain/y/4"), "4");
    options.contentsOnly = false;
    options.removeSourceDirs = false;
    RsyncJob plain(localPlan({tmp.filePath("plain")}, dest, options).transfers, options);
    QVERIFY(runJob(plain));
    QVERIFY(QFileInfo(tmp.filePath("plain/y")).isDir());
    QCOMPARE(plain.summary().sourceDirsRemoved, 0);
}

void TestRsyncCore::missingProgramAborts()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("a"), "a");
    RsyncOptions options = testOptions();
    options.program = "/nonexistent/rsync";
    RsyncJob job(localPlan({tmp.filePath("a")}, tmp.filePath("dest"), options).transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().aborted);
    QVERIFY(!job.summary().ok());
}

void TestRsyncCore::jsonRoundTrip()
{
    RsyncOptions o;
    o.program = "/usr/local/bin/rsync";
    o.archive = false;
    o.recursive = false;
    o.perms = o.owner = false;
    o.hardLinks = o.acls = o.xattrs = true;
    o.verbose = o.human = o.partial = false;
    o.progress = RsyncOptions::Progress::Overall;
    o.checksum = true;
    o.existing = RsyncOptions::Existing::IgnoreExisting;
    o.compress = o.contentsOnly = o.deleteExtraneous = o.dryRun = o.verifyAfter = true;
    o.useBwLimit = true;
    o.bwLimit = "10M";
    o.useRemoteShell = true;
    o.remoteShell = "ssh -i key";
    o.extraArguments = "--exclude='*.tmp'";

    QString error;
    const RsyncOptions copy = RsyncOptions::fromJson(o.toJson(), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(copy == o);
    QCOMPARE(o.toJson().value("progress").toString(), QStringLiteral("overall"));
    QCOMPARE(o.toJson().value("existing").toString(), QStringLiteral("ignore-existing"));
}

void TestRsyncCore::jsonMissingKeysKeepBase()
{
    RsyncOptions base;
    base.program = "/opt/rsync";
    base.remoteShell = "ssh -i key";
    base.useRemoteShell = true;
    QString error;
    const RsyncOptions o = RsyncOptions::fromJson(QJsonObject{{"checksum", true}, {"unknownKey", 5}}, &error, base);
    QVERIFY(error.isEmpty());
    QVERIFY(o.checksum);
    QCOMPARE(o.program, QStringLiteral("/opt/rsync"));
    QCOMPARE(o.remoteShell, QStringLiteral("ssh -i key"));
    QVERIFY(o.useRemoteShell);
}

void TestRsyncCore::jsonRejectsInvalidValues_data()
{
    QTest::addColumn<QJsonObject>("json");
    QTest::newRow("bool as string") << QJsonObject{{"checksum", "yes"}};
    QTest::newRow("string as number") << QJsonObject{{"program", 5}};
    QTest::newRow("unknown progress") << QJsonObject{{"progress", "fancy"}};
    QTest::newRow("progress as number") << QJsonObject{{"progress", 1}};
    QTest::newRow("unknown existing") << QJsonObject{{"existing", "merge"}};
}

void TestRsyncCore::jsonRejectsInvalidValues()
{
    QFETCH(QJsonObject, json);
    QString error;
    RsyncOptions::fromJson(json, &error);
    QVERIFY(!error.isEmpty());
}

void TestRsyncCore::jsonFileRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    RsyncOptions o;
    o.checksum = true;
    o.extraArguments = "--exclude \"a b\"";
    const QString file = tmp.filePath("settings.json");
    QString error;
    QVERIFY2(o.saveToFile(file, &error), qPrintable(error));
    QString name = "unchanged";
    const RsyncOptions loaded = RsyncOptions::loadFromFile(file, &error, RsyncOptions(), &name);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(loaded == o);
    QVERIFY(name.isEmpty());

    const QJsonObject root = QJsonDocument::fromJson(readFile(file)).object();
    QCOMPARE(root.value("format").toString(), QStringLiteral("rsync-gui-settings"));
    QCOMPARE(root.value("version").toInt(), RsyncOptions::JsonFormatVersion);
}

void TestRsyncCore::jsonFileRejectsInvalidFiles_data()
{
    QTest::addColumn<QByteArray>("content");
    QTest::newRow("not json") << QByteArray("hello");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("wrong format") << QByteArray(R"({"format":"shred-gui-settings","version":1,"options":{}})");
    QTest::newRow("newer version") << QByteArray(R"({"format":"rsync-gui-settings","version":99,"options":{}})");
    QTest::newRow("no options") << QByteArray(R"({"format":"rsync-gui-settings","version":1})");
    QTest::newRow("bad name") << QByteArray(R"({"format":"rsync-gui-settings","version":1,"name":1,"options":{}})");
    QTest::newRow("bad value") << QByteArray(R"({"format":"rsync-gui-settings","version":1,"options":{"dryRun":1}})");
}

void TestRsyncCore::jsonFileRejectsInvalidFiles()
{
    QFETCH(QByteArray, content);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString file = tmp.filePath("bad.json");
    writeFile(file, content);
    QString error;
    RsyncOptions::loadFromFile(file, &error);
    QVERIFY(!error.isEmpty());
    RsyncOptions::loadFromFile(tmp.filePath("missing.json"), &error);
    QVERIFY(!error.isEmpty());
}

void TestRsyncCore::settingsRoundTrip()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    RsyncOptions o;
    o.progress = RsyncOptions::Progress::None;
    o.existing = RsyncOptions::Existing::SkipNewer;
    o.verifyAfter = true;
    o.bwLimit = "1M";
    {
        QSettings settings(tmp.filePath("test.conf"), QSettings::IniFormat);
        o.save(settings);
    }
    QSettings settings(tmp.filePath("test.conf"), QSettings::IniFormat);
    QVERIFY(RsyncOptions::load(settings) == o);

    QSettings empty(tmp.filePath("empty.conf"), QSettings::IniFormat);
    QVERIFY(RsyncOptions::load(empty) == RsyncOptions());
}

void TestRsyncCore::locatePresets()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("user/one.json"), "{}");
    writeFile(tmp.filePath("system/one.json"), "{}");
    writeFile(tmp.filePath("system/two.json"), "{}");
    const QStringList paths{tmp.filePath("user"), tmp.filePath("system")};

    QCOMPARE(RsyncOptions::locatePreset("one", paths), tmp.filePath("user/one.json"));
    QCOMPARE(RsyncOptions::locatePreset("two.json", paths), tmp.filePath("system/two.json"));
    QVERIFY(RsyncOptions::locatePreset("three", paths).isEmpty());
    QCOMPARE(RsyncOptions::locatePreset(tmp.filePath("system/two.json"), paths), tmp.filePath("system/two.json"));
    const QMap<QString, QString> presets = RsyncOptions::availablePresets(paths);
    QCOMPARE(presets.keys(), (QStringList{"one", "two"}));
    QCOMPARE(presets.value("one"), tmp.filePath("user/one.json"));
}

void TestRsyncCore::shippedPresets_data()
{
    QTest::addColumn<QString>("preset");
    QTest::addColumn<QStringList>("arguments");
    QTest::addColumn<bool>("verify");
    // The presets reproduce the former Dolphin actions "Copy (rsync)" and "Copy (rsync + verify)".
    QTest::newRow("rsync-copy") << "rsync-copy" << QStringList{"-avhP"} << false;
    QTest::newRow("rsync-copy-verify") << "rsync-copy-verify" << QStringList{"-avhPc"} << true;
    QTest::newRow("rsync-copy-portable") << "rsync-copy-portable" << QStringList{"-rltvhP"} << false;
    QTest::newRow("rsync-move") << "rsync-move" << QStringList{"-avhP", "--remove-source-files"} << false;
}

void TestRsyncCore::shippedPresets()
{
    QFETCH(QString, preset);
    QFETCH(QStringList, arguments);
    QFETCH(bool, verify);

    const QString file = RsyncOptions::locatePreset(preset, {QStringLiteral(RSYNC_GUI_SOURCE_PRESETS_DIR)});
    QVERIFY(!file.isEmpty());
    // Presets must not override user-specific values such as the program or the remote shell.
    RsyncOptions base;
    base.program = "/opt/rsync";
    base.useRemoteShell = true;
    base.remoteShell = "ssh -i key";
    base.dryRun = true;
    QString error;
    QString name;
    const RsyncOptions o = RsyncOptions::loadFromFile(file, &error, base, &name);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!name.isEmpty());
    QCOMPARE(o.program, QStringLiteral("/opt/rsync"));
    QCOMPARE(o.remoteShell, QStringLiteral("ssh -i key"));
    QVERIFY(!o.dryRun);
    QVERIFY(o.useRemoteShell);
    RsyncOptions withoutShell = o;
    withoutShell.useRemoteShell = false;
    QCOMPARE(withoutShell.arguments(), arguments);
    QCOMPARE(o.canVerify(), verify);
}

void TestRsyncCore::presetNamesMatchDolphinMenu()
{
    const QList<RsyncPreset> presets = RsyncOptions::presets({QStringLiteral(RSYNC_GUI_SOURCE_PRESETS_DIR)});
    QVERIFY(presets.size() >= 3);
    QStringList names;
    for (const RsyncPreset &preset : presets) {
        QVERIFY2(preset.error.isEmpty(), qPrintable(preset.error));
        QVERIFY2(preset.name != preset.id, qPrintable(preset.id)); // every shipped preset has a name
        names << preset.name;
    }
    QStringList sorted = names;
    std::sort(sorted.begin(), sorted.end(), [](const QString &a, const QString &b) {
        return a.compare(b, Qt::CaseInsensitive) < 0;
    });
    QCOMPARE(names, sorted);

    // Every Dolphin action is "<preset name>…" and runs that preset; every preset has an action,
    // listed in the same order as in File > Load preset.
    // (QSettings cannot read .desktop files: ';' starts a comment there.)
    QHash<QString, QString> entries; // "group/key" -> value
    QString group;
    const QStringList lines = QString::fromUtf8(readFile(QStringLiteral(RSYNC_GUI_SOURCE_SERVICEMENU))).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
            group = line.mid(1, line.size() - 2);
        else if (const qsizetype eq = line.indexOf(QLatin1Char('=')); eq > 0)
            entries.insert(group + QLatin1Char('/') + line.left(eq), line.mid(eq + 1));
    }
    const QStringList actions = entries.value(QStringLiteral("Desktop Entry/Actions")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    QCOMPARE(actions.size(), presets.size());
    for (qsizetype i = 0; i < actions.size(); ++i) {
        const QString prefix = QStringLiteral("Desktop Action ") + actions.at(i) + QLatin1Char('/');
        const QString label = entries.value(prefix + QStringLiteral("Name"));
        const QString exec = entries.value(prefix + QStringLiteral("Exec"));
        QCOMPARE(label, presets.at(i).name + QStringLiteral("…"));
        QVERIFY2(exec.contains(QStringLiteral("--preset ") + presets.at(i).id + QLatin1Char(' ')), qPrintable(exec));
    }
}

QTEST_GUILESS_MAIN(TestRsyncCore)
#include "tst_rsynccore.moc"
