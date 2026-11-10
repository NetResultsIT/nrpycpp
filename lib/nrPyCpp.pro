
NRPYCPP_VERSION = 2.0.1

#DO NOT CHANGE ANYTHING BELOW THIS LINE UNLESS YOU ARE A MAINTAINER

# python location: see depspath.pri.sample, or export NRPYCPP_PYTHON_ROOT (and optionally
# NRPYCPP_PYTHON_VERSION) which take precedence over depspath.pri, e.g. for CI builds
PYTHON_ROOT = $$(NRPYCPP_PYTHON_ROOT)
isEmpty(PYTHON_ROOT) {
    exists($$PWD/depspath.pri): include($$PWD/depspath.pri)
} else {
    PYTHON_VERSION = $$(NRPYCPP_PYTHON_VERSION)
}
isEmpty(PYTHON_ROOT):isEmpty(PYTHON_INCLUDE_PATH) {
    error( "nrPyCpp: create depspath.pri from depspath.pri.sample or set NRPYCPP_PYTHON_ROOT" )
}

QT -= gui
CONFIG += c++17 debug_and_release
CONFIG -= app_bundle

TEMPLATE = lib
# the library version follows the package one (libnrPyCpp.so.2, libnrPyCpp.2.dylib); no version in the dll name
VERSION = $$NRPYCPP_VERSION
win32: CONFIG += skip_target_version_ext
win32 {
    CONFIG += console

    QMAKE_CXXFLAGS_RELEASE += /Zi
    QMAKE_LFLAGS_RELEASE += /DEBUG
    QMAKE_LFLAGS_RELEASE += /OPT:REF
    QMAKE_LFLAGS_RELEASE += /OPT:ICF

    DEFINES += NRPYQT_LIB_EXPORTS
}

macos: CONFIG += dll #to build a framework

# Qt 6.8's qyieldcpu.h calls __yield() without declaring it, which recent Apple clang (Xcode 26) rejects
macos: QMAKE_CXXFLAGS += -Wno-error=implicit-function-declaration

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
        nrpycppmodule.cpp \
        pyenvironment.cpp \
        pyrunner.cpp \
        PyRunnerQt.cpp


!isEmpty(PYTHON_ROOT) {
    # a relocatable runtime with the python-build-standalone ("install_only") layout
    isEmpty(PYTHON_VERSION): PYTHON_VERSION = 3.13
    PYTHON_ROOT = $$clean_path($$PYTHON_ROOT)
    win32 {
        PYTHON_INCLUDE_PATH = $$PYTHON_ROOT/include
        PYTHON_LIB_PATH     = $$PYTHON_ROOT/libs
        PYTHON_DLL_PATH     = $$PYTHON_ROOT
    } else {
        PYTHON_INCLUDE_PATH = $$PYTHON_ROOT/include/python$${PYTHON_VERSION}
        PYTHON_LIB_PATH     = $$PYTHON_ROOT/lib
    }
    # lets development builds find the stdlib without any further setup (see nrPyCppConfig.h)
    BUILD_CONFIG_LINES = "$${LITERAL_HASH}define NRPYCPP_BUILD_PYTHON_HOME \"$$PYTHON_ROOT\""
} else {
    win32: PYTHON_DLL_PATH = $$PWD/python-win32
    BUILD_CONFIG_LINES = "// no build time python home: PYTHON_ROOT was not set"
}
!exists($$PYTHON_INCLUDE_PATH/Python.h): error( "nrPyCpp: cannot find Python.h in $$PYTHON_INCLUDE_PATH" )
write_file($$OUT_PWD/nrpycpp_build_config.h, BUILD_CONFIG_LINES)
INCLUDEPATH += $$OUT_PWD

# windows import libraries have no dot in the version (python313.lib)
win32: PYTHON_LIBNAME = python$$replace(PYTHON_VERSION, "\\.", "")
else:  PYTHON_LIBNAME = python$${PYTHON_VERSION}

INCLUDEPATH += $${PYTHON_INCLUDE_PATH}
LIBS += -L$${PYTHON_LIB_PATH} -l$${PYTHON_LIBNAME}
message("nrPyCpp linking python lib version: $${PYTHON_VERSION}")
message("nrPyCpp LIBS = $${LIBS}")


HEADERS += \
    PyCall.h \
    PyCallBackIface.h \
    nrpycppmodule.h \
    nrPyCppConfig.h \
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
$$PWD/PyCallBackIface.h \
$$PWD/PyRunnerQt.h \
$$PWD/nrPyCppConfig.h

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

#copy the python lib to be distributed (on mac adjusting the libraries id if needed)
macos: {
    PYTHONLIB=libpython$${PYTHON_VERSION}.dylib
    QMAKE_POST_LINK+="cp \"$${PYTHON_LIB_PATH}/$${PYTHONLIB}\" \"$$DSTDIR\" $$escape_expand(\\n\\t)"
    isEmpty(PYTHON_ROOT) {
        # Xcode's Python3.framework: we have to change its id, then re-sign it, since a library with
        # a broken signature is killed by the loader (SIGKILL with no message on Apple Silicon)
        QMAKE_POST_LINK+="install_name_tool -id @rpath/$${PYTHONLIB} \"$$DSTDIR/$${PYTHONLIB}\" $$escape_expand(\\n\\t)"
        QMAKE_POST_LINK+="codesign --force --sign - \"$$DSTDIR/$${PYTHONLIB}\" $$escape_expand(\\n\\t)"
        QMAKE_POST_LINK+="install_name_tool -change @rpath/Python3.framework/Versions/$${PYTHON_VERSION}/Python3 @rpath/$${PYTHONLIB} \"$${DLL}.dylib\" $$escape_expand(\\n\\t)"
    }
    # python-build-standalone already uses @rpath/libpythonX.Y.dylib as id: a plain copy keeps it signed
}

linux:!isEmpty(PYTHON_ROOT) {
    QMAKE_POST_LINK+="cp -aP \"$${PYTHON_LIB_PATH}\"/lib$${PYTHON_LIBNAME}.so* \"$$DSTDIR\" $$escape_expand(\\n\\t)"
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
    PYTHONLIB=$$PYTHON_DLL_PATH/$${PYTHON_LIBNAME}.dll
    QMAKE_POST_LINK+="$$QMAKE_COPY $$replace(PYTHONLIB,"/","\\") \"$$DSTDIR\" $$escape_expand(\\n\\t)"
}


