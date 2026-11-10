# nrPyCpp test suite: build the library first (it installs into lib/last_build), then this project.
# run_tests.sh / run_tests.ps1 do both and run the tests.
QT -= gui
QT += testlib
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TARGET = tst_nrpycpp

# Qt 6.8's qyieldcpu.h calls __yield() without declaring it, which recent Apple clang (Xcode 26) rejects
macos: QMAKE_CXXFLAGS += -Wno-error=implicit-function-declaration

SOURCES = tst_nrpycpp.cpp
DISTFILES += scripts/talkback.py scripts/codec.py scripts/broken.py scripts/dep/helper.py

NRPYCPP_DIR = $$clean_path($$PWD/../lib/last_build)
INCLUDEPATH += $$NRPYCPP_DIR/include
LIBS += -L$$NRPYCPP_DIR

CONFIG(debug, debug|release) {
    win32: LIBSUFFIX = "d"
    macos: LIBSUFFIX = "_debug"
    linux: LIBSUFFIX = "_d"
}
LIBS += -lnrPyCpp$${LIBSUFFIX}
unix: QMAKE_RPATHDIR += $$NRPYCPP_DIR

DEFINES += NRPYCPP_TESTS_DIR=\\\"$$PWD\\\"
