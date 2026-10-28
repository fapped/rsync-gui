#include "localpath.h"
#include "location.h"
#include "rsyncjob.h"
#include "rsyncoptions.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QTest>
#include <QTimer>

#include <cstring>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
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

// Files with names that are not valid UTF-8 are created and read with raw bytes.
void writeRaw(const QByteArray &path, const QByteArray &data)
{
    const int fd = ::open(path.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    QVERIFY(fd >= 0);
    QCOMPARE(::write(fd, data.constData(), data.size()), ssize_t(data.size()));
    ::close(fd);
}

QByteArray readRaw(const QByteArray &path)
{
    const int fd = ::open(path.constData(), O_RDONLY);
    if (fd < 0)
        return "<missing>";
    QByteArray data(4096, '\0');
    const ssize_t n = ::read(fd, data.data(), data.size());
    ::close(fd);
    return data.left(qMax<ssize_t>(0, n));
}

// QTemporaryDir cannot remove such names (Qt converts them), so they are removed with raw bytes.
void removeTree(const QByteArray &path)
{
    struct stat st;
    if (::lstat(path.constData(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *d = ::opendir(path.constData())) {
            QList<QByteArray> children;
            while (dirent *entry = ::readdir(d)) {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                    children << path + '/' + entry->d_name;
            }
            ::closedir(d);
            for (const QByteArray &child : std::as_const(children))
                removeTree(child);
        }
        ::rmdir(path.constData());
    } else {
        ::unlink(path.constData());
    }
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



// The bytes of a file that are in the page cache (mincore).
qint64 residentBytes(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() == 0)
        return -1;
    void *map = ::mmap(nullptr, size_t(file.size()), PROT_READ, MAP_SHARED, file.handle(), 0);
    if (map == MAP_FAILED)
        return -1;
    const long page = ::sysconf(_SC_PAGESIZE);
    std::vector<unsigned char> pages(size_t((file.size() + page - 1) / page));
    qint64 resident = -1;
    if (::mincore(map, size_t(file.size()), pages.data()) == 0) {
        resident = 0;
        for (unsigned char p : pages)
            resident += (p & 1) ? page : 0;
    }
    ::munmap(map, size_t(file.size()));
    return resident;
}

// The remote tests run only against a test ssh server: tools/ssh-test/run.sh starts one in a container
// and sets RSYNC_GUI_TEST_SSH (sftp://user@host:port/home) and RSYNC_GUI_TEST_SSH_SHELL (the ssh command).
struct RemoteServer
{
    Location home;
    QString shell;
    QString dir; // a fresh directory for one test, removed by the destructor

    explicit RemoteServer(const QString &testName)
    {
        home = Location::parse(qEnvironmentVariable("RSYNC_GUI_TEST_SSH"));
        shell = qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SHELL");
        if (isValid()) {
            dir = home.path + QStringLiteral("/") + testName + QLatin1Char('-')
                + QString::number(QRandomGenerator::global()->generate(), 16);
            run(QStringLiteral("mkdir -p '%1'").arg(dir));
        }
    }
    ~RemoteServer()
    {
        if (isValid())
            run(QStringLiteral("rm -rf '%1'").arg(dir));
    }
    bool isValid() const { return home.isRemote() && !shell.isEmpty(); }

    // A location on the server (inside the test directory).
    Location at(const QString &relative) const
    {
        Location location = home;
        location.path = dir + QLatin1Char('/') + relative;
        return location;
    }
    // Runs a shell command on the server and returns its standard output.
    QByteArray run(const QString &command, int *exitCode = nullptr) const
    {
        QStringList args;
        splitArguments(shell, &args);
        const QString program = args.takeFirst();
        args << QStringLiteral("-p") << QString::number(home.port)
             << (home.user.isEmpty() ? home.host : home.user + QLatin1Char('@') + home.host) << command;
        QProcess process;
        process.start(program, args);
        process.waitForFinished(30000);
        if (exitCode)
            *exitCode = process.exitCode();
        return process.readAllStandardOutput();
    }
    RsyncOptions options() const
    {
        RsyncOptions o = testOptions();
        o.useRemoteShell = true;
        o.remoteShell = shell;
        return o;
    }
};

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
    void parseSizeAndRate_data();
    void parseSizeAndRate();
    void humanLevel();
    void processedBytes_data();
    void processedBytes();
    void parseDifference_data();
    void parseDifference();
    void copyLocal();
    void copyWithVerify();
    void copyRltKeepsTimesNotPermissions();
    void verifyDetectsDifference();
    void dryRunChangesNothing();
    void moveRemovesSources();
    void removeEmptyDirectories();
    void verifiedMove();
    void moveRemovesEmptiedDirectories();
    void missingProgramAborts();
    void localPathRoundTrip_data();
    void localPathRoundTrip();
    void localPathRandomRoundTrip();
    void localPathUrlsAndQuoting();
    void localTotalSize();
    void copyNonUtf8Names();
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
    void summaryFlags_data();
    void summaryFlags();
    void modifyWindowArgument();
    void modifyWindowProbe();
    void destinationPaths();
    void dropFromPageCache();
    void verifyReadsFromDisk();
    void cancelDuringCacheDrop_data();
    void cancelDuringCacheDrop();
    void deleteJobDuringCacheDrop();
    void remoteRoundTrip();
    void remoteVerifiedMove();
    void remoteSourceMove();
    void remoteCancel();
    void remoteAskpassHostKeyAndPassword();
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
    // A move with verification removes the sources only in a separate pass after the verification.
    o.removeSourceFiles = true;
    QVERIFY(o.canVerify());
    QVERIFY(o.verifiedMove());
    QVERIFY(!o.arguments().contains(QStringLiteral("--remove-source-files")));
    QVERIFY(o.removalArguments().contains(QStringLiteral("--remove-source-files")));
    o.verifyAfter = false;
    QVERIFY(!o.verifiedMove());
    QVERIFY(o.arguments().contains(QStringLiteral("--remove-source-files")));
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

void TestRsyncCore::parseSizeAndRate_data()
{
    // Formats seen in rsync 3.4.1 output (a locale with a decimal comma and the C locale), 6 000 000 bytes at 2 MiB/s.
    QTest::addColumn<QString>("line");
    QTest::addColumn<int>("level");
    QTest::addColumn<qint64>("bytes");
    QTest::addColumn<qint64>("rate");
    QTest::addColumn<int>("xfr");
    const qint64 mib2 = 2 * 1024 * 1024;
    QTest::newRow("plain, comma") << "      6.000.000 100%    2,00MB/s    0:00:02 (xfr#1, to-chk=0/1)" << 1 << qint64(6000000) << mib2 << 1;
    QTest::newRow("plain, C") << "      6,000,000 100%    2.00MB/s    0:00:02 (xfr#3, ir-chk=10/20)" << 1 << qint64(6000000) << mib2 << 3;
    QTest::newRow("-h, units of 1000") << "          4,62M  77%    2,00MB/s    0:00:00" << 2 << qint64(4620000) << mib2 << -1;
    QTest::newRow("-hh, units of 1024") << "          4,41M  77%    2,00MB/s    0:00:00" << 3 << qint64(4.41 * 1024 * 1024) << mib2 << -1;
    QTest::newRow("kB/s, C locale") << "         32.77K   1%    0.00kB/s    0:00:00" << 2 << qint64(32770) << qint64(0) << -1;
    QTest::newRow("GB/s") << "          3,00M  99%    2,76GB/s    0:00:00 (xfr#1, to-chk=3/5)" << 2 << qint64(3000000)
                          << qint64(2.76 * 1024 * 1024 * 1024) << 1;
}

void TestRsyncCore::parseSizeAndRate()
{
    QFETCH(QString, line);
    QFETCH(int, level);
    QFETCH(qint64, bytes);
    QFETCH(qint64, rate);
    QFETCH(int, xfr);
    RsyncJob::ProgressInfo info;
    QVERIFY(RsyncJob::parseProgressLine(line, &info, level));
    QCOMPARE(info.bytes, bytes);
    QCOMPARE(info.bytesPerSecond, rate);
    QCOMPARE(info.transferNumber, xfr);
    QCOMPARE(RsyncJob::parseRate(QStringLiteral("???")), qint64(-1));
}

void TestRsyncCore::humanLevel()
{
    QCOMPARE(RsyncJob::humanLevel({"-avP"}), 1);
    QCOMPARE(RsyncJob::humanLevel({"-avhP"}), 2);
    QCOMPARE(RsyncJob::humanLevel({"-avhP", "-h"}), 3);
    QCOMPARE(RsyncJob::humanLevel({"-a", "--human-readable"}), 2);
    QCOMPARE(RsyncJob::humanLevel({"-a", "--", "-h-named-file"}), 1); // sources are not options
    QCOMPARE(RsyncJob::humanLevel({"-a", "--exclude=*.h"}), 1);
}

void TestRsyncCore::processedBytes_data()
{
    QTest::addColumn<int>("progress");
    QTest::addColumn<bool>("human");
    QTest::newRow("per file") << int(RsyncOptions::Progress::PerFile) << false;
    QTest::newRow("overall, -h") << int(RsyncOptions::Progress::Overall) << true;
}

void TestRsyncCore::processedBytes()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QFETCH(int, progress);
    QFETCH(bool, human);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), QByteArray(300000, 'a'));
    writeFile(tmp.filePath("src/sub/b"), QByteArray(200000, 'b'));
    writeFile(tmp.filePath("src/c"), QByteArray(100000, 'c'));
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.progress = RsyncOptions::Progress(progress);
    options.human = human;
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QSignalSpy rates(&job, &RsyncJob::transferRateChanged);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QVERIFY(!rates.isEmpty());
    const qint64 processed = rates.last().at(1).toLongLong();
    // -h rounds to 3 significant digits ("600K").
    QVERIFY2(qAbs(processed - 600000) <= (human ? 5000 : 0), qPrintable(QString::number(processed)));
    for (const QList<QVariant> &args : std::as_const(rates))
        QVERIFY(args.at(0).toLongLong() >= 0);
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

