#pragma once

#ifndef POLYHOOK2_FUNCTION_HOOK
#include "MinhookFunctionHook.hpp"
typedef MinhookFunctionHook FunctionHook;
#else
#include "PolyHook2FunctionHook.h"
typedef PolyHook2FunctionHook FunctionHook;
#endif
