#ifndef oolong_math_h
#define oolong_math_h

#include <string.h>
#include <math.h>

#include "optionals.h"
#include "vm.h"

#define FLOAT_TOLERANCE 0.00001

Value createMathsModule(DictuVM *vm);

#endif //dictu_math_h