void TestRsyncCore::verifiedMove()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a/1"), "one");
    writeFile(tmp.filePath("src/2"), "two");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    RsyncOptions options = testOptions();
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    options.verifyAfter = true;

    // A difference that only the verification sees (same size and time, quick check skips it):
    // nothing in the source may be removed.
    writeFile(dest + "/src/2", "TWO");
    setMtime(tmp.filePath("src/2"), 1700000000);
    setMtime(dest + "/src/2", 1700000000);
    RsyncJob refused(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QVERIFY(runJob(refused));
    QCOMPARE(refused.summary().differences, 1);
    QCOMPARE(refused.summary().sourcesRemoved, 0);
    QVERIFY(!refused.summary().ok());
    QCOMPARE(readFile(tmp.filePath("src/a/1")), QByteArray("one")); // copied, but still in the source
    QCOMPARE(readFile(dest + "/src/a/1"), QByteArray("one"));
    QCOMPARE(readFile(tmp.filePath("src/2")), QByteArray("two"));

    // With -c the copy fixes the file, the verification passes and only then the sources go.
    options.checksum = true;
    RsyncJob move(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QSignalSpy phases(&move, &RsyncJob::phaseChanged);
    QVERIFY(runJob(move));
    QVERIFY(move.summary().ok());
    QCOMPARE(move.summary().verified, 1);
    QCOMPARE(move.summary().sourcesRemoved, 1);
    QCOMPARE(readFile(dest + "/src/2"), QByteArray("two"));
    QCOMPARE(readFile(dest + "/src/a/1"), QByteArray("one"));
    QVERIFY(!QFileInfo::exists(tmp.filePath("src"))); // files and the emptied directories
    QStringList order;
    for (const QList<QVariant> &args : std::as_const(phases)) {
        if (!args.first().toString().isEmpty())
            order << args.first().toString();
    }
    QCOMPARE(order, (QStringList{"Copying", "Writing to the disk", "Verifying", "Removing the sources"}));
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

void TestRsyncCore::localPathRoundTrip_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("plain");
    QTest::addColumn<QString>("display");

    QTest::newRow("ascii") << QByteArray("/tmp/a b") << true << "/tmp/a b";
    QTest::newRow("utf-8") << QByteArray("/tmp/na\xc3\xafve") << true << QString::fromUtf8("/tmp/naïve");
    QTest::newRow("emoji") << QByteArray("\xf0\x9f\x98\x80") << true << QString::fromUtf8("\xf0\x9f\x98\x80");
    QTest::newRow("latin-1 byte") << QByteArray("/tmp/caf\xe9") << false << "/tmp/caf\\xe9";
    QTest::newRow("0xff") << QByteArray("\xff\xfe") << false << "\\xff\\xfe";
    QTest::newRow("overlong") << QByteArray("\xc0\xaf") << false << "\\xc0\\xaf";
    QTest::newRow("surrogate") << QByteArray("\xed\xa0\x80") << false << "\\xed\\xa0\\x80";
    QTest::newRow("truncated") << QByteArray("a\xe2\x82") << false << "a\\xe2\\x82";
    QTest::newRow("above U+10FFFF") << QByteArray("\xf4\x90\x80\x80") << false << "\\xf4\\x90\\x80\\x80";
    // U+F780..U+F7FF are the escape characters themselves - stored escaped, so they survive too.
    QTest::newRow("escape range") << QByteArray("\xef\x9e\x80") << false << "\\xef\\x9e\\x80";
    QTest::newRow("empty") << QByteArray() << true << "";
}

void TestRsyncCore::localPathRoundTrip()
{
    QFETCH(QByteArray, bytes);
    QFETCH(bool, plain);
    QFETCH(QString, display);
    const QString decoded = LocalPath::decode(bytes);
    QCOMPARE(LocalPath::encode(decoded), bytes);
    QCOMPARE(LocalPath::isPlain(decoded), plain);
    QCOMPARE(LocalPath::display(decoded), display);
    if (plain)
        QCOMPARE(decoded, QString::fromUtf8(bytes));
}

void TestRsyncCore::localPathRandomRoundTrip()
{
    QRandomGenerator random(20261028);
    for (int i = 0; i < 20000; ++i) {
        QByteArray bytes(int(random.bounded(24)), '\0');
        for (char &c : bytes)
            c = char(random.bounded(i % 2 ? 256 : 64) + (i % 2 ? 0 : 0xC0)); // half biased to UTF-8 lead bytes
        QCOMPARE(LocalPath::encode(LocalPath::decode(bytes)), bytes);
    }
}

void TestRsyncCore::localPathUrlsAndQuoting()
{
    // file:// URLs (Dolphin, drag & drop, clipboard) keep the bytes - QUrl::toLocalFile() would not.
    const Location local = Location::parse(QStringLiteral("file:///tmp/a%FFb%20c"));
    QCOMPARE(LocalPath::encode(local.path), QByteArray("/tmp/a\xff" "b c"));
    const Location remote = Location::parse(QStringLiteral("sftp://me@nas:2222/data/x%FE"));
    QCOMPARE(LocalPath::encode(remote.path), QByteArray("/data/x\xfe"));
    QCOMPARE(Location::parse(remote.toString()), remote);
    QCOMPARE(Location::parse(local.toString()), local);

    // The command preview uses bash's $'...' for such names, so it can be pasted into a shell.
    QCOMPARE(shellQuote(LocalPath::decode("/tmp/a\xff'b\\c")), QStringLiteral("$'/tmp/a\\xff\\'b\\\\c'"));
}

void TestRsyncCore::localTotalSize()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("dir/a"), QByteArray(1000, 'a'));
    writeFile(tmp.filePath("dir/sub/b"), QByteArray(234, 'b'));
    writeFile(tmp.filePath("file"), QByteArray(5, 'f'));
    writeFile(tmp.filePath("outside/big"), QByteArray(100000, 'x'));
    QVERIFY(QFile::link(tmp.filePath("outside"), tmp.filePath("dir/link"))); // not followed

    const QStringList paths{tmp.filePath("dir"), tmp.filePath("file"), tmp.filePath("missing")};
    QCOMPARE(LocalPath::totalSize(paths, true), qint64(1239));
    QCOMPARE(LocalPath::totalSize(paths, false), qint64(5)); // without -r rsync skips directories
    const std::atomic_bool cancelled{true};
    QCOMPARE(LocalPath::totalSize(paths, true, &cancelled), qint64(-1));
}

