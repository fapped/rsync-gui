cmake_minimum_required(VERSION 3.19)

project(rsync-gui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets DBus Concurrent)
qt_standard_project_setup()

include(CTest)
include(GNUInstallDirs)

# GUI-independent logic (options, locations, running rsync) - used by the application and the tests.
add_library(rsynccore STATIC
    src/localpath.h src/localpath.cpp
    src/location.h src/location.cpp
    src/rsyncoptions.h src/rsyncoptions.cpp
    src/rsyncjob.h src/rsyncjob.cpp
    src/remoteprobe.h src/remoteprobe.cpp
    src/networkshares.h src/networkshares.cpp
    src/sshhosts.h src/sshhosts.cpp
    src/speedhistory.h src/speedhistory.cpp
    src/filelisting.h src/filelisting.cpp
    src/runlog.h src/runlog.cpp
)
target_include_directories(rsynccore PUBLIC src)
target_link_libraries(rsynccore PUBLIC Qt6::Core)
target_compile_definitions(rsynccore PUBLIC RSYNC_GUI_VERSION="${PROJECT_VERSION}")

qt_add_executable(rsync-gui
    src/main.cpp
    src/mainwindow.h src/mainwindow.cpp
    src/desktopprogress.h src/desktopprogress.cpp
    src/speedgraph.h src/speedgraph.cpp
    src/filedialog.h src/filedialog.cpp
    src/elidedlabel.h src/elidedlabel.cpp
    src/sharedialog.h src/sharedialog.cpp
)
target_link_libraries(rsync-gui PRIVATE rsynccore Qt6::Widgets Qt6::DBus Qt6::Concurrent)

option(RSYNC_GUI_INSTALL_SERVICEMENUS "Install the Dolphin (KDE) context menu entries" ON)

# Presets shipped with the application. They are also copied next to the executable in the build
# directory, so that `./build/rsync-gui --preset <name>` works without installing.
set(RSYNC_GUI_PRESETS
    presets/rsync-copy.json
    presets/rsync-copy-verify.json
    presets/rsync-copy-portable.json
    presets/rsync-move.json
    presets/rsync-move-verify.json
    presets/rsync-move-portable.json
)
foreach(preset IN LISTS RSYNC_GUI_PRESETS)
    configure_file(${preset} ${CMAKE_CURRENT_BINARY_DIR}/${preset} COPYONLY)
endforeach()

install(TARGETS rsync-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The .desktop files get the absolute path of the installed executable, so that Dolphin and the
# launcher find it even when <prefix>/bin is not in their PATH (e.g. ~/.local/bin before re-login).
# They are generated at install time, because `cmake --install --prefix` may change the prefix.
set(RSYNC_GUI_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/packaging)
install(CODE "
    set(RSYNC_GUI_EXECUTABLE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/rsync-gui\")
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/packaging/rsync-gui.desktop.in\"
                   \"${RSYNC_GUI_GENERATED_DIR}/rsync-gui.desktop\" @ONLY)
    configure_file(\"${CMAKE_CURRENT_SOURCE_DIR}/packaging/servicemenus/rsync-gui.desktop.in\"
                   \"${RSYNC_GUI_GENERATED_DIR}/servicemenus/rsync-gui.desktop\" @ONLY)
")
install(FILES ${RSYNC_GUI_GENERATED_DIR}/rsync-gui.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${RSYNC_GUI_PRESETS} DESTINATION ${CMAKE_INSTALL_DATADIR}/rsync-gui/presets)
if(RSYNC_GUI_INSTALL_SERVICEMENUS)
    # KF6 ignores service menus without the executable bit in user directories.
    install(FILES
        ${RSYNC_GUI_GENERATED_DIR}/servicemenus/rsync-gui.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/kio/servicemenus
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
    )
endif()

# Debian package: tools/make-deb.sh (cpack -G DEB in a Release build on a file system with Unix permissions,
# see README, "Debian package"). The package installs to /usr; the .desktop files then point to /usr/bin/rsync-gui. The library dependencies (Qt) are
# found by dpkg-shlibdeps.
set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME rsync-gui)
set(CPACK_PACKAGE_VENDOR fapped)
set(CPACK_PACKAGE_CONTACT "fapped <5470801+fapped@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Qt GUI for rsync: copy, verify and move files, with a Dolphin context menu")
string(CONCAT CPACK_DEBIAN_PACKAGE_DESCRIPTION
    "A graphical front end for rsync: sources and a destination (local, sftp:// or host:path),\n"
    "the common options, verification after copying, moves, progress with a speed graph,\n"
    "Plasma task manager and notification progress, presets and a Dolphin context menu.\n"
    "Copies within one server and to network shares set up for it run over ssh.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/fapped/rsync-gui")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION utils)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "rsync, openssh-client")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "ksshaskpass, qt6-wayland")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "dolphin, sshfs, samba-common-bin")
include(CPack)

if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    qt_add_executable(tst_rsynccore tests/tst_rsynccore.cpp)
    target_link_libraries(tst_rsynccore PRIVATE rsynccore Qt6::Test)
    target_compile_definitions(tst_rsynccore PRIVATE
        RSYNC_GUI_SOURCE_PRESETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/presets"
        RSYNC_GUI_SOURCE_SERVICEMENU="${CMAKE_CURRENT_SOURCE_DIR}/packaging/servicemenus/rsync-gui.desktop.in")
    add_test(NAME tst_rsynccore COMMAND tst_rsynccore)
endif()
