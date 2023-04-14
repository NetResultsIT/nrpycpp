
NRPYCPP_VERSION = 1.0.2

#DO NOT CHANGE ANYTHING BELOW THIS LINE UNLESS YOU ARE A MAINTAINER

!include (depspath.pri) {
    error( "nrPyCpp: Cannot find depspath.pri" )
}

QT -= gui
CONFIG += c++11 debug_and_release
CONFIG -= app_bundle

TEMPLATE = lib
win32 {
    CONFIG += console

    QMAKE_CXXFLAGS_RELEASE += /Zi
    QMAKE_LFLAGS_RELEASE += /DEBUG
    QMAKE_LFLAGS_RELEASE += /OPT:REF
    QMAKE_LFLAGS_RELEASE += /OPT:ICF

    DEFINES += NRPYQT_LIB_EXPORTS
}

macos: CONFIG += dll #to build a framework

message("Output dir: $$TARGET")

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        pyenvironment.cpp \
        pyrunner.cpp \
        PyRunnerQt.cpp


INCLUDEPATH += $${PYTHON_INCLUDE_PATH}
LIBS += -L$${PYTHON_LIB_PATH} -lpython$${PYTHON_VERSION}
message("nrPyCpp linking python lib version: $${PYTHON_VERSION}")
message("nrPyCpp LIBS = $${LIBS}")


HEADERS += \
    PyCall.h \
    pyenvironment.h \
    pyrunner.h \
    PyRunnerQt.h \
    sleep_header.h

DISTFILES += \
    PyQAC.py \
    PyQAC2.py \
    PyTest.py

# includes to be packaged
DIST_INCS += \
$$PWD/PyCall.h \
$$PWD/PyRunnerQt.h

CONFIG(debug, debug|release) {
    win32: LIBSUFFIX = "d"
    macos: LIBSUFFIX = "_debug"
    linux: LIBSUFFIX = "_d"
}


# NR rules for deployment.
FINALDIR = $${QMAKE_CXX}_qt-$${QT_VERSION}
CONFIG(debug, debug|release) {
    message("nrPyCpp Debug build")
    BUILDTYPE = debug
    DLLPATH = "/debug/bin/"
    DLLPATH = $$join(DLLPATH,,$$OUT_PWD,)
    FINALDIR = $$join(FINALDIR,,"$$PWD/debug/","/")
}

CONFIG(release, debug|release) {
    message("nrPyCpp Release build")
    BUILDTYPE = release
    DLLPATH = "/release/bin/"
    DLLPATH = $$join(DLLPATH,,$${OUT_PWD},)
    FINALDIR = $$join(FINALDIR,,"$$PWD/release/","/")
}


DSTDIR=$$PWD/last_build

#we first get a full fledged dll filename (w/o dot extension)
win32 {
    DLL = $$join(TARGET,,$$DLLPATH/,$$LIBSUFFIX)
} else {
    DLL = $$join(TARGET,,$${DLLPATH}/lib,$$LIBSUFFIX)
}
#add libsuffix to distinguish debug builds
TARGET = $$join(TARGET,,,$$LIBSUFFIX)
#then we add bin to the target to have it generated in the bin/ subfolder (compiler quirks...)
TARGET = $$join(TARGET,,bin/,)
#gcc compiler quirks
!win32 {
    TARGET = $$join(TARGET,,$$BUILDTYPE/,)
}


# And at last common unix library movements
INCLUDE_DIR = $$DSTDIR/include

#on mac we need to adjust the libraries id and copy the pythonlib to be distributed
macos: {
    PYTHONLIB=libpython$${PYTHON_VERSION}.dylib
    QMAKE_POST_LINK+="cp $${PYTHON_LIB_PATH}/$${PYTHONLIB} $$DSTDIR $$escape_expand(\\n\\t)"
    QMAKE_POST_LINK+="install_name_tool -id @rpath/$${PYTHONLIB} $$DSTDIR/$${PYTHONLIB} $$escape_expand(\\n\\t)"
    QMAKE_POST_LINK+="install_name_tool -change @rpath/Python3.framework/Versions/$${PYTHON_VERSION}/Python3 @rpath/$${PYTHONLIB} $${DLL}.dylib $$escape_expand(\\n\\t)"
}


unix:!ios {
    #QMAKE_POST_LINK+="mkdir -p $$FINALDIR $$escape_expand(\\n\\t)"
    QMAKE_POST_LINK+="mkdir -p $$INCLUDE_DIR $$escape_expand(\\n\\t)"
    #QMAKE_POST_LINK+="cp -aP $${DLL}.* $$FINALDIR $$escape_expand(\\n\\t)"
    QMAKE_POST_LINK+="cp -aP $${DLL}.* $$DSTDIR $$escape_expand(\\n\\t)"
    for(vinc, DIST_INCS):QMAKE_POST_LINK+="$$QMAKE_COPY $$vinc \"$$INCLUDE_DIR\" $$escape_expand(\\n\\t)"
}

WINEXT = lib pdb dll
win32 {
    DLL = $$replace(DLL,"/","\\")
    DSTDIR = $$replace(DSTDIR,"/","\\")
    #FINALDIR = $$replace(FINALDIR,"/","\\")
    INCLUDE_DIR = $$replace(INCLUDE_DIR,"/","\\")
    QMAKE_POST_LINK+="$$QMAKE_CHK_DIR_EXISTS \"$$INCLUDE_DIR\" $$QMAKE_MKDIR \"$$INCLUDE_DIR\" $$escape_expand(\\n\\t)"
    #for(ext, WINEXT):QMAKE_POST_LINK+="$$QMAKE_COPY $$join(DLL,,,.$${ext}) \"$$FINALDIR\" $$escape_expand(\\n\\t)"
    for(ext, WINEXT):QMAKE_POST_LINK+="$$QMAKE_COPY $$join(DLL,,,.$${ext}) \"$$DSTDIR\" $$escape_expand(\\n\\t)"
    for(vinc, DIST_INCS):QMAKE_POST_LINK+="$$QMAKE_COPY $$replace(vinc,"/","\\") \"$$INCLUDE_DIR\" $$escape_expand(\\n\\t)"
    #copy the python lib
    PYTHONLIB=$$PWD\python-win32\python$${PYTHON_VERSION}.dll
    QMAKE_POST_LINK+="$$QMAKE_COPY $$replace(PYTHONLIB,"/","\\") \"$$DSTDIR\" $$escape_expand(\\n\\t)"
}