void TestRsyncCore::copyNonUtf8Names()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray base = QFile::encodeName(tmp.path());
    const auto cleanup = qScopeGuard([&] { removeTree(base + "/src\xff"); removeTree(base + "/out\xfc"); });
    const QByteArray src = base + "/src\xff";
    QCOMPARE(::mkdir(src.constData(), 0755), 0);
    QCOMPARE(::mkdir((src + "/sub\xfd").constData(), 0755), 0);
    writeRaw(src + "/f\xfe.txt", "data");
    writeRaw(src + "/sub\xfd/g", "more");

    const QString source = LocalPath::decode(src);
    QVERIFY(LocalPath::info(source).isDir);
    QCOMPARE(LocalPath::canonical(source), source);
    const QString dest = LocalPath::decode(base + "/out\xfc/deeper");
    QVERIFY(LocalPath::makePath(dest));
    QVERIFY(LocalPath::isDirectory(dest));

    // Copy with verification: both rsync runs get the exact bytes.
    RsyncOptions options = testOptions();
    options.verifyAfter = true;
    const RsyncJob::Plan plan = RsyncJob::plan({Location::parse(source)}, Location::parse(dest), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 1);
    QCOMPARE(readRaw(base + "/out\xfc/deeper/src\xff/f\xfe.txt"), QByteArray("data"));
    QCOMPARE(readRaw(base + "/out\xfc/deeper/src\xff/sub\xfd/g"), QByteArray("more"));

    // Move: the source files and the emptied directories (also with such names) are removed.
    options.verifyAfter = false;
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    RsyncJob move(RsyncJob::plan({Location::parse(source)}, Location::parse(dest), options).transfers, options);
    QVERIFY(runJob(move));
    QVERIFY(move.summary().ok());
    QCOMPARE(move.summary().sourceDirsRemoved, 2);
    QVERIFY(!LocalPath::info(source).exists);
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
    o.autoModifyWindow = false;

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
    QTest::newRow("rsync-move-verify") << "rsync-move-verify" << QStringList{"-avhPc"} << true;
    QTest::newRow("rsync-move-portable") << "rsync-move-portable" << QStringList{"-rltvhP", "--remove-source-files"}
                                         << false;
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

