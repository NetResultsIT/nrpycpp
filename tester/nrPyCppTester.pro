QT -= gui

# Qt 6.8's qyieldcpu.h calls __yield() without declaring it, which recent Apple clang (Xcode 26) rejects
macos: QMAKE_CXXFLAGS += -Wno-error=implicit-function-declaration

SOURCES = main.cpp \
    pycpptester.cpp

LIBS = -L$$PWD/../lib/last_build

INCLUDEPATH = $$PWD/../lib/last_build/include

CONFIG(debug, debug|release) {
    win32: LIBSUFFIX = "d"
    macos: LIBSUFFIX = "_debug"
    linux: LIBSUFFIX = "_d"
}
LIBS += -lnrPyCpp$${LIBSUFFIX}

HEADERS += \
    pycpptester.h
