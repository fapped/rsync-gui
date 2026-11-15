#include "filelisting.h"
#include "localpath.h"
#include "networkshares.h"
#include "location.h"
#include "remoteprobe.h"
#include "rsyncjob.h"
#include "rsyncoptions.h"
#include "runlog.h"
#include "speedhistory.h"
#include "sshhosts.h"

#include <QDir>
#include <QElapsedTimer>
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
#include <QRegularExpression>
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

// The POSIX shells at hand, as program + arguments before the script argument of -c.
QList<QStringList> availableShells()
{
    QList<QStringList> shells;
    for (const char *shell : {"sh", "dash", "bash"}) {
        if (!QStandardPaths::findExecutable(QLatin1String(shell)).isEmpty())
            shells << QStringList{QLatin1String(shell)};
    }
    if (!QStandardPaths::findExecutable("busybox").isEmpty())
        shells << QStringList{"busybox", "sh"};
    return shells;
}

// Runs a remote command line (as the login shell on a server would) with a local shell in `dir`.
QByteArray runRemoteCommandLocally(const QStringList &shell, const QString &command, const QString &dir,
                                   int *exitCode = nullptr)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    QStringList args = shell.mid(1);
    args << QStringLiteral("-c") << command;
    RsyncJob::setRawArguments(&process, shell.first(), args); // the exact bytes of names that are not UTF-8
    process.start(shell.first(), args);
    process.waitForFinished(30000);
    if (exitCode)
        *exitCode = process.exitCode();
    return process.readAllStandardOutput();
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
    void planOnServer();
    void commandOnServer();
    void remoteShellQuoting();
    void sshSharing();
    void probeParse();
    void probeScriptLocally();
    void cleanUpScriptLocally();
    void probeTargets();
    void mountInfo();
    void networkShares();
    void shareSettings();
    void shareRouting();
    void sambaChmod();
    void sshHostsUnify();
    void sshHostsConfig();
    void parseProgress_data();
    void parseProgress();
    void parseSizeAndRate_data();
    void parseSizeAndRate();
    void humanLevel();
    void processedBytes_data();
    void processedBytes();
    void fileListTotal();
    void fileCountText();
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
    void runLogDirectory();
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
    void speedHistoryAverages();
    void speedHistoryGaps();
    void speedHistoryMergesBuckets();
    void speedHistoryNiceScale_data();
    void speedHistoryNiceScale();
    void fileListingKeepsNames();
    void fileListingFromDisplay_data();
    void fileListingFromDisplay();
    void fileListingHints();
    void fileListingMountPoints();
    void fileListingReplacedBytes();
    void remoteRoundTrip();
    void remoteVerifiedMove();
    void remoteSourceMove();
    void remoteCancel();
    void remoteOnServerCopy();
    void remoteOnServerCancel();
    void remoteOnServerSftpOnly();
    void remoteProbe();
    void remoteOnServerMoveRemovesDirs();
    void remoteSharedConnection();
    void remoteSshfsShare();
    void remoteSambaDetect();
    void remoteFromSftpOnly();
    void remoteSameServerOtherName();
    void remoteAskpassHostKeyAndPassword();
};

void TestRsyncCore::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
}

// With progress, the whole file list is built first so that its total is known from the start.
static const QStringList kFileList{"--no-inc-recursive", "--info=flist2"};

void TestRsyncCore::defaultArguments()
{
    // The defaults reproduce the "Copy (rsync)" command used before: rsync -avhP.
    QCOMPARE(RsyncOptions().arguments(), QStringList{QStringLiteral("-avhP")} + kFileList);
}

void TestRsyncCore::arguments_data()
{
    QTest::addColumn<QString>("change");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("checksum") << "checksum" << QStringList{"-avhPc"} + kFileList;
    QTest::newRow("no partial") << "nopartial" << QStringList{"-avh", "--progress"} + kFileList;
    QTest::newRow("overall") << "overall" << QStringList{"-avh", "--partial", "--info=progress2"} + kFileList;
    QTest::newRow("no progress") << "noprogress" << QStringList{"-avh", "--partial"};
    // Without -a its parts are listed one by one; by default they equal -a.
    QTest::newRow("no archive") << "noarchive" << QStringList{"-rlptgoDvhP"} + kFileList;
    QTest::newRow("rlt") << "rlt" << QStringList{"-rltvhP"} + kFileList;
    QTest::newRow("parts ignored with archive") << "archiveparts" << QStringList{"-avhP"} + kFileList;
    QTest::newRow("no archive, nothing preserved") << "flat" << QStringList{"-vhP"};
    QTest::newRow("preserve all") << "preserve" << QStringList{"-aHAXvhP"} + kFileList;
    QTest::newRow("update") << "update" << QStringList{"-avhPu"} + kFileList;
    QTest::newRow("ignore existing") << "ignore" << QStringList{"-avhP", "--ignore-existing"} + kFileList;
    QTest::newRow("compress, dry run") << "zn" << QStringList{"-avhPzn"} + kFileList;
    QTest::newRow("delete") << "delete" << QStringList{"-avhP", "--delete"} + kFileList;
    QTest::newRow("move") << "move" << QStringList{"-avhP", "--remove-source-files"} + kFileList;
    QTest::newRow("bwlimit") << "bwlimit" << QStringList{"-avhP", "--bwlimit=10M"} + kFileList;
    QTest::newRow("extra") << "extra"
                           << QStringList{"-avhP", "--no-inc-recursive", "--info=flist2", "--exclude=*.tmp", "--exclude", "a b", "--mkpath"};
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
    QCOMPARE(o.arguments(2222), (QStringList{"-avhP", "--no-inc-recursive", "--info=flist2", "-e", "ssh -i ~/.ssh/key -p 2222"}));
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

void TestRsyncCore::planOnServer()
{
    // Sources and a destination on the same host: one transfer that runs there, with the host's paths.
    RsyncOptions o;
    RsyncJob::Plan plan = RsyncJob::plan({Location::parse("sftp://u@h:2222/a"), Location::parse("sftp://u@h:2222/b c")},
                                         Location::parse("sftp://u@h:2222/dest"), o);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.size(), 1);
    const RsyncJob::Transfer &t = plan.transfers.first();
    QVERIFY(t.runsOnServer());
    QCOMPARE(t.host, QStringLiteral("u@h"));
    QCOMPARE(t.port, 2222);
    QCOMPARE(t.sources, (QStringList{"/a", "/b c"}));
    QCOMPARE(t.destination, QStringLiteral("/dest/"));

    // Paths relative to the home directory stay relative (ssh starts there).
    plan = localPlan({"h:docs", "h:~/pics/"}, "h:backup", o);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.first().sources, (QStringList{"docs", "~/pics/"}));
    QCOMPARE(plan.transfers.first().destination, QStringLiteral("backup/"));
    QCOMPARE(plan.transfers.first().host, QStringLiteral("h"));

    // A local source with the same destination: a normal transfer to the host and one on the host.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("f"), "f");
    plan = localPlan({tmp.filePath("f"), "h:/a"}, "h:/d", o);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.size(), 2);
    QVERIFY(!plan.transfers.at(0).runsOnServer());
    QCOMPARE(plan.transfers.at(0).destination, QStringLiteral("h:/d/"));
    QVERIFY(plan.transfers.at(1).runsOnServer());
    QCOMPARE(plan.transfers.at(1).destination, QStringLiteral("/d/"));

    // Refused: the option off, different hosts / users / ports, a directory copied into itself.
    RsyncOptions off;
    off.runOnServer = false;
    QVERIFY(!localPlan({"h:/a"}, "h:/b", off).ok());
    QVERIFY(!localPlan({"h:/a"}, "g:/b", o).ok());
    QVERIFY(!localPlan({"u@h:/a"}, "v@h:/b", o).ok());
    QVERIFY(!RsyncJob::plan({Location::parse("sftp://h:22/a")}, Location::parse("h:/b"), o).ok());
    QVERIFY(!localPlan({"h:/data"}, "h:/data/sub", o).ok());
    QVERIFY(!localPlan({"h:/data/"}, "h:/data", o).ok());
    QVERIFY(localPlan({"h:/data"}, "h:/data2", o).ok());
}

void TestRsyncCore::commandOnServer()
{
    RsyncJob::Transfer t;
    t.host = QStringLiteral("u@h");
    t.port = 2222;
    t.sources = QStringList{"/a", "/b c", "it's"};
    t.destination = QStringLiteral("~/dest dir/");
    RsyncOptions o; // -avhP
    RsyncJob::Command c = RsyncJob::command(t, o, RsyncJob::Pass::Copy);
    QCOMPARE(c.program, QStringLiteral("ssh"));
    QCOMPARE(c.arguments, (QStringList{"-p", "2222", "--", "u@h",
                                       "rsync -avhP --no-inc-recursive --info=flist2 -- /a '/b c' 'it'\\''s' ~/'dest dir/'"}));
    QCOMPARE(c.display(), QStringLiteral("ssh -p 2222 -- u@h \"rsync -avhP --no-inc-recursive --info=flist2 -- /a '/b c' 'it'\\\\''s' ~/'dest dir/'\""));
    // The displayed command is the same command for a local shell.
    {
        QProcess sh;
        sh.start("sh", {"-c", "f() { shift 3; printf '%s|' \"$@\"; }; f " + c.display().mid(4)});
        QVERIFY(sh.waitForFinished(5000));
        QCOMPARE(QString::fromUtf8(sh.readAllStandardOutput()), "u@h|" + c.arguments.last() + "|");
    }
    // The remote shell option is the ssh command here; rsync on the server gets no -e.
    o.useRemoteShell = true;
    o.remoteShell = QStringLiteral("ssh -i '/k k'");
    o.program = QStringLiteral("/opt/rsync/bin/rsync"); // a local path: on the server "rsync" from the PATH
    c = RsyncJob::command(t, o, RsyncJob::Pass::Copy, true);
    QCOMPARE(c.arguments.mid(0, 6), (QStringList{"-i", "/k k", "-p", "2222", "--", "u@h"}));
    QCOMPARE(c.arguments.last(),
             QStringLiteral("echo RSYNC-GUI-PID $$; exec rsync -avhP --no-inc-recursive --info=flist2 -- /a '/b c' 'it'\\''s' ~/'dest dir/'"));
    o.verifyAfter = true;
    c = RsyncJob::command(t, o, RsyncJob::Pass::Verify);
    QVERIFY2(c.arguments.last().startsWith("rsync -nrlci '--out-format=%i %n%L' -- /a"), qPrintable(c.arguments.last()));
    // A local transfer is unchanged: the local program with -e.
    RsyncJob::Transfer local;
    local.sources = QStringList{"/x"};
    local.destination = QStringLiteral("h:/y/");
    local.port = 2222;
    c = RsyncJob::command(local, o, RsyncJob::Pass::Copy);
    QCOMPARE(c.program, o.program);
    QVERIFY(c.arguments.contains("-e"));
    QCOMPARE(c.arguments.mid(c.arguments.size() - 3), (QStringList{"--", "/x", "h:/y/"}));
    QVERIFY(RsyncJob::exitCodeDescription(127).contains("rsync"));
    QVERIFY(RsyncOptions().runOnServer);
    QCOMPARE(RsyncOptions().toJson().value("runOnServer").toBool(false), true);
}