void TestRsyncCore::summaryFlags_data()
{
    QTest::addColumn<QString>("change");
    QTest::addColumn<QString>("flags");
    QTest::newRow("default") << "" << "-a";
    QTest::newRow("checksum") << "checksum" << "-ac";
    QTest::newRow("rlt") << "rlt" << "-rlt";
    QTest::newRow("move") << "move" << "-a";             // "Move" in the name says it
    QTest::newRow("output only") << "output" << "-a";    // -v -h -P -n do not count
    QTest::newRow("preserve, update") << "preserve" << "-aHAXuz";
    QTest::newRow("delete, ignore") << "delete" << "-a --ignore-existing --delete";
    QTest::newRow("nothing") << "nothing" << "";
}

void TestRsyncCore::summaryFlags()
{
    QFETCH(QString, change);
    QFETCH(QString, flags);
    RsyncOptions o;
    if (change == "checksum") {
        o.checksum = true;
    } else if (change == "rlt") {
        o.archive = false;
        o.perms = o.group = o.owner = o.devices = false;
    } else if (change == "move") {
        o.removeSourceFiles = o.removeSourceDirs = true;
    } else if (change == "output") {
        o.dryRun = true;
        o.progress = RsyncOptions::Progress::Overall;
    } else if (change == "preserve") {
        o.hardLinks = o.acls = o.xattrs = o.compress = true;
        o.existing = RsyncOptions::Existing::SkipNewer;
    } else if (change == "delete") {
        o.existing = RsyncOptions::Existing::IgnoreExisting;
        o.deleteExtraneous = true;
    } else if (change == "nothing") {
        o.archive = o.recursive = o.links = o.perms = o.times = o.group = o.owner = o.devices = false;
    }
    QCOMPARE(o.summaryFlags(), flags);
    QCOMPARE(o.label("Name"), flags.isEmpty() ? QStringLiteral("Name") : "Name (" + flags + ")");
}

