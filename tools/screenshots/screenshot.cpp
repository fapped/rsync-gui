// Takes the README screenshots from inside the real application (its own main() runs): a startup
// function drives the window with timers and saves QWidget::grab() of single widgets, so nothing
// else on the screen ends up in the images. Started by tools/make-screenshots.sh.

#include "mainwindow.h"

#include <QApplication>
#include <QGroupBox>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>

#include <cstdio>
#include <functional>

namespace {

MainWindow *window()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *main = qobject_cast<MainWindow *>(widget))
            return main;
    }
    return nullptr;
}

QPushButton *button(QWidget *parent, const QString &text)
{
    for (QPushButton *b : parent->findChildren<QPushButton *>()) {
        if (b->text() == text)
            return b;
    }
    return nullptr;
}

void save(QWidget *widget, const QString &name)
{
    const QString file = qEnvironmentVariable("SCREENSHOT_DIR") + QLatin1Char('/') + name;
    const bool ok = widget->grab().save(file);
    std::printf("%s %s\n", ok ? "saved" : "FAILED", qPrintable(file));
}

void after(int ms, std::function<void()> step)
{
    QTimer::singleShot(ms, qApp, std::move(step));
}

// Calls `then` once the transfer has finished (the Copy/Move button is enabled again).
void whenFinished(QPushButton *start, std::function<void()> then)
{
    if (start->isEnabled()) {
        after(500, std::move(then));
        return;
    }
    after(200, [start, then] { whenFinished(start, then); });
}

// Saves the first visible message box, then answers it with the button that has `role`.
void catchMessageBox(const QString &name, QMessageBox::ButtonRole role)
{
    after(300, [name, role] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            auto *box = qobject_cast<QMessageBox *>(widget);
            if (!box || !box->isVisible())
                continue;
            save(box, name);
            for (QAbstractButton *b : box->buttons()) {
                if (box->buttonRole(b) == role) {
                    b->click();
                    return;
                }
            }
            box->reject();
            return;
        }
        catchMessageBox(name, role); // not shown yet
    });
}

void run()
{
    MainWindow *w = window();
    if (!w) {
        std::fprintf(stderr, "no main window\n");
        QApplication::exit(1);
        return;
    }
    // With Plasma's global menu the menu bar is an empty strip in the window - leave it out
    // (File > Load preset has its own screenshot).
    w->menuBar()->hide();
    // The log starts with "Loaded ... from <path of the preset>" - not needed in the images.
    w->findChild<QPlainTextEdit *>()->clear();
    w->resize(920, 880);

    // 1. The window with the "Copy and verify" preset, sources and the destination (from the command line).
    after(800, [w] {
        save(w, QStringLiteral("rsync-gui-main.png"));
        // 2. All options (the group box, also the part scrolled out of view).
        for (QGroupBox *box : w->findChildren<QGroupBox *>()) {
            if (box->title().startsWith(QStringLiteral("rsync options")))
                save(box, QStringLiteral("rsync-gui-options.png"));
        }
        // 3. File > Load preset.
        for (QMenu *menu : w->findChildren<QMenu *>()) {
            if (menu->title() == QStringLiteral("Load &preset")) {
                QMetaObject::invokeMethod(menu, "aboutToShow");
                menu->popup(w->mapToGlobal(QPoint(40, 40)));
                after(400, [menu] {
                    save(menu, QStringLiteral("rsync-gui-presets.png"));
                    menu->hide();
                });
            }
        }
    });

    // 4. Copy with verification, the log afterwards.
    after(1800, [w] {
        QPushButton *start = button(w, QStringLiteral("Copy"));
        start->click();
        whenFinished(start, [w] {
            save(w, QStringLiteral("rsync-gui-finished.png"));
            // 5. The confirmation of "Move and verify" (cancelled - nothing is moved).
            w->loadPreset(QStringLiteral("rsync-move-verify"));
            after(300, [w] {
                catchMessageBox(QStringLiteral("rsync-gui-move-confirm.png"), QMessageBox::RejectRole);
                button(w, QStringLiteral("Move"))->click();
                after(800, [] { QApplication::quit(); });
            });
        });
    });
}

void start()
{
    after(0, run); // after main() has created and filled the window
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(start)