void TestRsyncCore::remoteShellQuoting()
{
    // Every word must reach a program on the server exactly - checked with the POSIX shells at hand.
    const QStringList words{
        "plain", "with space", "it's", "a\"b$c`d\\e!f", "new\nline", "-dash", "", "*", "~notexpanded",
        "tab\tx", LocalPath::decode(QByteArray("caf\xe9 '\xff'")), QString::fromUtf8("zażółć"),
    };
    QByteArray expected;
    QStringList quoted;
    for (const QString &word : words) {
        expected += LocalPath::encode(word) + '\0';
        quoted << remoteShellQuote(word);
    }
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString script = tmp.filePath("script.sh");
    QFile file(script);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(LocalPath::encode(QStringLiteral("printf '%s\\0' ") + quoted.join(QLatin1Char(' ')) + QLatin1Char('\n')));
    file.write("printf '%s\\0' " + LocalPath::encode(remotePathQuote("~") + ' ' + remotePathQuote("~/a b") + ' '
                                                      + remotePathQuote("/x ~")) + "\n");
    file.close();
    expected += QByteArray("/h\0/h/a b\0/x ~\0", 15);

    QList<QStringList> shells;
    for (const char *shell : {"sh", "dash", "bash"}) {
        if (!QStandardPaths::findExecutable(QLatin1String(shell)).isEmpty())
            shells << QStringList{QLatin1String(shell)};
    }
    if (!QStandardPaths::findExecutable("busybox").isEmpty())
        shells << QStringList{"busybox", "sh"};
    QVERIFY(!shells.isEmpty());
    for (QStringList command : std::as_const(shells)) {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", "/h");
        process.setProcessEnvironment(env);
        const QString program = command.takeFirst();
        process.start(program, command << script);
        QVERIFY(process.waitForFinished(10000));
        QCOMPARE(process.readAllStandardError(), QByteArray());
        QCOMPARE(process.readAllStandardOutput(), expected);
    }
}

void TestRsyncCore::sshSharing()
{
    RsyncOptions o;
    QCOMPARE(o.sshCommand(0), QStringList{"ssh"}); // not shared without a control path
    o.sshControlPath = QStringLiteral("/run/x/ssh-%C");
    const QStringList sharing{"-o", "ControlMaster=auto", "-o", "ControlPersist=60", "-o", "ControlPath=/run/x/ssh-%C"};
    QCOMPARE(o.sshCommand(22), (QStringList{"ssh"} + sharing + QStringList{"-p", "22"}));
    QCOMPARE(o.remoteShellArguments(0),
             (QStringList{"-e", "ssh -o ControlMaster=auto -o ControlPersist=60 -o ControlPath=/run/x/ssh-%C"}));
    QCOMPARE(o.remoteShellArguments(RsyncOptions::NoRemoteShell), QStringList());
    o.useRemoteShell = true;
    o.remoteShell = QStringLiteral("/usr/bin/ssh -i key");
    QCOMPARE(o.sshCommand(0), (QStringList{"/usr/bin/ssh", "-i", "key"} + sharing));
    // Another remote shell is left as it is.
    o.remoteShell = QStringLiteral("plink -batch");
    QCOMPARE(o.sshCommand(0), (QStringList{"plink", "-batch"}));
    QCOMPARE(o.remoteShellArguments(0), (QStringList{"-e", "plink -batch"}));
    // The control path is not an option of the window: not saved, not compared.
    RsyncOptions a, b;
    a.sshControlPath = QStringLiteral("/x");
    QVERIFY(a == b);
    QVERIFY(!a.toJson().contains("sshControlPath"));
    QVERIFY(!RsyncOptions::defaultSshControlPath().isEmpty() || qEnvironmentVariableIsEmpty("XDG_RUNTIME_DIR"));

    // The destination is created on the server before the copy (and only then).
    RsyncJob::Transfer t;
    t.host = QStringLiteral("h");
    t.sources = QStringList{"/a"};
    t.destination = QStringLiteral("new dir/");
    t.createDestination = true;
    QCOMPARE(RsyncJob::command(t, RsyncOptions(), RsyncJob::Pass::Copy, true).arguments.last(),
             QStringLiteral("echo RSYNC-GUI-PID $$; mkdir -p './new dir/' && exec rsync -avhP --no-inc-recursive --info=flist2 -- /a 'new dir/'"));
    QCOMPARE(RsyncJob::command(t, RsyncOptions(), RsyncJob::Pass::Verify).arguments.last(),
             QStringLiteral("rsync -nrlci '--out-format=%i %n%L' -- /a 'new dir/'"));
}

void TestRsyncCore::probeParse()
{
    const QStringList sources{"/data/a", "/data/missing", "rel"};
    const char raw[] = "Welcome to the server\0RSYNC-GUI-PROBE\0rsync\0rsync  version 3.4.1  protocol version 32\0"
                       "source\0dir\0/real/data/a\0source\0missing\0\0source\0file\0/home/u/rel\0"
                       "destination\0missing\0\0parent\0/data/a\0yes\0" "1234\0/real/data/a\0end\0";
    const QByteArray out(raw, sizeof(raw) - 1);
    RemoteProbe::Result r = RemoteProbe::parse(out, sources, "/data/a/new/sub");
    QVERIFY(r.ran);
    QVERIFY(r.rsyncFound);
    QCOMPARE(r.rsyncVersion, QStringLiteral("rsync  version 3.4.1  protocol version 32"));
    QCOMPARE(r.output, QStringLiteral("Welcome to the server"));
    QCOMPARE(r.sources.size(), 3);
    QCOMPARE(r.sources.at(0).type, RemoteProbe::Type::Directory);
    QCOMPARE(r.sources.at(2).type, RemoteProbe::Type::File);
    QCOMPARE(r.missingSources(), QStringList{"/data/missing"});
    QCOMPARE(r.destination.type, RemoteProbe::Type::Missing);
    QCOMPARE(r.existingParent, QStringLiteral("/data/a"));
    QVERIFY(r.parentWritable);
    QCOMPARE(r.freeBytes, qint64(1234) * 1024);
    // The destination does not exist yet; its parent (by the real path) is inside the source "/data/a".
    QCOMPARE(r.destinationInsideSource(), QStringLiteral("/data/a"));
    QCOMPARE(r.error(), QString());

    // No marker: the command did not run (sftp only); 255: ssh failed; no rsync.
    r = RemoteProbe::parse("This service allows sftp connections only.\n", sources, "/x");
    r.exitCode = 1;
    QVERIFY(!r.ran);
    QVERIFY(r.error().contains("sftp only"));
    QVERIFY(r.error().contains("This service allows sftp connections only."));
    r.exitCode = 255;
    QVERIFY(r.error().contains("ssh could not connect"));
    const char noRsync[] = "RSYNC-GUI-PROBE\0destination\0dir\0/x\0end\0";
    r = RemoteProbe::parse(QByteArray(noRsync, sizeof(noRsync) - 1), {}, "/x");
    QVERIFY(r.error().contains("rsync is not installed"));
    QCOMPARE(RemoteProbe::parseSize("12\t./a\n30\t/b c\n"), qint64(42 * 1024));
    QCOMPARE(RemoteProbe::parseSize(""), qint64(-1));
}

void TestRsyncCore::probeScriptLocally()
{
    // The probe script with the local shells, as the login shell of a server would run it (in the home directory).
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString home = QFileInfo(tmp.path()).canonicalFilePath();
    writeFile(home + "/src dir/f", "12345");
    writeFile(home + "/file", "x");
    QVERIFY(QDir().mkpath(home + "/dest"));
    QVERIFY(QFile::link(home + "/src dir", home + "/link"));
    const QByteArray odd = LocalPath::encode(home) + "/-caf\xe9";
    QVERIFY(::mkdir(odd.constData(), 0755) == 0);
    const auto cleanup = qScopeGuard([&] { removeTree(odd); });
    const QStringList sources{"src dir", home + "/file", "missing it's", "link", LocalPath::decode("-caf\xe9")};
    const QList<QStringList> shells = availableShells();
    QVERIFY(!shells.isEmpty());
    for (const QStringList &shell : shells) {
        int exitCode = -1;
        const QByteArray out = runRemoteCommandLocally(
            shell, RemoteProbe::remoteCommand(sources, QStringLiteral("link/new/deeper")), home, &exitCode);
        QCOMPARE(exitCode, 0);
        const RemoteProbe::Result r = RemoteProbe::parse(out, sources, QStringLiteral("link/new/deeper"));
        QVERIFY2(r.ran, qPrintable(shell.join(' ')));
        QCOMPARE(r.rsyncFound, haveRsync());
        QCOMPARE(r.sources.size(), 5);
        QCOMPARE(r.sources.at(0).type, RemoteProbe::Type::Directory);
        QCOMPARE(r.sources.at(0).realPath, home + "/src dir");
        QCOMPARE(r.sources.at(1).type, RemoteProbe::Type::File);
        QCOMPARE(r.missingSources(), QStringList{"missing it's"});
        QCOMPARE(r.sources.at(3).realPath, home + "/src dir"); // the symlink resolved
        QCOMPARE(r.sources.at(4).type, RemoteProbe::Type::Directory); // a name starting with "-", not UTF-8
        QCOMPARE(r.destination.type, RemoteProbe::Type::Missing);
        QCOMPARE(r.existingParent, QStringLiteral("./link"));
        QVERIFY(r.parentWritable);
        QVERIFY(r.freeBytes > 0);
        // "link/new/deeper" is inside "src dir" through the symlink.
        QCOMPARE(r.destinationInsideSource(), QStringLiteral("src dir"));
        // The size of the sources (du -sk): at least the 5 bytes rounded up to a block.
        const QByteArray du = runRemoteCommandLocally(shell, RemoteProbe::sizeCommand({"src dir", "missing"}), home);
        QVERIFY2(RemoteProbe::parseSize(du) >= 4096, du.constData());
    }
    const RemoteProbe::Result fine = RemoteProbe::parse(
        runRemoteCommandLocally(shells.first(), RemoteProbe::remoteCommand({"src dir"}, home + "/dest"), home),
        {"src dir"}, home + "/dest");
    QCOMPARE(fine.destination.type, RemoteProbe::Type::Directory);
    QCOMPARE(fine.destinationInsideSource(), QString());
}