void TestRsyncCore::presetNamesMatchDolphinMenu()
{
    const QList<RsyncPreset> presets = RsyncOptions::presets({QStringLiteral(RSYNC_GUI_SOURCE_PRESETS_DIR)});
    QVERIFY(presets.size() >= 3);
    QStringList names;
    for (const RsyncPreset &preset : presets) {
        QVERIFY2(preset.error.isEmpty(), qPrintable(preset.error));
        QVERIFY2(preset.name != preset.id, qPrintable(preset.id)); // every shipped preset has a name
        names << preset.id;
    }
    QStringList sorted = names;
    std::sort(sorted.begin(), sorted.end());
    QCOMPARE(names, sorted); // File > Load preset: by file name

    // Every Dolphin action is "<preset label>…" and runs that preset; every preset has an action,
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
    QStringList actions = entries.value(QStringLiteral("Desktop Entry/Actions")).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    // Dolphin shows the actions of a submenu sorted by their ids, whatever the order in Actions=.
    QStringList shown = actions;
    std::sort(shown.begin(), shown.end());
    QCOMPARE(actions, shown); // keep the file readable in the order the menu shows
    actions = shown;
    QCOMPARE(actions.size(), presets.size());
    for (qsizetype i = 0; i < actions.size(); ++i) {
        const QString prefix = QStringLiteral("Desktop Action ") + actions.at(i) + QLatin1Char('/');
        const QString label = entries.value(prefix + QStringLiteral("Name"));
        const QString exec = entries.value(prefix + QStringLiteral("Exec"));
        QCOMPARE(label, presets.at(i).label + QStringLiteral("…"));
        QVERIFY2(exec.contains(QStringLiteral("--preset ") + presets.at(i).id + QLatin1Char(' ')), qPrintable(exec));
    }
}

void TestRsyncCore::modifyWindowArgument()
{
    RsyncOptions options;
    QVERIFY(!options.arguments().join(QLatin1Char(' ')).contains(QStringLiteral("--modify-window")));
    options.modifyWindow = 1;
    QVERIFY(options.arguments().contains(QStringLiteral("--modify-window=1")));
    QVERIFY(options.removalArguments().contains(QStringLiteral("--modify-window=1")));
    // The verification compares content only.
    QVERIFY(!options.verifyArguments().join(QLatin1Char(' ')).contains(QStringLiteral("--modify-window")));
    // Without -t the times are not kept, so there is nothing to compare.
    options.archive = false;
    options.times = false;
    QVERIFY(!options.arguments().join(QLatin1Char(' ')).contains(QStringLiteral("--modify-window")));
    options.times = true;
    QVERIFY(options.arguments().contains(QStringLiteral("--modify-window=1")));
    // Given by the user: not added a second time.
    options.extraArguments = QStringLiteral("--modify-window=3601");
    QCOMPARE(options.arguments().filter(QStringLiteral("--modify-window")), QStringList{QStringLiteral("--modify-window=3601")});
    options.extraArguments = QStringLiteral("-@ 2");
    QVERIFY(options.arguments().filter(QStringLiteral("--modify-window")).isEmpty());
    // On by default; turned off it is kept (the window then never sets modifyWindow).
    QVERIFY(RsyncOptions().autoModifyWindow);
    QCOMPARE(RsyncOptions().toJson().value("autoModifyWindow").toBool(true), true);
    // Not a saved option.
    RsyncOptions plain;
    plain.modifyWindow = 1;
    QVERIFY(!RsyncOptions::fromJson(plain.toJson()).modifyWindow);
}

void TestRsyncCore::modifyWindowProbe()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // Linux file systems keep nanoseconds.
    QCOMPARE(RsyncJob::modifyWindowFor(tmp.path()), 0);
    QCOMPARE(RsyncJob::modifyWindowFor(tmp.filePath(QStringLiteral("not/created/yet"))), 0);
    QCOMPARE(RsyncJob::modifyWindowFor(tmp.path(), false), 0);
    QVERIFY(!RsyncJob::isFatFileSystem(tmp.path()));
    // The probe file is removed again, and a missing destination is not created.
    QCOMPARE(QDir(tmp.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList());

    // A mounted FAT file system, if there is one to test with (e.g. a loop-mounted image).
    const QString fat = qEnvironmentVariable("RSYNC_GUI_TEST_FAT_DIR");
    if (fat.isEmpty())
        QSKIP("Set RSYNC_GUI_TEST_FAT_DIR to a writable directory on FAT to test the real case.");
    QVERIFY(RsyncJob::isFatFileSystem(fat));
    QCOMPARE(RsyncJob::modifyWindowFor(fat), 1);
    QCOMPARE(RsyncJob::modifyWindowFor(fat, false), 1);
    QCOMPARE(RsyncJob::modifyWindowFor(fat + QStringLiteral("/missing/dir")), 1);
}

void TestRsyncCore::destinationPaths()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "a");
    writeFile(tmp.filePath("src/sub/b"), "b");
    writeFile(tmp.filePath("dest/src/a"), "a");
    writeFile(tmp.filePath("dest/a"), "a");
    writeFile(tmp.filePath("dest/sub/b"), "b");
    const QString dest = tmp.filePath("dest");

    RsyncJob::Transfer transfer;
    transfer.destination = dest + QLatin1Char('/');
    transfer.sources = {tmp.filePath("src")};
    QCOMPARE(RsyncJob::destinationPaths(transfer), QStringList{dest + "/src"});
    // The contents of a directory: its entries.
    transfer.sources = {tmp.filePath("src") + QLatin1Char('/')};
    QStringList paths = RsyncJob::destinationPaths(transfer);
    paths.sort();
    QCOMPARE(paths, QStringList({dest + "/a", dest + "/sub"}));
    // A copy that is not where expected (e.g. --relative), and remote contents: the whole destination.
    transfer.sources = {tmp.filePath("other")};
    QCOMPARE(RsyncJob::destinationPaths(transfer), QStringList{dest});
    transfer.sources = {QStringLiteral("user@host:/data/")};
    QCOMPARE(RsyncJob::destinationPaths(transfer), QStringList{dest});
    transfer.sources = {QStringLiteral("user@host:/data/src")};
    QCOMPARE(RsyncJob::destinationPaths(transfer), QStringList{dest + "/src"});
}

