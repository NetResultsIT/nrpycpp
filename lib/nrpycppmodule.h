/**************************************************************************
 *  Copyright (C) 2026 by NetResults S.r.l. ( http://www.netresults.it )  *
 *  Author( s ) :                                                         *
 *         Francesco Lamonica      <f.lamonica@netresults.it>             *
 **************************************************************************/

#ifndef NRPYCPPMODULE_H
#define NRPYCPPMODULE_H

#include "pyrunner.h"

/*
 * The built-in python module "nrpycpp": how the scripts talk back to the C++ side, per runner.
 * Internal to the library, see docs/USAGE.md for the python API.
 */

//the init function of the module, to register with PyImport_AppendInittab() before the interpreter starts
PyObject *nrpycppModuleInit();

//installs the sys.stdout / sys.stderr replacements and the threading hooks
//must be called with the GIL held, right after the interpreter is initialized
bool nrpycppInstallBootstrap();

#endif // NRPYCPPMODULE_H