void TestRsyncCore::probeTargets()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("f"), "f");
    RsyncOptions o;
    // On the server, from it, to it, local.
    auto plan = localPlan({"u@h:/a", "u@h:/b c"}, "u@h:/d", o);
    auto target = RemoteProbe::targetFor(plan.transfers.first());
    QCOMPARE(target->kind, RemoteProbe::Target::Kind::OnServer);
    QCOMPARE(target->host, QStringLiteral("u@h"));
    QCOMPARE(target->destination, QStringLiteral("/d/"));
    plan = RsyncJob::plan({Location::parse("sftp://u@h:2222/x y"), Location::parse("sftp://u@h:2222/z")},
                          Location::parse(tmp.filePath("dest")), o);
    target = RemoteProbe::targetFor(plan.transfers.first());
    QCOMPARE(target->kind, RemoteProbe::Target::Kind::From);
    QCOMPARE(target->host, QStringLiteral("u@h"));
    QCOMPARE(target->port, 2222);
    QCOMPARE(target->sources, (QStringList{"/x y", "/z"}));
    QCOMPARE(target->sourceSpecs, plan.transfers.first().sources);
    QCOMPARE(target->destination, QString());
    plan = localPlan({tmp.filePath("f")}, "h:backup", o);
    target = RemoteProbe::targetFor(plan.transfers.first());
    QCOMPARE(target->kind, RemoteProbe::Target::Kind::To);
    QCOMPARE(target->host, QStringLiteral("h"));
    QCOMPARE(target->sources, QStringList());
    QCOMPARE(target->destination, QStringLiteral("backup/"));
    plan = localPlan({tmp.filePath("f")}, tmp.filePath("dest"), o);
    QVERIFY(!RemoteProbe::targetFor(plan.transfers.first()));

    // Without a destination only the sources are checked.
    for (const QStringList &shell : availableShells()) {
        const RemoteProbe::Result r = RemoteProbe::parse(
            runRemoteCommandLocally(shell, RemoteProbe::remoteCommand({"f", "missing"}, QString()), tmp.path()),
            {"f", "missing"}, QString());
        QVERIFY(r.ran);
        QVERIFY(!r.destinationChecked);
        QCOMPARE(r.missingSources(), QStringList{"missing"});
        QCOMPARE(r.existingParent, QString());
        QCOMPARE(r.error(), haveRsync() ? QString() : r.error());
    }
}

void TestRsyncCore::cleanUpScriptLocally()
{
    // The removal of emptied source directories after a move on the server, with the local shells.
    for (const QStringList &shell : availableShells()) {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString home = tmp.path();
        QVERIFY(QDir().mkpath(home + "/a it's/x/y"));
        QVERIFY(QDir().mkpath(home + "/a it's/z"));
        writeFile(home + "/a it's/z/kept", "k"); // not empty: stays (with its parents)
        QVERIFY(QDir().mkpath(home + "/b/c"));
        QVERIFY(QDir().mkpath(home + "/target/t"));
        QVERIFY(QFile::link(home + "/target", home + "/link")); // a symlink: not followed
        RsyncJob::Transfer t;
        t.host = QStringLiteral("h");
        t.sources = QStringList{"a it's", "b/", "link", "missing"};
        const RsyncJob::Command c = RsyncJob::command(t, RsyncOptions(), RsyncJob::Pass::CleanUp);
        QCOMPARE(c.arguments.mid(c.arguments.size() - 2, 1), QStringList{"h"});
        int exitCode = -1;
        const QByteArray out = runRemoteCommandLocally(shell, c.arguments.last(), home, &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY(!QFileInfo::exists(home + "/a it's/x"));
        QVERIFY(QFileInfo::exists(home + "/a it's/z/kept"));
        QVERIFY(QFileInfo::exists(home + "/b"));       // "b/": only the content was moved, b stays
        QVERIFY(!QFileInfo::exists(home + "/b/c"));
        QVERIFY(QFileInfo::exists(home + "/target/t")); // not through the symlink
        const QList<QByteArray> fields = out.split('\0');
        QVERIFY2(fields.contains("removed") && fields.contains("./a it's") && fields.contains("2"), out.constData());
        QVERIFY(fields.contains("left"));
    }
}

void TestRsyncCore::mountInfo()
{
    const QByteArray info =
        "22 1 8:2 / / rw,relatime shared:1 - ext4 /dev/sda2 rw\n"
        "41 30 0:51 / /mnt/video rw,relatime shared:858 - cifs //10.0.0.6/Video rw,vers=3.1.1,username=anna,uid=1000\n"
        "42 30 0:52 /sub/dir /media/old\\040disk rw shared:9 master:2 - cifs //nas/Data rw,username=bob\n"
        "43 30 0:53 / /mnt/sshfs rw,nosuid - fuse.sshfs anna@server:/srv/photos rw,user_id=1000\n"
        "44 30 0:54 / /mnt/home rw - fuse.sshfs server: rw\n"
        "45 30 0:55 / /mnt/v6 rw - fuse.sshfs [fe80::1]:/x rw\n"
        "bad line\n";
    const QList<Mount> mounts = Mount::parse(info);
    QCOMPARE(mounts.size(), 6);
    QCOMPARE(mounts.at(2).mountPoint, QStringLiteral("/media/old disk"));
    QCOMPARE(mounts.at(2).root, QStringLiteral("/sub/dir"));
    QCOMPARE(mounts.at(2).source, QStringLiteral("//nas/Data"));
    QCOMPARE(Mount::find(mounts, "/mnt/video/a/b")->mountPoint, QStringLiteral("/mnt/video"));
    QCOMPARE(Mount::find(mounts, "/mnt/video")->mountPoint, QStringLiteral("/mnt/video"));
    QCOMPARE(Mount::find(mounts, "/mnt/video2")->mountPoint, QStringLiteral("/"));
    QCOMPARE(Mount::find(mounts, "/media/old disk/x")->fsType, QStringLiteral("cifs"));
    // The real table can be read (whatever is mounted here).
    QVERIFY(!Mount::current().isEmpty());
}

void TestRsyncCore::networkShares()
{
    const QList<Mount> mounts = Mount::parse(
        "22 1 8:2 / / rw - ext4 /dev/sda2 rw\n"
        "41 30 0:51 / /mnt/video rw - cifs //10.0.0.6/Video rw,vers=3.1.1,username=anna,uid=1000\n"
        "42 30 0:52 /sub/dir /media/old\\040disk rw - cifs //nas/Data rw,username=bob\n"
        "46 30 0:56 / /media/pre rw - cifs //nas/Pre rw,prefixpath=/a/b\n"
        "43 30 0:53 / /mnt/sshfs rw - fuse.sshfs anna@server:/srv/photos rw\n"
        "44 30 0:54 / /mnt/home rw - fuse.sshfs server: rw\n"
        "45 30 0:55 / /mnt/v6 rw - fuse.sshfs [fe80::1]:/x rw\n");
    QString rel;
    auto share = NetworkShare::of(mounts, "/mnt/video/2024/a.mkv", &rel);
    QVERIFY(share);
    QCOMPARE(share->kind, NetworkShare::Kind::Cifs);
    QCOMPARE(share->key, QStringLiteral("//10.0.0.6/Video"));
    QCOMPARE(share->server, QStringLiteral("10.0.0.6"));
    QCOMPARE(share->share, QStringLiteral("Video"));
    QCOMPARE(share->user, QStringLiteral("anna"));
    QCOMPARE(share->remotePath, QString());
    QCOMPARE(rel, QStringLiteral("2024/a.mkv"));
    share = NetworkShare::of(mounts, "/media/old disk", &rel);
    QCOMPARE(share->remotePath, QStringLiteral("sub/dir")); // mounted from a subdirectory of the share
    QCOMPARE(share->display(), QStringLiteral("//nas/Data/sub/dir"));
    QCOMPARE(rel, QString());
    QCOMPARE(NetworkShare::of(mounts, "/media/pre/x")->remotePath, QStringLiteral("a/b"));
    share = NetworkShare::of(mounts, "/mnt/sshfs/2020", &rel);
    QCOMPARE(share->kind, NetworkShare::Kind::Sshfs);
    QCOMPARE(share->key, QStringLiteral("sshfs:anna@server:/srv/photos"));
    QCOMPARE(share->user, QStringLiteral("anna"));
    QCOMPARE(share->server, QStringLiteral("server"));
    QCOMPARE(share->remotePath, QStringLiteral("/srv/photos"));
    QCOMPARE(NetworkShare::of(mounts, "/mnt/home")->remotePath, QString());
    QCOMPARE(NetworkShare::of(mounts, "/mnt/v6")->server, QStringLiteral("fe80::1"));
    QVERIFY(!NetworkShare::of(mounts, "/home/user"));
    QVERIFY(NetworkShare::sharePathCommand("my share").contains("--section-name='my share'"));
    // Known only from the settings (not mounted).
    QCOMPARE(NetworkShare::fromKey("//nas/Video").share, QStringLiteral("Video"));
    QCOMPARE(NetworkShare::fromKey("//nas/Video").mountPoint, QString());
    QCOMPARE(NetworkShare::fromKey("sshfs:anna@server:/srv").remotePath, QStringLiteral("/srv"));
    QCOMPARE(NetworkShare::fromKey("sshfs:anna@server:/srv").key, QStringLiteral("sshfs:anna@server:/srv"));

    // Detected settings: sshfs knows everything, CIFS only the server and the user.
    ShareSettings detected = ShareSettings::detected(*NetworkShare::of(mounts, "/mnt/sshfs"));
    QCOMPARE(detected.sshTarget, QStringLiteral("anna@server"));
    QCOMPARE(detected.serverPath, QStringLiteral("/srv/photos"));
    QCOMPARE(ShareSettings::detected(*NetworkShare::of(mounts, "/mnt/home")).serverPath, QStringLiteral("~"));
    detected = ShareSettings::detected(*NetworkShare::of(mounts, "/mnt/video"));
    QCOMPARE(detected.sshTarget, QStringLiteral("anna@10.0.0.6"));
    QCOMPARE(detected.serverPath, QString());
    QCOMPARE(detected.use, ShareSettings::Use::Unset);
}

void TestRsyncCore::shareSettings()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QSettings settings(tmp.filePath("s.conf"), QSettings::IniFormat);
    ShareSettings a;
    a.use = ShareSettings::Use::Enabled;
    a.sshTarget = QStringLiteral("anna@nas");
    a.port = 2222;
    a.serverPath = QStringLiteral("/volume1/video/");
    a.save(settings, "//10.0.0.6/Video");
    ShareSettings never;
    never.use = ShareSettings::Use::Never;
    never.save(settings, "sshfs:x@y:/z");
    QCOMPARE(ShareSettings::savedKeys(settings), (QStringList{"//10.0.0.6/Video", "sshfs:x@y:/z"}));
    const ShareSettings b = ShareSettings::load(settings, "//10.0.0.6/Video");
    QCOMPARE(b.use, ShareSettings::Use::Enabled);
    QCOMPARE(b.sshTarget, a.sshTarget);
    QCOMPARE(b.port, 2222);
    QVERIFY(b.isUsable());
    QCOMPARE(ShareSettings::load(settings, "sshfs:x@y:/z").use, ShareSettings::Use::Never);
    QCOMPARE(ShareSettings::load(settings, "unknown").use, ShareSettings::Use::Unset);
    Location loc = b.location("2024/a b");
    QCOMPARE(loc.user, QStringLiteral("anna"));
    QCOMPARE(loc.host, QStringLiteral("nas"));
    QCOMPARE(loc.port, 2222);
    QCOMPARE(loc.path, QStringLiteral("/volume1/video/2024/a b"));
    QCOMPARE(b.location(QString()).path, QStringLiteral("/volume1/video"));
    ShareSettings::remove(settings, "sshfs:x@y:/z");
    QCOMPARE(ShareSettings::savedKeys(settings), QStringList{"//10.0.0.6/Video"});
}