void TestRsyncCore::dropFromPageCache()
{
    // On a disk file system (not tmpfs, where the page cache is the storage): the build directory.
    QTemporaryDir tmp(QCoreApplication::applicationDirPath() + QStringLiteral("/tst-cache-XXXXXX"));
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("dir/big.bin"), QByteArray(4 * 1024 * 1024, 'x'));
    writeFile(tmp.filePath("dir/sub/small"), "small");
    QTemporaryDir outside;
    QVERIFY(outside.isValid());
    writeFile(outside.filePath("elsewhere"), "not mine");
    QVERIFY(QFile::link(outside.filePath("elsewhere"), tmp.filePath("dir/link")));

    std::atomic<bool> cancel{true};
    QVERIFY(RsyncJob::dropFromPageCache({tmp.filePath("dir")}, &cancel).cancelled);
    cancel = false;
    const RsyncJob::CacheDropResult result = RsyncJob::dropFromPageCache({tmp.filePath("dir")}, &cancel);
    QVERIFY(!result.cancelled);
    QCOMPARE(result.files, 2); // the symlink is not followed
    QCOMPARE(result.bytes, 4 * 1024 * 1024 + 5);
    QCOMPARE(result.errors, 0);
    QCOMPARE(readFile(tmp.filePath("dir/big.bin")).size(), 4 * 1024 * 1024); // nothing lost

    struct statfs fs;
    QVERIFY(::statfs(QFile::encodeName(tmp.path()).constData(), &fs) == 0);
    constexpr long kTmpfsMagic = 0x01021994;
    if (long(fs.f_type) == kTmpfsMagic)
        QSKIP("The build directory is on tmpfs - the page cache cannot be dropped there.");
    QVERIFY(residentBytes(tmp.filePath("dir/big.bin")) > 0); // read just now
    RsyncJob::dropFromPageCache({tmp.filePath("dir")});
    QCOMPARE(residentBytes(tmp.filePath("dir/big.bin")), 0);
}

void TestRsyncCore::verifyReadsFromDisk()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp(QCoreApplication::applicationDirPath() + QStringLiteral("/tst-verify-XXXXXX"));
    QVERIFY(tmp.isValid());
    QTemporaryDir source;
    QVERIFY(source.isValid());
    writeFile(source.filePath("src/a"), QByteArray(1024 * 1024, 'a'));
    writeFile(source.filePath("src/sub/b"), "bbb");

    RsyncOptions options = testOptions();
    options.verifyAfter = true;
    RsyncJob job(localPlan({source.filePath("src")}, tmp.path(), options).transfers, options);
    QStringList log;
    connect(&job, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 1);
    const QStringList dropped = log.filter(QStringLiteral("dropped from the cache"));
    QCOMPARE(dropped.size(), 1);
    QVERIFY2(dropped.first().startsWith(QStringLiteral("2 file(s)")), qPrintable(dropped.first()));
    // The drop comes between the copy and the verification.
    const qsizetype dropIndex = log.indexOf(dropped.first());
    QVERIFY(dropIndex > log.indexOf(QStringLiteral("Transfer finished.")));
    QVERIFY(dropIndex < log.indexOf(QStringLiteral("--- Verifying ---")));
}

void TestRsyncCore::cancelDuringCacheDrop_data()
{
    QTest::addColumn<bool>("whileRunning");
    // Cancelled when the phase starts (before the thread looks at any file), and from the event loop
    // while the thread walks the copies (or right after it - the outcome must be the same).
    QTest::newRow("at the start") << false;
    QTest::newRow("while running") << true;
}

