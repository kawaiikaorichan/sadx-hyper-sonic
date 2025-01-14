// pch.h: este es un archivo de encabezado precompilado.
// Los archivos que se muestran a continuación se compilan solo una vez, lo que mejora el rendimiento de la compilación en futuras compilaciones.
// Esto también afecta al rendimiento de IntelliSense, incluida la integridad del código y muchas funciones de exploración del código.
// Sin embargo, los archivos que se muestran aquí se vuelven TODOS a compilar si alguno de ellos se actualiza entre compilaciones.
// No agregue aquí los archivos que se vayan a actualizar con frecuencia, ya que esto invalida la ventaja de rendimiento.

#ifndef PCH_H
#define PCH_H

// agregue aquí los encabezados que desea precompilar
#include <vector>
#include <algorithm>
#include <stdlib.h>
#include <time.h>

#include "framework.h"
#include "SADXModLoader.h"
#include "LandTableInfo.h"
#include "ModelInfo.h"
#include "AnimationFile.h"
#include "IniFile.hpp"
#include "Trampoline.h"
#include "FunctionHook.h"
#include "IniFile.hpp"
#include "util.h"
#include "helper.h"
#include "UsercallFunctionHandler.h"

#endif //PCH_H