void TestRsyncCore::shareRouting()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    // Mount points in a temporary directory, so that "is a directory" can be looked at through them.
    const QString video = tmp.filePath("video");
    const QString audio = tmp.filePath("audio");
    const QString photos = tmp.filePath("photos");
    QVERIFY(QDir().mkpath(video + "/dir"));
    QVERIFY(QDir().mkpath(audio));
    QVERIFY(QDir().mkpath(photos));
    const auto line = [](int id, const QString &point, const QString &type, const QString &source) {
        return QStringLiteral("%1 1 0:%1 / %2 rw - %3 %4 rw\n").arg(id).arg(point, type, source).toUtf8();
    };
    const QList<Mount> mounts = Mount::parse("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n" + line(2, video, "cifs", "//nas/Video")
                                             + line(3, audio, "cifs", "//nas/Audio")
                                             + line(4, photos, "fuse.sshfs", "u@other:/srv/photos"));
    QHash<QString, ShareSettings> stored;
    ShareSettings nasVideo;
    nasVideo.use = ShareSettings::Use::Enabled;
    nasVideo.sshTarget = QStringLiteral("nas");
    nasVideo.serverPath = QStringLiteral("/volume1/video");
    nasVideo.chmod = QStringLiteral("Fg-wx,Fo-wx");
    stored.insert("//nas/Video", nasVideo);
    ShareSettings otherPhotos = ShareSettings::detected(*NetworkShare::of(mounts, photos));
    otherPhotos.use = ShareSettings::Use::Enabled;
    stored.insert("sshfs:u@other:/srv/photos", otherPhotos);
    const auto lookup = [&stored](const QString &key) { return stored.value(key); };
    const Location local = Location::parse(tmp.filePath("local"));

    // Both on one host: rsync on the server (plan() makes it one transfer there).
    ShareRouting r = ShareRouting::route({Location::parse(video + "/dir")}, Location::parse(video + "/backup"), mounts,
                                         lookup, true);
    QCOMPARE(r.sources.first().toString(), QStringLiteral("nas:/volume1/video/dir"));
    QCOMPARE(r.destination.toString(), QStringLiteral("nas:/volume1/video/backup"));
    QVERIFY(!r.throughNetworkTwice);
    QCOMPARE(r.notes.size(), 2);
    QCOMPARE(r.destinationChmod, QStringLiteral("Fg-wx,Fo-wx")); // the destination's share
    QCOMPARE(ShareRouting::route({Location::parse(video + "/dir")}, Location::parse(tmp.filePath("local")), mounts, lookup,
                                 true).destinationChmod, QString()); // not for a local destination
    RsyncJob::Plan plan = RsyncJob::plan(r.sources, r.destination, RsyncOptions());
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.transfers.first().runsOnServer());
    // Contents only: the "/" is added for the server.
    r = ShareRouting::route({Location::parse(video + "/dir")}, local, mounts, lookup, true, true);
    QCOMPARE(r.sources.first().path, QStringLiteral("/volume1/video/dir/"));

    // Local <-> share: over ssh.
    r = ShareRouting::route({local}, Location::parse(video), mounts, lookup, true);
    QCOMPARE(r.sources.first(), local);
    QCOMPARE(r.destination.toString(), QStringLiteral("nas:/volume1/video"));
    r = ShareRouting::route({Location::parse(video + "/dir")}, local, mounts, lookup, true);
    QCOMPARE(r.sources.first().toString(), QStringLiteral("nas:/volume1/video/dir"));
    QCOMPARE(r.destination, local);

    // Another host than the destination: through the mount (rsync cannot copy between two hosts).
    r = ShareRouting::route({Location::parse(photos + "/p"), Location::parse(video + "/dir")}, Location::parse(video),
                            mounts, lookup, true);
    QCOMPARE(r.sources.at(0), Location::parse(photos + "/p"));
    QCOMPARE(r.sources.at(1).host, QStringLiteral("nas"));
    QVERIFY(r.throughNetworkTwice);

    // A share that is not set up: through the mount, offered to set it up; with both on network mounts a warning.
    r = ShareRouting::route({Location::parse(audio + "/a")}, Location::parse(audio + "/b"), mounts, lookup, true);
    QCOMPARE(r.sources.first(), Location::parse(audio + "/a"));
    QCOMPARE(r.unsetKeys, QStringList{"//nas/Audio"});
    QCOMPARE(r.unsetShares.first().share, QStringLiteral("Audio"));
    QVERIFY(r.throughNetworkTwice);
    stored.insert("//nas/Audio", ShareSettings{ShareSettings::Use::Never, {}, 0, {}});
    QVERIFY(ShareRouting::route({Location::parse(audio + "/a")}, local, mounts, lookup, true).unsetKeys.isEmpty());

    // The option off: nothing is changed and nothing offered.
    stored.remove("//nas/Audio");
    r = ShareRouting::route({Location::parse(video + "/dir")}, Location::parse(audio), mounts, lookup, false);
    QCOMPARE(r.sources.first(), Location::parse(video + "/dir"));
    QCOMPARE(r.destination, Location::parse(audio));
    QVERIFY(r.unsetKeys.isEmpty());
    QVERIFY(r.throughNetworkTwice);
    // Remote locations are left as they are.
    r = ShareRouting::route({Location::parse("sftp://nas/x")}, Location::parse(video), mounts, lookup, true);
    QCOMPARE(r.sources.first(), Location::parse("sftp://nas/x"));
    QVERIFY(RsyncJob::plan(r.sources, r.destination, RsyncOptions()).transfers.first().runsOnServer());
}

void TestRsyncCore::sambaChmod()
{
    // Samba's defaults: create mask 0744, directory mask 0755.
    QCOMPARE(NetworkShare::chmodFromSamba({}), QStringLiteral("Fg-wx,Fo-wx,Dg-w,Do-w"));
    QCOMPARE(NetworkShare::chmodFromSamba({{"create mask", "0777"}, {"directory mask", "0777"}}), QString());
    const QMap<QString, QString> parameters = NetworkShare::parseParameters(
        "force user=tester\nforce group=\ncreate mask=0640\nforce create mode=0004\ndirectory mask=0750\n"
        "force directory mode=0000\n");
    QCOMPARE(parameters.value("force user"), QStringLiteral("tester"));
    QVERIFY(!parameters.contains("force group"));
    const QString chmod = NetworkShare::chmodFromSamba(parameters);
    QCOMPARE(chmod, QStringLiteral("Fu-x,Fg-wx,Fo-rwx,Fo+r,Dg-w,Do-rwx"));
    if (!haveRsync())
        QSKIP("rsync is not installed");
    // rsync applies it as Samba would: (mode & mask) | force.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("src/d/f"), "f");
    QCOMPARE(::chmod(QFile::encodeName(tmp.filePath("src/d/f")).constData(), 0777), 0);
    QCOMPARE(::chmod(QFile::encodeName(tmp.filePath("src/d")).constData(), 0777), 0);
    QCOMPARE(QProcess::execute("rsync", {"-a", "--chmod=" + chmod, tmp.filePath("src/d"), tmp.filePath("dest")}), 0);
    struct stat st;
    QCOMPARE(::stat(QFile::encodeName(tmp.filePath("dest/d/f")).constData(), &st), 0);
    QCOMPARE(st.st_mode & 07777, mode_t(0644)); // 0777 & 0640 | 0004
    QCOMPARE(::stat(QFile::encodeName(tmp.filePath("dest/d")).constData(), &st), 0);
    QCOMPARE(st.st_mode & 07777, mode_t(0750));
}

void TestRsyncCore::sshHostsUnify()
{
    // An alias and user@address of one server and account: one origin, so rsync runs there.
    const auto resolver = [](const Location &loc) {
        if (loc.host == "nas-files" || (loc.host == "10.0.0.6" && loc.user == "shareuser"))
            return SshIdentity{"shareuser", "10.0.0.6", 22};
        if (loc.host == "10.0.0.6")
            return SshIdentity{"other", "10.0.0.6", 22};
        return SshIdentity{};
    };
    QList<Location> sources{Location::parse("sftp://nas-files/srv/a"), Location::parse("/local")};
    Location dest = Location::parse("shareuser@10.0.0.6:/volume1/b");
    const QStringList notes = SshHosts::unify(sources, dest, resolver);
    QCOMPARE(dest.toString(), QStringLiteral("nas-files:/volume1/b"));
    QCOMPARE(sources.at(1), Location::parse("/local"));
    QCOMPARE(notes.size(), 1);
    QVERIFY2(notes.first().contains("nas-files"), qPrintable(notes.first()));
    const RsyncJob::Plan plan = RsyncJob::plan({sources.first()}, dest, RsyncOptions());
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.transfers.first().runsOnServer());
    // Another account on the same address is another origin; unknown hosts are left as they are.
    sources = {Location::parse("sftp://nas-files/srv/a")};
    dest = Location::parse("10.0.0.6:/b");
    QVERIFY(SshHosts::unify(sources, dest, resolver).isEmpty());
    QCOMPARE(dest.host, QStringLiteral("10.0.0.6"));
    sources = {Location::parse("x:/a")};
    dest = Location::parse("y:/b");
    QVERIFY(SshHosts::unify(sources, dest, resolver).isEmpty());

    // The suggested login for a share: a location of the copy on that server first.
    QCOMPARE(SshHosts::suggestTarget({"ssh", "-F", "/dev/null"}, "10.0.0.6", "shareuser",
                                     {Location::parse("/x"), Location::parse("nas-files:/a")}, resolver),
             QStringLiteral("nas-files"));
    QCOMPARE(SshHosts::suggestTarget({"ssh", "-F", "/dev/null"}, "10.0.0.7", "anna", {}, resolver),
             QStringLiteral("anna@10.0.0.7"));
}

