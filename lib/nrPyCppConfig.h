/**************************************************************************
 *  Copyright (C) 2026 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/
#ifndef NRPYCPPCONFIG_H
#define NRPYCPPCONFIG_H

#include <QString>

#ifndef NRPYQT_LIB_API
#ifdef WIN32
 #ifdef NRPYQT_LIB_EXPORTS
   #define NRPYQT_LIB_API __declspec(dllexport)
 #else
   #define NRPYQT_LIB_API __declspec(dllimport)
 #endif
#else
 #define NRPYQT_LIB_API
#endif
#endif

/*
 * Interpreter configuration: these must be called before the first PyRunnerQt is created,
 * the interpreter is started once and lives until the process exits.
 *
 * The python home (the folder holding the bundled runtime, i.e. lib/python3.X on unix or Lib\ on
 * windows) is searched in this order:
 *   1. the path passed to setPythonHome()
 *   2. the NRPYCPP_PYTHONHOME environment variable
 *   3. a "python" folder next to the executable (on macOS also in Contents/Frameworks and
 *      Contents/Resources, on linux also in ../lib)
 *   4. the runtime the library was built against (development builds only)
 *
 * The virtual environment (created by the user with the same python X.Y, e.g. "uv venv -p 3.13 ~/vdkpy")
 * is optional, its site-packages are added to the search path: setVirtualEnv(), or NRPYCPP_VENV.
 */
namespace NrPyCpp {
NRPYQT_LIB_API void setPythonHome(const QString &path);
NRPYQT_LIB_API void setVirtualEnv(const QString &path);
NRPYQT_LIB_API bool isPythonAvailable(); //starts the interpreter if needed, false if that fails
}

#endif // NRPYCPPCONFIG_H