void TestRsyncCore::cancelDuringCacheDrop()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QFETCH(bool, whileRunning);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    for (int i = 0; i < 2000; ++i)
        writeFile(tmp.filePath(QStringLiteral("src/d%1/f%2").arg(i % 20).arg(i)), "x");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    // A verified move: cancelling before the verification must leave every source in place.
    RsyncOptions options = testOptions();
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    options.verifyAfter = true;
    RsyncJob job(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QStringList log;
    connect(&job, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    bool cancelled = false;
    connect(&job, &RsyncJob::phaseChanged, this, [&](const QString &phase) {
        if (phase != QLatin1String("Writing to the disk") || cancelled)
            return;
        cancelled = true;
        if (whileRunning)
            QTimer::singleShot(0, &job, &RsyncJob::cancel);
        else
            job.cancel();
    });
    QVERIFY(runJob(job));
    QVERIFY(cancelled);
    QVERIFY(job.summary().cancelled);
    QCOMPARE(job.summary().transfersOk, 1);
    QCOMPARE(job.summary().verifyFailed, 1);
    QCOMPARE(job.summary().verified, 0);
    QCOMPARE(job.summary().sourcesRemoved, 0);
    QVERIFY(!log.contains(QStringLiteral("--- Verifying ---")));
    QVERIFY(log.contains(QStringLiteral("Verification cancelled.")));
    QVERIFY(log.contains(QStringLiteral("The sources were not removed, because the verification did not pass.")));
    // Copied, and still in the source.
    QCOMPARE(readFile(tmp.filePath("src/d7/f1987")), QByteArray("x"));
    QCOMPARE(readFile(dest + "/src/d7/f1987"), QByteArray("x"));
    QCOMPARE(QDir(tmp.filePath("src")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 20);
}

void TestRsyncCore::deleteJobDuringCacheDrop()
{
    if (!haveRsync())
        QSKIP("rsync is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    for (int i = 0; i < 2000; ++i)
        writeFile(tmp.filePath(QStringLiteral("src/f%1").arg(i)), "x");
    const QString dest = tmp.filePath("dest");
    QVERIFY(QDir().mkpath(dest));

    // E.g. the window is closed: the destructor stops the thread and waits for it, nothing is called afterwards.
    RsyncOptions options = testOptions();
    options.verifyAfter = true;
    auto *job = new RsyncJob(localPlan({tmp.filePath("src")}, dest, options).transfers, options);
    QPointer<RsyncJob> guard = job;
    bool finished = false;
    const auto threads = [] { return QDir(QStringLiteral("/proc/self/task")).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(); };
    const qsizetype threadsBefore = threads();
    qsizetype threadsDuring = 0;
    qsizetype threadsAfter = 0;
    connect(job, &RsyncJob::finished, this, [&finished] { finished = true; });
    connect(job, &RsyncJob::phaseChanged, this, [&, job](const QString &phase) {
        if (phase != QLatin1String("Writing to the disk"))
            return;
        QTimer::singleShot(0, job, [&, job] {
            threadsDuring = threads();
            delete job;
            threadsAfter = threads(); // the thread has ended, not just been left behind
        });
    });
    job->start();
    QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 30000);
    QTest::qWait(200); // a queued call from the thread would arrive now
    QVERIFY(!finished);
    QVERIFY2(threadsDuring > threadsBefore, "the cache was not being dropped in a thread when the job was deleted");
    QCOMPARE(threadsAfter, threadsBefore);
}

void TestRsyncCore::remoteRoundTrip()
{
    RemoteServer server(QStringLiteral("roundtrip"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // Names with a space, with a byte that is not valid UTF-8 (Latin-1 "é"), a subdirectory and a symlink.
    const QByteArray src = QFile::encodeName(tmp.filePath(QStringLiteral("src")));
    QVERIFY(QDir().mkpath(tmp.filePath(QStringLiteral("src/sub dir"))));
    const QList<QPair<QByteArray, QByteArray>> files{
        {"plain.txt", "plain"},
        {"with space.txt", "space"},
        {"latin1-\xe9.txt", "latin1"},
        {"sub dir/deep.bin", QByteArray(3000, 'x')}, // readRaw() reads up to 4 KiB
    };
    for (const auto &[name, data] : files)
        writeRaw(src + '/' + name, data);
    QCOMPARE(::symlink("plain.txt", (src + "/link").constData()), 0);
    // QTemporaryDir cannot remove names that are not valid UTF-8.
    const auto cleanup = qScopeGuard([&] {
        removeTree(src);
        removeTree(QFile::encodeName(tmp.filePath(QStringLiteral("back"))));
    });

    // Local -> remote, into a directory that does not exist yet (rsync creates the last one).
    RsyncOptions options = server.options();
    const RsyncJob::Plan up = RsyncJob::plan({Location::parse(LocalPath::decode(src))}, server.at(QStringLiteral("dest")),
                                             options);
    QVERIFY2(up.ok(), qPrintable(up.error));
    QCOMPARE(up.transfers.first().port, server.home.port);
    RsyncJob upload(up.transfers, options);
    QVERIFY(runJob(upload));
    QVERIFY(upload.summary().ok());
    const QByteArray listing = server.run(QStringLiteral("cd '%1/dest/src' && find . | LC_ALL=C sort").arg(server.dir));
    QCOMPARE(listing, QByteArray(".\n./latin1-\xe9.txt\n./link\n./plain.txt\n./sub dir\n./sub dir/deep.bin\n./with space.txt\n"));
    QCOMPARE(server.run(QStringLiteral("readlink '%1/dest/src/link'").arg(server.dir)), QByteArray("plain.txt\n"));

    // Remote -> local, back again: the same bytes under the same names.
    const QString back = tmp.filePath(QStringLiteral("back"));
    QVERIFY(QDir().mkpath(back));
    const RsyncJob::Plan down = RsyncJob::plan({server.at(QStringLiteral("dest/src"))}, Location::parse(back), options);
    QVERIFY2(down.ok(), qPrintable(down.error));
    RsyncJob download(down.transfers, options);
    QVERIFY(runJob(download));
    QVERIFY(download.summary().ok());
    for (const auto &[name, data] : files)
        QCOMPARE(readRaw(QFile::encodeName(back) + "/src/" + name), data);
    QVERIFY(QFileInfo(back + QStringLiteral("/src/link")).isSymLink());
}

void TestRsyncCore::remoteVerifiedMove()
{
    RemoteServer server(QStringLiteral("verifiedmove"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");
    writeFile(tmp.filePath("src/sub/b"), "bbb");

    // Copy, verify on the server, and only then remove the local sources (and the emptied directories).
    RsyncOptions options = server.options();
    options.checksum = true;
    options.verifyAfter = true;
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    QVERIFY(options.verifiedMove());
    const RsyncJob::Plan plan = RsyncJob::plan({Location::parse(tmp.filePath("src"))}, server.at(QStringLiteral("dest")),
                                               options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 1);
    QCOMPARE(job.summary().differences, 0);
    QCOMPARE(job.summary().sourcesRemoved, 1);
    QVERIFY(!QFileInfo::exists(tmp.filePath("src")));
    QCOMPARE(server.run(QStringLiteral("cat '%1/dest/src/a' '%1/dest/src/sub/b'").arg(server.dir)), QByteArray("aaabbb"));
}

void TestRsyncCore::remoteSourceMove()
{
    RemoteServer server(QStringLiteral("sourcemove"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    server.run(QStringLiteral("mkdir -p '%1/src/sub' && printf one > '%1/src/one' && printf two > '%1/src/sub/two'")
                   .arg(server.dir));

    // Remote -> local move: rsync removes the files on the server (directories stay, they are not local).
    RsyncOptions options = server.options();
    options.removeSourceFiles = true;
    const QString dest = tmp.filePath(QStringLiteral("dest"));
    QVERIFY(QDir().mkpath(dest));
    const RsyncJob::Plan plan = RsyncJob::plan({server.at(QStringLiteral("src"))}, Location::parse(dest), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(readFile(dest + QStringLiteral("/src/one")), QByteArray("one"));
    QCOMPARE(readFile(dest + QStringLiteral("/src/sub/two")), QByteArray("two"));
    QCOMPARE(server.run(QStringLiteral("find '%1/src' -type f | wc -l").arg(server.dir)).trimmed(), QByteArray("0"));
}

void TestRsyncCore::remoteCancel()
{
    RemoteServer server(QStringLiteral("cancel"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QByteArray data(16 * 1024 * 1024, '\0');
    for (qsizetype i = 0; i < data.size(); i += 4)
        *reinterpret_cast<quint32 *>(data.data() + i) = QRandomGenerator::global()->generate();
    writeFile(tmp.filePath("src/big.bin"), data);

    RsyncOptions options = server.options();
    options.useBwLimit = true;
    options.bwLimit = QStringLiteral("1M"); // 16 s for the whole file
    const RsyncJob::Plan plan = RsyncJob::plan({Location::parse(tmp.filePath("src"))}, server.at(QStringLiteral("dest")),
                                               options);
    RsyncJob job(plan.transfers, options);
    QSignalSpy finished(&job, &RsyncJob::finished);
    job.start();
    QTest::qWait(1500);
    QVERIFY(job.isRunning());
    job.cancel();
    QVERIFY(finished.count() > 0 || finished.wait(15000));
    QVERIFY(job.summary().cancelled);
    QVERIFY(!job.summary().ok());
    // The rsync process on the server ends too (the connection is gone).
    QByteArray processes;
    for (int i = 0; i < 30; ++i) {
        processes = server.run(QStringLiteral("pgrep -x rsync || true")).trimmed();
        if (processes.isEmpty())
            break;
        QTest::qWait(100);
    }
    QCOMPARE(processes, QByteArray());
}

void TestRsyncCore::remoteAskpassHostKeyAndPassword()
{
    RemoteServer server(QStringLiteral("askpass"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    const QString password = qEnvironmentVariable("RSYNC_GUI_TEST_SSH_PASSWORD");
    if (password.isEmpty())
        QSKIP("RSYNC_GUI_TEST_SSH_PASSWORD is not set");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/a"), "aaa");

    // rsync runs ssh without a terminal: questions go to the SSH_ASKPASS program (in Plasma ksshaskpass,
    // with SSH_ASKPASS_REQUIRE=prefer). This one answers the host key question and the password prompt
    // and records them. A fresh known_hosts file, so the host is unknown.
    const QString askpass = tmp.filePath(QStringLiteral("askpass.sh"));
    const QString prompts = tmp.filePath(QStringLiteral("prompts.log"));
    writeFile(askpass, QStringLiteral("#!/bin/sh\nprintf '%s\\n---\\n' \"$1\" >> '%1'\ncase \"$1\" in\n"
                                      "  *'continue connecting'*) echo yes ;;\n  *assword*) echo '%2' ;;\nesac\n")
                           .arg(prompts, password)
                           .toUtf8());
    QVERIFY(QFile::setPermissions(askpass, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QByteArray oldAskpass = qgetenv("SSH_ASKPASS");
    const QByteArray oldRequire = qgetenv("SSH_ASKPASS_REQUIRE");
    qputenv("SSH_ASKPASS", QFile::encodeName(askpass));
    qputenv("SSH_ASKPASS_REQUIRE", "force");
    const auto restore = qScopeGuard([&] {
        qputenv("SSH_ASKPASS", oldAskpass);
        qputenv("SSH_ASKPASS_REQUIRE", oldRequire);
    });

    RsyncOptions options = testOptions();
    options.useRemoteShell = true;
    options.remoteShell = QStringLiteral("ssh -F /dev/null -o PubkeyAuthentication=no -o PreferredAuthentications=password "
                                         "-o UserKnownHostsFile=%1")
                              .arg(tmp.filePath(QStringLiteral("known_hosts")));
    const RsyncJob::Plan plan = RsyncJob::plan({Location::parse(tmp.filePath("src"))}, server.at(QStringLiteral("dest")),
                                               options);
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(server.run(QStringLiteral("cat '%1/dest/src/a'").arg(server.dir)), QByteArray("aaa"));
    const QByteArray asked = readFile(prompts);
    QVERIFY2(asked.contains("continue connecting"), asked.constData());
    QVERIFY2(asked.contains("password"), asked.constData());
    QVERIFY(QFileInfo(tmp.filePath(QStringLiteral("known_hosts"))).size() > 0);
}

QTEST_GUILESS_MAIN(TestRsyncCore)
#include "tst_rsynccore.moc"