void TestRsyncCore::sshHostsConfig()
{
    if (QStandardPaths::findExecutable("ssh").isEmpty())
        QSKIP("ssh is not installed");
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString config = tmp.filePath("config");
    writeFile(config, "Host files nas-files\n    HostName 10.0.0.6\n    User shareuser\n"
                      "Host *.lan !secret wild?\n    User nobody\n"
                      "Host=other\n    HostName 10.0.0.9\n    Port 2222\n");
    const QStringList ssh{"ssh", "-F", config};
    QCOMPARE(SshHosts::configFile(ssh), config);
    QCOMPARE(SshHosts::configFile({"ssh"}), QDir::homePath() + "/.ssh/config");
    QCOMPARE(SshHosts::configAliases(config), (QStringList{"files", "nas-files", "other"}));
    const SshIdentity alias = SshHosts::resolve(ssh, "nas-files", 0);
    QCOMPARE(alias.user, QStringLiteral("shareuser"));
    QCOMPARE(alias.hostname, QStringLiteral("10.0.0.6"));
    QCOMPARE(alias.port, 22);
    QVERIFY(alias == SshHosts::resolve(ssh, "shareuser@10.0.0.6", 0));
    QVERIFY(!(alias == SshHosts::resolve(ssh, "root@10.0.0.6", 0)));
    QCOMPARE(SshHosts::resolve(ssh, "other", 0).port, 2222);
    QCOMPARE(SshHosts::resolve(ssh, "other", 2200).port, 2200); // -p wins
    QVERIFY(!SshHosts::resolve({"plink"}, "other", 0).isValid());
    QCOMPARE(SshHosts::aliasFor(ssh, "10.0.0.6", "shareuser"), QStringLiteral("files"));
    QCOMPARE(SshHosts::aliasFor(ssh, "10.0.0.6", "root"), QString());
    QCOMPARE(SshHosts::aliasFor(ssh, "10.0.0.9", QString()), QStringLiteral("other"));
    // The CIFS user differs from the ssh one: an alias of the server is still better than user@address.
    const auto resolver = [&ssh](const Location &loc) {
        return SshHosts::resolve(ssh, loc.user.isEmpty() ? loc.host : loc.user + "@" + loc.host, loc.port);
    };
    QCOMPARE(SshHosts::suggestTarget(ssh, "10.0.0.6", "smbuser", {}, resolver), QStringLiteral("files"));
    QCOMPARE(SshHosts::suggestTarget(ssh, "10.0.0.8", "smbuser", {}, resolver), QStringLiteral("smbuser@10.0.0.8"));

    // A source on a server copied to a network mount here: through this computer, both ways over the network.
    const QString share = tmp.filePath("share");
    QVERIFY(QDir().mkpath(share));
    const QList<Mount> mounts = Mount::parse(QStringLiteral("1 0 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                                            "2 1 0:2 / %1 rw - cifs //10.0.0.6/Video rw\n").arg(share).toUtf8());
    const ShareRouting r = ShareRouting::route({Location::parse("nas-files:/a")}, Location::parse(share), mounts,
                                               [](const QString &) { return ShareSettings{ShareSettings::Use::Never, {}, 0, {}, {}}; },
                                               true);
    QVERIFY(r.throughNetworkTwice);
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
    QList<RsyncJob::FileCount> files;
    connect(&job, &RsyncJob::filesChanged, this, [&files](const RsyncJob::FileCount &count) { files << count; });
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QVERIFY(!rates.isEmpty());
    // "src", "src/a", "src/c", "src/sub", "src/sub/b" - three of them files that were copied.
    QVERIFY(!files.isEmpty());
    // The total is known before the first file is finished (--no-inc-recursive --info=flist2).
    QCOMPARE(files.constFirst().total, 5);
    QCOMPARE(files.constFirst().checked, 0);
    QVERIFY(files.constFirst().totalFinal);
    QCOMPARE(files.constLast().checked, 5);
    QCOMPARE(files.constLast().total, 5);
    QVERIFY(files.constLast().totalFinal);
    QCOMPARE(files.constLast().transferred, 3);
    for (qsizetype i = 1; i < files.size(); ++i) {
        QVERIFY(files.at(i).checked >= files.at(i - 1).checked);
        QVERIFY(files.at(i).transferred >= files.at(i - 1).transferred);
    }
    const qint64 processed = rates.last().at(1).toLongLong();
    // -h rounds to 3 significant digits ("600K").
    QVERIFY2(qAbs(processed - 600000) <= (human ? 5000 : 0), qPrintable(QString::number(processed)));
    for (const QList<QVariant> &args : std::as_const(rates))
        QVERIFY(args.at(0).toLongLong() >= 0);
}

void TestRsyncCore::fileListTotal()
{
    QCOMPARE(RsyncJob::parseFileListTotal(QStringLiteral("1501 files to consider")), 1501);
    QCOMPARE(RsyncJob::parseFileListTotal(QStringLiteral("1 file to consider")), 1);
    QCOMPARE(RsyncJob::parseFileListTotal(QStringLiteral("receiving file list ... 3 files to consider")), 3);
    QCOMPARE(RsyncJob::parseFileListTotal(QStringLiteral(" 100 files...")), -1);
    QCOMPARE(RsyncJob::parseFileListTotal(QStringLiteral("building file list ... ")), -1);

    // Only with progress and recursion; an explicit (--no-)inc-recursive of the extra arguments wins.
    RsyncOptions options;
    QVERIFY(options.arguments().contains(QStringLiteral("--no-inc-recursive")));
    options.extraArguments = QStringLiteral("--inc-recursive");
    QVERIFY(!options.arguments().contains(QStringLiteral("--no-inc-recursive")));
    options.extraArguments.clear();
    options.progress = RsyncOptions::Progress::None;
    QVERIFY(!options.arguments().contains(QStringLiteral("--info=flist2")));
}

void TestRsyncCore::fileCountText()
{
    const QLocale previous;
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    const auto restore = qScopeGuard([&previous] { QLocale::setDefault(previous); });
    QCOMPARE(RsyncJob::fileCountText({}), QString());
    RsyncJob::FileCount count;
    count.checked = 1200;
    count.total = 5000;
    count.transferred = 340;
    const QString growing = RsyncJob::fileCountText(count);
    QVERIFY2(growing.contains(QStringLiteral("5,000+")) && growing.contains(QStringLiteral("3,800+ left"))
                 && growing.endsWith(QStringLiteral("340")),
             qPrintable(growing));
    count.totalFinal = true;
    QVERIFY(!RsyncJob::fileCountText(count).contains(QLatin1Char('+')));
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
    // "Source directory not empty - left in place": done, but with a warning (the window does not close).
    QVERIFY(job.summary().warnings > 0);
    QVERIFY(!job.summary().clean());

    // Contents only ("dir/"): the source directory itself stays.
    options.contentsOnly = true;
    options.extraArguments.clear();
    RsyncJob contents(localPlan({tmp.filePath("contents")}, dest, options).transfers, options);
    QVERIFY(runJob(contents));
    QVERIFY(contents.summary().ok());
    QVERIFY(contents.summary().clean());
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

void TestRsyncCore::runLogDirectory()
{
    QVERIFY(RunLog::defaultDirectory(QStringLiteral("rsync-gui")).endsWith(QStringLiteral("/rsync-gui/logs")));
    const QString home = QDir::homePath();
    QCOMPARE(RunLog::displayDirectory(home + QStringLiteral("/.local/state/rsync-gui/logs")),
             QStringLiteral("~/.local/state/rsync-gui/logs"));
    QCOMPARE(RunLog::displayDirectory(home + QStringLiteral("x/logs")), home + QStringLiteral("x/logs"));

    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("a/b/logs"));
    QVERIFY(RunLog::makeDirectory(dir));
    QCOMPARE(QFile::permissions(dir) & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
    RunLog log;
    QString error;
    QVERIFY2(log.open(dir, QDateTime(QDate(2026, 11, 14), QTime(12, 0)), &error), qPrintable(error));
    QCOMPARE(log.path(), dir + QStringLiteral("/2026-11-14_12-00-00.log"));
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
    o.logToFile = false;

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
    QTest::newRow("rsync-copy") << "rsync-copy" << QStringList{"-avhP"} + kFileList << false;
    QTest::newRow("rsync-copy-verify") << "rsync-copy-verify" << QStringList{"-avhPc"} + kFileList << true;
    QTest::newRow("rsync-copy-portable") << "rsync-copy-portable" << QStringList{"-rltvhP"} + kFileList << false;
    QTest::newRow("rsync-move") << "rsync-move" << QStringList{"-avhP", "--remove-source-files"} + kFileList << false;
    QTest::newRow("rsync-move-verify") << "rsync-move-verify" << QStringList{"-avhPc"} + kFileList << true;
    QTest::newRow("rsync-move-portable") << "rsync-move-portable" << QStringList{"-rltvhP", "--remove-source-files"} + kFileList
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
    // The log is saved to a file by default; presets without the key keep the current choice.
    QVERIFY(RsyncOptions().logToFile);
    {
        RsyncOptions base;
        base.logToFile = false;
        QString error;
        QVERIFY(!RsyncOptions::fromJson(QJsonObject{{"checksum", true}}, &error, base).logToFile);
    }
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
    // The thread has ended by the time delete returns. Compared with the count right before the delete, not with
    // the one at the start: other threads (Qt's thread pool) may start or end in the meantime.
    QVERIFY2(threadsAfter < threadsDuring, qPrintable(QStringLiteral("%1 threads before the delete, %2 after")
                                                          .arg(threadsDuring).arg(threadsAfter)));
}

void TestRsyncCore::speedHistoryAverages()
{
    SpeedHistory history(512, 250, 5000);
    qint64 bucketMs = 0;
    QVERIFY(history.averages(1000, &bucketMs).isEmpty());
    QCOMPARE(history.peak(1000), 0.0);

    // Each sample holds until the next one; the last one until "now".
    history.addSample(0, 1000);
    history.addSample(500, 3000);
    QCOMPARE(history.averages(1000, &bucketMs), QList<double>({1000, 1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(250));
    QCOMPARE(history.peak(1000), 3000.0);

    // A change in the middle of a bucket: the average weighted by time.
    history.clear();
    history.addSample(0, 1000);
    history.addSample(125, 3000);
    QCOMPARE(history.averages(250), QList<double>({2000}));
    // Negative speeds (unknown) count as 0.
    history.clear();
    history.addSample(0, -1);
    QCOMPARE(history.averages(250), QList<double>({0}));
}

void TestRsyncCore::speedHistoryGaps()
{
    SpeedHistory history(512, 250, 5000);
    history.addSample(0, 1000);
    history.pause(500);
    history.addSample(1000, 2000);
    QCOMPARE(history.averages(1500), QList<double>({1000, 1000, -1, -1, 2000, 2000}));
    history.pause(1500);
    QCOMPARE(history.averages(3000), QList<double>({1000, 1000, -1, -1, 2000, 2000}));

    // A sample does not hold for longer than 5 s.
    history.clear();
    history.addSample(0, 1000);
    QCOMPARE(history.averages(10000), QList<double>(20, 1000));
    history.addSample(10000, 500);
    const QList<double> averages = history.averages(10250);
    QCOMPARE(averages.size(), 41);
    QCOMPARE(averages.mid(0, 20), QList<double>(20, 1000));
    QCOMPARE(averages.mid(20, 20), QList<double>(20, -1));
    QCOMPARE(averages.last(), 500.0);
}

void TestRsyncCore::speedHistoryMergesBuckets()
{
    SpeedHistory history(4, 100, 100000);
    history.addSample(0, 1000);
    history.addSample(400, 3000);
    qint64 bucketMs = 0;
    QCOMPARE(history.averages(400, &bucketMs), QList<double>(4, 1000));
    QCOMPARE(bucketMs, qint64(100));
    // "Now" beyond 4 buckets: the result has longer buckets, the history itself does not change.
    QCOMPARE(history.averages(800, &bucketMs), QList<double>({1000, 1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(200));
    QCOMPARE(history.averages(400, &bucketMs), QList<double>(4, 1000));
    QCOMPARE(bucketMs, qint64(100));
    // Merged twice; the last bucket has a known speed for half of it.
    history.addSample(1000, 0);
    QCOMPARE(history.averages(1000, &bucketMs), QList<double>({1000, 3000, 3000}));
    QCOMPARE(bucketMs, qint64(400));
    history.clear();
    history.addSample(0, 1);
    QCOMPARE(history.averages(100, &bucketMs), QList<double>({1}));
    QCOMPARE(bucketMs, qint64(100));
}

void TestRsyncCore::speedHistoryNiceScale_data()
{
    QTest::addColumn<double>("speed");
    QTest::addColumn<qint64>("scale");
    const qint64 KiB = 1024;
    const qint64 MiB = 1024 * KiB;
    const qint64 GiB = 1024 * MiB;
    QTest::newRow("0") << 0.0 << qint64(1);
    QTest::newRow("1.2 B") << 1.2 << qint64(2);
    QTest::newRow("3 B") << 3.0 << qint64(3);
    QTest::newRow("14 B") << 14.0 << qint64(15);
    QTest::newRow("999 B") << 999.0 << KiB;
    QTest::newRow("1 KiB") << double(KiB) << KiB;
    QTest::newRow("1.2 KiB") << 1.2 * double(KiB) << KiB * 3 / 2;
    QTest::newRow("700 KiB") << double(700 * KiB) << 800 * KiB;
    QTest::newRow("900 KiB") << double(900 * KiB) << MiB;
    QTest::newRow("1 MiB") << double(MiB) << MiB;
    QTest::newRow("37 MiB") << double(37 * MiB) << 40 * MiB;
    QTest::newRow("101 MiB") << double(101 * MiB) << 150 * MiB;
    QTest::newRow("600 MiB") << double(600 * MiB) << 600 * MiB;
    QTest::newRow("1.5 GiB") << 1.5 * double(GiB) << GiB * 3 / 2;
    QTest::newRow("1.6 GiB") << 1.6 * double(GiB) << 2 * GiB;
}

void TestRsyncCore::speedHistoryNiceScale()
{
    QFETCH(double, speed);
    QFETCH(qint64, scale);
    QCOMPARE(SpeedHistory::niceScale(speed), scale);
}

void TestRsyncCore::fileListingKeepsNames()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray dir = QFile::encodeName(tmp.path());
    auto cleanup = qScopeGuard([&] {
        for (const char *name : {"caf\xe9.txt", "caf\xe8.txt", "dir\xff", "link\xfe"})
            removeTree(dir + '/' + name);
    });
    writeRaw(dir + "/caf\xe9.txt", "12345");
    writeRaw(dir + "/caf\xe8.txt", "1");
    writeRaw(dir + "/r\xc3\xa9sum\xc3\xa9.txt", "");
    QCOMPARE(::mkdir((dir + "/dir\xff").constData(), 0755), 0);
    QCOMPARE(::symlink("dir\xff", (dir + "/link\xfe").constData()), 0);

    int error = -1;
    QList<FileListing::Entry> entries = FileListing::list(LocalPath::decode(dir), &error);
    QCOMPARE(error, 0);
    std::sort(entries.begin(), entries.end(),
              [](const FileListing::Entry &a, const FileListing::Entry &b) { return LocalPath::encode(a.name) < LocalPath::encode(b.name); });
    QStringList names;
    for (const FileListing::Entry &entry : std::as_const(entries))
        names << LocalPath::display(entry.name);
    QCOMPARE(names, (QStringList{QStringLiteral("caf\\xe8.txt"), QStringLiteral("caf\\xe9.txt"), QStringLiteral("dir\\xff"), QStringLiteral("link\\xfe"), QStringLiteral("résumé.txt")}));
    // The names give the exact bytes back.
    QCOMPARE(LocalPath::encode(entries.at(1).name), QByteArray("caf\xe9.txt"));
    QCOMPARE(entries.at(1).size, 5);
    QVERIFY(!entries.at(1).isDir);
    QVERIFY(entries.at(2).isDir);
    QVERIFY(!entries.at(2).isSymLink);
    QVERIFY(entries.at(3).isDir); // a symlink to a directory
    QVERIFY(entries.at(3).isSymLink);
    QVERIFY(LocalPath::isPlain(entries.at(4).name));
    QVERIFY(entries.at(1).modified > 0);

    // A directory that is not valid UTF-8 itself, and one that does not exist.
    writeRaw(dir + "/dir\xff/inside\xfd", "x");
    entries = FileListing::list(LocalPath::decode(dir + "/dir\xff/"), &error);
    QCOMPARE(error, 0);
    QCOMPARE(entries.size(), 1);
    QCOMPARE(LocalPath::encode(entries.first().name), QByteArray("inside\xfd"));
    QVERIFY(FileListing::list(LocalPath::decode(dir + "/missing"), &error).isEmpty());
    QCOMPARE(error, ENOENT);
}

void TestRsyncCore::fileListingFromDisplay_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("plain") << QStringLiteral("/a/b c") << QByteArray("/a/b c");
    QTest::newRow("byte") << QStringLiteral("/a/caf\\xe9.txt") << QByteArray("/a/caf\xe9.txt");
    QTest::newRow("upper case") << QStringLiteral("/a/\\xFF") << QByteArray("/a/\xff");
    QTest::newRow("several") << QStringLiteral("\\xfc\\xfd-\\x80") << QByteArray("\xfc\xfd-\x80");
    QTest::newRow("valid UTF-8 as bytes") << QStringLiteral("r\\xc3\\xa9sum\\xc3\\xa9") << QByteArray("r\xc3\xa9sum\xc3\xa9");
    QTest::newRow("ASCII escape stays text") << QStringLiteral("a\\x41") << QByteArray("a\\x41");
    QTest::newRow("incomplete") << QStringLiteral("a\\xe") << QByteArray("a\\xe");
    QTest::newRow("non-ASCII text") << QStringLiteral("ü\\xe9") << QByteArray("\xc3\xbc\xe9");
}

void TestRsyncCore::fileListingFromDisplay()
{
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    const QString path = FileListing::fromDisplay(text);
    QCOMPARE(LocalPath::encode(path), bytes);
    // The same string as a directory listing gives for these bytes.
    QCOMPARE(path, LocalPath::decode(bytes));
}

void TestRsyncCore::fileListingHints()
{
    const QString name = LocalPath::decode("M\xfcnchen \xe9t\xe9 \xc3\xbc.txt");
    QCOMPARE(FileListing::invalidBytes(name), QStringLiteral("FC E9 E9"));
    QCOMPARE(FileListing::latin1Reading(name), QStringLiteral("München été ü.txt"));
    QCOMPARE(FileListing::invalidBytes(QStringLiteral("plain ü")), QString());
    QCOMPARE(FileListing::latin1Reading(QStringLiteral("plain ü")), QStringLiteral("plain ü"));
}

void TestRsyncCore::fileListingReplacedBytes()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QByteArray dir = QFile::encodeName(tmp.path());
    auto cleanup = qScopeGuard([&] { removeTree(dir + "/d"); });
    QCOMPARE(::mkdir((dir + "/d").constData(), 0755), 0);
    const QByteArray d = dir + "/d";
    writeRaw(d + "/caf\xe9.txt", "");
    writeRaw(d + "/caf\xe8.txt", "");
    writeRaw(d + "/caf.txt", "");
    writeRaw(d + "/na\xefve.txt", "");
    writeRaw(d + "/two\xe9\xfc.txt", "");
    writeRaw(d + "/real\xef\xbf\xbd.txt", ""); // U+FFFD in the name itself (valid UTF-8)
    writeRaw(d + "/dot.\xe9", "");
    QCOMPARE(::mkdir((d + "/sub\xfc").constData(), 0755), 0);
    writeRaw(d + "/sub\xfc/x\xff", "");
    writeRaw(d + "/sub\xfd", ""); // a file: not a directory in the middle of the path

    // As Dolphin passes them: U+FFFD (EF BF BD) in place of the bytes.
    const auto passed = [&](const QByteArray &name) { return LocalPath::decode(d + '/' + name); };
    const auto exact = [&](const QList<QByteArray> &names) {
        QStringList paths;
        for (const QByteArray &name : names)
            paths << LocalPath::decode(d + '/' + name);
        paths.sort();
        return paths;
    };
    QCOMPARE(FileListing::replacedBytesMatches(passed("na\xef\xbf\xbdve.txt")), exact({"na\xefve.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf\xef\xbf\xbd.txt")), exact({"caf\xe9.txt", "caf\xe8.txt"}));
    // One U+FFFD can stand for several bytes (depending on the decoder), and several for several.
    QCOMPARE(FileListing::replacedBytesMatches(passed("two\xef\xbf\xbd.txt")), exact({"two\xe9\xfc.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("two\xef\xbf\xbd\xef\xbf\xbd.txt")), exact({"two\xe9\xfc.txt"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("real\xef\xbf\xbd.txt")), exact({"real\xef\xbf\xbd.txt"}));
    // The rest of the name is matched literally ("." is not a pattern).
    QCOMPARE(FileListing::replacedBytesMatches(passed("dot\xef\xbf\xbd\xef\xbf\xbd")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("dot.\xef\xbf\xbd")), exact({"dot.\xe9"}));
    // In a directory of the path too.
    QCOMPARE(FileListing::replacedBytesMatches(passed("sub\xef\xbf\xbd/x\xef\xbf\xbd")), exact({"sub\xfc/x\xff"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("sub\xef\xbf\xbd")), exact({"sub\xfc", "sub\xfd"}));
    QCOMPARE(FileListing::replacedBytesMatches(passed("nothing\xef\xbf\xbd")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf.txt")), QStringList()); // no U+FFFD
    QCOMPARE(FileListing::replacedBytesMatches(QStringLiteral("relative/caf�.txt")), QStringList());
    QCOMPARE(FileListing::replacedBytesMatches(passed("caf\xef\xbf\xbd.txt"), 1).size(), 1);
}

void TestRsyncCore::fileListingMountPoints()
{
    const QByteArray mounts = "/dev/sda1 / ext4 rw,relatime 0 0\n"
                              "proc /proc proc rw 0 0\n"
                              "/dev/sdb1 /media/user/OLD\\040DISK vfat rw 0 0\n"
                              "/dev/sdc1 /media/user/caf\\351 vfat rw 0 0\n"
                              "/dev/sdd1 /mnt/back\\134slash ext4 rw 0 0\n"
                              "\n";
    const QStringList points = FileListing::mountPoints(mounts);
    QCOMPARE(points.size(), 5);
    QCOMPARE(points.at(0), QStringLiteral("/"));
    QCOMPARE(points.at(2), QStringLiteral("/media/user/OLD DISK"));
    QCOMPARE(LocalPath::encode(points.at(3)), QByteArray("/media/user/caf\xe9"));
    QCOMPARE(points.at(4), QStringLiteral("/mnt/back\\slash"));
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

void TestRsyncCore::remoteOnServerCopy()
{
    RemoteServer server(QStringLiteral("onserver"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    // A name with a quote, a space, a byte that is not valid UTF-8 (Latin-1 "é"), a subdirectory and a symlink.
    server.run(QStringLiteral("cd '%1' && mkdir -p \"src it's/sub dir\" && cd \"src it's\" && printf plain > plain.txt"
                              " && printf latin > \"$(printf 'caf\\351').txt\" && printf deep > 'sub dir/deep'"
                              " && ln -s plain.txt link").arg(server.dir));

    // Copy and verify on the server.
    RsyncOptions options = server.options();
    options.checksum = true;
    options.verifyAfter = true;
    RsyncJob::Plan plan = RsyncJob::plan({server.at(QStringLiteral("src it's"))}, server.at(QStringLiteral("dest")),
                                         options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.size(), 1);
    QVERIFY(plan.transfers.first().runsOnServer());
    RsyncJob copy(plan.transfers, options);
    QStringList log;
    connect(&copy, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    QVERIFY(runJob(copy));
    QVERIFY2(copy.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(copy.summary().verified, 1);
    // The PID line itself is not logged (only the command that prints it).
    QVERIFY(!log.join('\n').contains(QRegularExpression(QStringLiteral("RSYNC-GUI-PID [0-9]"))));
    const QString listCommand = QStringLiteral("cd \"%1/dest/src it's\" && find . | LC_ALL=C sort && cat plain.txt"
                                               " \"$(printf 'caf\\351').txt\" 'sub dir/deep' && readlink link");
    QCOMPARE(server.run(listCommand.arg(server.dir)),
             QByteArray(".\n./caf\xe9.txt\n./link\n./plain.txt\n./sub dir\n./sub dir/deep\nplainlatindeepplain.txt\n"));

    // A verified move within the server: the sources are removed after the verification.
    options.removeSourceFiles = true;
    plan = RsyncJob::plan({server.at(QStringLiteral("src it's/"))}, server.at(QStringLiteral("moved")), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    RsyncJob move(plan.transfers, options);
    QVERIFY(runJob(move));
    QVERIFY(move.summary().ok());
    QCOMPARE(move.summary().sourcesRemoved, 1);
    QCOMPARE(server.run(QStringLiteral("find \"%1/src it's\" -type f -o -type l | wc -l").arg(server.dir)).trimmed(),
             QByteArray("0"));
    QCOMPARE(server.run(QStringLiteral("cat '%1/moved/plain.txt' '%1/moved/sub dir/deep'").arg(server.dir)),
             QByteArray("plaindeep"));
}

void TestRsyncCore::remoteOnServerCancel()
{
    RemoteServer server(QStringLiteral("onservercancel"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    server.run(QStringLiteral("mkdir -p '%1/src' && head -c 33554432 /dev/urandom > '%1/src/big.bin'").arg(server.dir));

    // No progress output: rsync on the server never writes, so it would not notice the closed connection -
    // it must be stopped by its PID.
    RsyncOptions options = server.options();
    options.useBwLimit = true;
    options.bwLimit = QStringLiteral("1M"); // 32 s for the whole file
    const RsyncJob::Plan plan = RsyncJob::plan({server.at(QStringLiteral("src"))}, server.at(QStringLiteral("dest")),
                                               options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.transfers.first().runsOnServer());
    RsyncJob job(plan.transfers, options);
    QSignalSpy finished(&job, &RsyncJob::finished);
    job.start();
    QTest::qWait(2000);
    QVERIFY(job.isRunning());
    QVERIFY(!server.run(QStringLiteral("pgrep -x rsync || true")).trimmed().isEmpty());
    QElapsedTimer timer;
    timer.start();
    job.cancel();
    QVERIFY(finished.count() > 0 || finished.wait(15000));
    // Ended by the remote kill, before the local ssh would be terminated (5 s).
    QVERIFY2(timer.elapsed() < 4500, qPrintable(QString::number(timer.elapsed())));
    QVERIFY(job.summary().cancelled);
    QCOMPARE(server.run(QStringLiteral("pgrep -x rsync || true")).trimmed(), QByteArray());
}

void TestRsyncCore::remoteOnServerSftpOnly()
{
    // An account that allows sftp only runs no commands: the failure says so, not "syntax or usage error".
    const Location home = Location::parse(qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SFTP_ONLY"));
    const QString shell = qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SHELL");
    if (!home.isRemote() || shell.isEmpty())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    RsyncOptions options = testOptions();
    options.useRemoteShell = true;
    options.remoteShell = shell;
    Location source = home;
    source.path += QStringLiteral("/src");
    Location dest = home;
    dest.path += QStringLiteral("/dest");
    const RsyncJob::Plan plan = RsyncJob::plan({source}, dest, options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.transfers.first().runsOnServer());
    RsyncJob job(plan.transfers, options);
    QStringList log;
    connect(&job, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    QVERIFY(runJob(job));
    QCOMPARE(job.summary().transfersFailed, 1);
    const QString text = log.join('\n');
    QVERIFY2(text.contains("This service allows sftp connections only."), qPrintable(text));
    QVERIFY2(text.contains("the server did not run the command"), qPrintable(text));
    QVERIFY2(!text.contains("syntax or usage error"), qPrintable(text));
}

namespace {

RemoteProbe::Result runProbe(const RsyncOptions &options, const Location &home, const QStringList &sources,
                             const QString &destination)
{
    RemoteProbe probe(options, home.user.isEmpty() ? home.host : home.user + QLatin1Char('@') + home.host, home.port,
                      sources, destination);
    QSignalSpy spy(&probe, &RemoteProbe::finished);
    probe.start();
    if (spy.isEmpty() && !spy.wait(30000))
        return {};
    return spy.first().first().value<RemoteProbe::Result>();
}

} // namespace

void TestRsyncCore::remoteProbe()
{
    RemoteServer server(QStringLiteral("probe"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    server.run(QStringLiteral("mkdir -p '%1/src dir' && printf x > '%1/file'").arg(server.dir));
    const QStringList sources{server.dir + "/src dir", server.dir + "/file", server.dir + "/missing"};
    RemoteProbe::Result r = runProbe(server.options(), server.home, sources, server.dir + "/new/dest");
    QVERIFY2(r.ran, qPrintable(r.output));
    QCOMPARE(r.error(), QString());
    QVERIFY(r.rsyncVersion.startsWith("rsync"));
    QCOMPARE(r.sources.at(0).type, RemoteProbe::Type::Directory);
    QCOMPARE(r.sources.at(1).type, RemoteProbe::Type::File);
    QCOMPARE(r.missingSources(), QStringList{server.dir + "/missing"});
    QCOMPARE(r.destination.type, RemoteProbe::Type::Missing);
    QCOMPARE(r.existingParent, server.dir);
    QVERIFY(r.parentWritable);
    QVERIFY(r.freeBytes > 0);
    QCOMPARE(r.destinationInsideSource(), QString());
    r = runProbe(server.options(), server.home, sources, server.dir + "/src dir/inside");
    QCOMPARE(r.destinationInsideSource(), server.dir + "/src dir");
    // The size for the graph over ssh.
    QVERIFY(RemoteProbe::parseSize(server.run(RemoteProbe::sizeCommand(sources))) > 0);

    // An account for sftp only: the probe says that rsync cannot run there.
    const Location sftpOnly = Location::parse(qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SFTP_ONLY"));
    if (sftpOnly.isRemote()) {
        r = runProbe(server.options(), sftpOnly, {sftpOnly.path}, sftpOnly.path + "/dest");
        QVERIFY(!r.ran);
        QVERIFY2(r.error().contains("sftp only") && r.error().contains("This service allows sftp connections only."),
                 qPrintable(r.error()));
    }
}

void TestRsyncCore::remoteOnServerMoveRemovesDirs()
{
    RemoteServer server(QStringLiteral("movedirs"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    server.run(QStringLiteral("cd '%1' && mkdir -p 'src/sub/deep' src/empty && printf f > src/sub/deep/f").arg(server.dir));
    RsyncOptions options = server.options();
    options.removeSourceFiles = true;
    options.removeSourceDirs = true;
    RsyncJob::Plan plan = RsyncJob::plan({server.at("src")}, server.at("new/dest"), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    plan.transfers.first().createDestination = true; // as the window does after asking (the destination is missing)
    RsyncJob job(plan.transfers, options);
    QStringList log;
    connect(&job, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    QVERIFY(runJob(job));
    QVERIFY2(job.summary().ok(), qPrintable(log.join('\n')));
    QCOMPARE(job.summary().sourceDirsRemoved, 4); // src, src/sub, src/sub/deep, src/empty
    QCOMPARE(server.run(QStringLiteral("test -e '%1/src' && echo exists || echo gone").arg(server.dir)), QByteArray("gone\n"));
    QCOMPARE(server.run(QStringLiteral("cat '%1/new/dest/src/sub/deep/f'").arg(server.dir)), QByteArray("f"));
}

void TestRsyncCore::remoteSharedConnection()
{
    RemoteServer server(QStringLiteral("shared"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    server.run(QStringLiteral("mkdir -p '%1/src' && printf a > '%1/src/a'").arg(server.dir));
    QTemporaryDir sockets(QDir::tempPath() + "/rgcm-XXXXXX"); // short: socket paths are limited to 108 bytes
    QVERIFY(sockets.isValid());
    RsyncOptions options = server.options();
    options.sshControlPath = sockets.path() + "/%C";
    options.checksum = true;
    options.verifyAfter = true;
    // All runs (copy and verification on the server) through one connection; the master stays afterwards.
    const RsyncJob::Plan plan = RsyncJob::plan({server.at("src")}, server.at("dest"), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    RsyncJob job(plan.transfers, options);
    QElapsedTimer timer;
    timer.start();
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(job.summary().verified, 1);
    QVERIFY2(timer.elapsed() < 20000, "a background ssh master must not keep the job waiting");
    // A local -> remote copy (rsync's own -e) uses it too.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    writeFile(tmp.filePath("up/b"), "b");
    const RsyncJob::Plan up = RsyncJob::plan({Location::parse(tmp.filePath("up"))}, server.at("dest"), options);
    RsyncJob upload(up.transfers, options);
    QVERIFY(runJob(upload));
    QVERIFY(upload.summary().ok());
    const QStringList entries = QDir(sockets.path()).entryList(QDir::System | QDir::NoDotAndDotDot);
    QCOMPARE(entries.size(), 1); // one master socket for the server
    // Stop the master.
    QStringList ssh = options.sshCommand(server.home.port);
    const QString program = ssh.takeFirst();
    QProcess stop;
    stop.start(program, ssh << "-O" << "exit" << "--" << server.home.user + "@" + server.home.host);
    QVERIFY(stop.waitForFinished(10000));
}

void TestRsyncCore::remoteSshfsShare()
{
    // A real sshfs mount of the test server: a copy between two of its directories runs on the server.
    RemoteServer server(QStringLiteral("sshfs"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    if (QStandardPaths::findExecutable("sshfs").isEmpty() || QStandardPaths::findExecutable("fusermount3").isEmpty())
        QSKIP("sshfs is not installed");
    server.run(QStringLiteral("mkdir -p '%1/share/src' && printf data > '%1/share/src/f'").arg(server.dir));
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString mountPoint = tmp.filePath("mnt");
    QVERIFY(QDir().mkpath(mountPoint));
    const QString target = server.home.user + "@" + server.home.host;
    QProcess sshfs;
    sshfs.start("sshfs", {"-o", "ssh_command=" + server.shell, "-p", QString::number(server.home.port),
                          target + ":" + server.dir + "/share", mountPoint});
    QVERIFY(sshfs.waitForFinished(20000));
    if (sshfs.exitCode() != 0)
        QSKIP(qPrintable("sshfs cannot mount here: " + QString::fromLocal8Bit(sshfs.readAllStandardError())));
    const auto unmount = qScopeGuard([&] { QProcess::execute("fusermount3", {"-u", mountPoint}); });
    QVERIFY(QFileInfo::exists(mountPoint + "/src/f"));

    const QList<Mount> mounts = Mount::current();
    QString rel;
    const std::optional<NetworkShare> share = NetworkShare::of(mounts, mountPoint + "/src", &rel);
    QVERIFY(share);
    QCOMPARE(share->kind, NetworkShare::Kind::Sshfs);
    QCOMPARE(share->remotePath, server.dir + "/share");
    QCOMPARE(rel, QStringLiteral("src"));
    ShareSettings settings = ShareSettings::detected(*share);
    QCOMPARE(settings.sshTarget, target);
    settings.port = server.home.port; // not in the mount table
    settings.use = ShareSettings::Use::Enabled;
    const ShareRouting routing = ShareRouting::route({Location::parse(mountPoint + "/src")},
                                                     Location::parse(mountPoint + "/dest"), mounts,
                                                     [&](const QString &) { return settings; }, true);
    RsyncOptions options = server.options();
    const RsyncJob::Plan plan = RsyncJob::plan(routing.sources, routing.destination, options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QCOMPARE(plan.transfers.size(), 1);
    QVERIFY(plan.transfers.first().runsOnServer());
    QCOMPARE(plan.transfers.first().destination, server.dir + "/share/dest/");
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(server.run(QStringLiteral("cat '%1/share/dest/src/f'").arg(server.dir)), QByteArray("data"));
    QCOMPARE(readFile(mountPoint + "/dest/src/f"), QByteArray("data")); // and seen through the mount
}

void TestRsyncCore::remoteSambaDetect()
{
    // Detect in the share dialog: the path and the modes of a Samba share read with testparm over ssh.
    RemoteServer server(QStringLiteral("samba"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QCOMPARE(server.run(NetworkShare::sharePathCommand("testshare")), QByteArray("/home/tester/samba\n"));
    const QMap<QString, QString> parameters = NetworkShare::parseParameters(server.run(NetworkShare::ownershipCommand("testshare")));
    QCOMPARE(parameters.value("force user"), QStringLiteral("tester"));
    QCOMPARE(parameters.value("create mask"), QStringLiteral("0640"));
    const QString chmod = NetworkShare::chmodFromSamba(parameters);
    QCOMPARE(chmod, QStringLiteral("Fu-x,Fg-wx,Fo-rwx,Fo+r,Dg-w,Do-rwx"));

    // A copy on the server with it: the modes Samba would give.
    server.run(QStringLiteral("mkdir -p '%1/src/d' && printf f > '%1/src/d/f' && chmod 777 '%1/src/d' '%1/src/d/f'").arg(server.dir));
    RsyncOptions options = server.options();
    RsyncJob::Plan plan = RsyncJob::plan({server.at("src/d")}, server.at("dest"), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    plan.transfers.first().extraArguments << "--chmod=" + chmod;
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(server.run(QStringLiteral("stat -c %a '%1/dest/d' '%1/dest/d/f'").arg(server.dir)), QByteArray("750\n644\n"));
}

void TestRsyncCore::remoteFromSftpOnly()
{
    // A copy from an account for sftp only: the probe finds it before the start, and if rsync runs anyway, its
    // "protocol version mismatch -- is your shell clean?" is explained.
    const Location home = Location::parse(qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SFTP_ONLY"));
    const QString shell = qEnvironmentVariable("RSYNC_GUI_TEST_SSH_SHELL");
    if (!home.isRemote() || shell.isEmpty())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    RsyncOptions options = testOptions();
    options.useRemoteShell = true;
    options.remoteShell = shell;
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    Location source = home;
    source.path += QStringLiteral("/x");
    const RsyncJob::Plan plan = RsyncJob::plan({source}, Location::parse(tmp.path()), options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    const std::optional<RemoteProbe::Target> target = RemoteProbe::targetFor(plan.transfers.first());
    QVERIFY(target);
    const RemoteProbe::Result r = runProbe(options, home, target->sources, target->destination);
    QVERIFY(!r.ran);
    QVERIFY2(r.error().contains("sftp only"), qPrintable(r.error()));

    RsyncJob job(plan.transfers, options);
    QStringList log;
    connect(&job, &RsyncJob::log, this, [&log](RsyncJob::LogLevel, const QString &text) { log << text; });
    QVERIFY(runJob(job));
    QCOMPARE(job.summary().transfersFailed, 1);
    const QString text = log.join('\n');
    QVERIFY2(text.contains("is your shell clean?"), qPrintable(text));
    QVERIFY2(text.contains("rsync could not be started on the server"), qPrintable(text));
}

void TestRsyncCore::remoteSameServerOtherName()
{
    // The source by an alias from an ssh configuration, the destination as user@address: one server, rsync runs there.
    RemoteServer server(QStringLiteral("othername"));
    if (!server.isValid())
        QSKIP("No test ssh server - run tools/ssh-test/run.sh");
    QStringList shell;
    splitArguments(server.shell, &shell);
    QString key, knownHosts;
    for (qsizetype i = 0; i + 1 < shell.size(); ++i) {
        if (shell.at(i) == "-i")
            key = shell.at(i + 1);
        if (shell.at(i + 1).startsWith("UserKnownHostsFile="))
            knownHosts = shell.at(i + 1).mid(19);
    }
    QVERIFY(!key.isEmpty() && !knownHosts.isEmpty());
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString config = tmp.filePath("config");
    writeFile(config, QStringLiteral("Host testbox\n    HostName %1\n    Port %2\n    User %3\n    IdentityFile %4\n"
                                     "    IdentitiesOnly yes\n    BatchMode yes\n    UserKnownHostsFile %5\n"
                                     "    StrictHostKeyChecking accept-new\n")
                          .arg(server.home.host).arg(server.home.port).arg(server.home.user, key, knownHosts).toUtf8());
    server.run(QStringLiteral("mkdir -p '%1/src' && printf x > '%1/src/x'").arg(server.dir));
    RsyncOptions options = testOptions();
    options.useRemoteShell = true;
    options.remoteShell = "ssh -F " + config;
    QList<Location> sources{Location::parse("testbox:" + server.dir + "/src")};
    Location dest = server.at("dest"); // tester@127.0.0.1, port
    const auto resolver = [&options](const Location &loc) {
        return SshHosts::resolve(options.sshCommand(0), loc.user.isEmpty() ? loc.host : loc.user + "@" + loc.host, loc.port);
    };
    QCOMPARE(SshHosts::unify(sources, dest, resolver).size(), 1);
    QCOMPARE(dest.host, QStringLiteral("testbox"));
    const RsyncJob::Plan plan = RsyncJob::plan(sources, dest, options);
    QVERIFY2(plan.ok(), qPrintable(plan.error));
    QVERIFY(plan.transfers.first().runsOnServer());
    RsyncJob job(plan.transfers, options);
    QVERIFY(runJob(job));
    QVERIFY(job.summary().ok());
    QCOMPARE(server.run(QStringLiteral("cat '%1/dest/src/x'").arg(server.dir)), QByteArray("x"));
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
