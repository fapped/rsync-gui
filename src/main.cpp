#include "localpath.h"
#include "mainwindow.h"
#include "rsyncoptions.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QTimer>

#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("fapped"));
    QApplication::setApplicationName(QStringLiteral("rsync-gui"));
    QApplication::setApplicationDisplayName(QStringLiteral("Rsync GUI"));
    QApplication::setApplicationVersion(QStringLiteral(RSYNC_GUI_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("rsync-gui"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "Graphical front-end for copying files with rsync.\n\n"
        "Options loaded with --preset or --config apply to this session only "
        "and do not replace the remembered options."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption presetOption({QStringLiteral("p"), QStringLiteral("preset")},
                                          QStringLiteral("Load the preset <name> (see --list-presets)."),
                                          QStringLiteral("name"));
    const QCommandLineOption configOption({QStringLiteral("c"), QStringLiteral("config")},
                                          QStringLiteral("Load options from the JSON settings <file>."),
                                          QStringLiteral("file"));
    const QCommandLineOption destinationOption({QStringLiteral("d"), QStringLiteral("destination")},
                                               QStringLiteral("Set the destination directory."),
                                               QStringLiteral("dir"));
    const QCommandLineOption browseOption(QStringLiteral("browse-destination"),
                                          QStringLiteral("Open the dialog for choosing the destination at startup."));
    const QCommandLineOption listPresetsOption(QStringLiteral("list-presets"),
                                               QStringLiteral("List available presets and exit."));
    parser.addOption(presetOption);
    parser.addOption(configOption);
    parser.addOption(destinationOption);
    parser.addOption(browseOption);
    parser.addOption(listPresetsOption);
    parser.addPositionalArgument(QStringLiteral("sources"),
                                 QStringLiteral("Files or directories to copy (paths, file:// or sftp:// URLs)."),
                                 QStringLiteral("[sources...]"));
    // QCoreApplication::arguments() would replace bytes that are not valid UTF-8 in file names;
    // the raw argv (Qt's own options already removed by QApplication) is decoded losslessly.
    QStringList arguments;
    for (int i = 0; i < argc; ++i)
        arguments << LocalPath::decode(QByteArray(argv[i]));
    parser.process(arguments);

    if (parser.isSet(listPresetsOption)) {
        const QList<RsyncPreset> presets = RsyncOptions::presets();
        for (const RsyncPreset &preset : presets)
            std::printf("%s\t%s\t%s\n", qPrintable(preset.id), qPrintable(preset.name), qPrintable(preset.file));
        if (presets.isEmpty()) {
            std::fprintf(stderr, "No presets found. Searched in:\n%s\n",
                         qPrintable(RsyncOptions::presetSearchPaths().join(QLatin1Char('\n'))));
        }
        return 0;
    }

    MainWindow window;
    window.show();
    if (parser.isSet(presetOption))
        window.loadPreset(parser.value(presetOption));
    if (parser.isSet(configOption))
        window.importSettingsFile(parser.value(configOption), MainWindow::Persistence::SessionOnly);
    window.addSources(parser.positionalArguments());
    if (parser.isSet(destinationOption))
        window.setDestination(parser.value(destinationOption));
    if (parser.isSet(browseOption))
        QTimer::singleShot(0, &window, &MainWindow::browseDestination);
    return app.exec();
}
