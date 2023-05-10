#ifndef oolong_optionals_h
#define oolong_optionals_h

#include "util.h"
#include "math.h"
#include "time.h"
#include "random.h"
#include "http.h"
#include "object.h"

typedef Value (*BuiltinModule)(DictuVM *vm);

typedef struct {
    char *name;
    BuiltinModule module;
    bool dictuSource;
} BuiltinModules;

Value importBuiltinModule(DictuVM *vm, int index);

int findBuiltinModule(char *name, int length, bool *dictuSource);

#endif //dictu_optionals_h
