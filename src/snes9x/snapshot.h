#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ff.h"

bool S9xSaveState(FIL *fp);
bool S9xLoadState(FIL *fp);
