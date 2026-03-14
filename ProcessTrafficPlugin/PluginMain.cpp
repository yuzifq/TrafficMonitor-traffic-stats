#include "ProcNetPlugin.h"

extern "C"
{
__declspec(dllexport) ITMPlugin* TMPluginGetInstance()
{
    return &CProcNetPlugin::Instance();
}
}
