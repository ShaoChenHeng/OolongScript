#include "math.h"
#include "vm.h"
#include <math.h>

static Value averageNative(DictuVM *vm, int argCount, Value *args) {
    double average = 0;

    if (argCount == 0) {
        return NUMBER_VAL(0);
    } else if (argCount == 1 && IS_LIST(args[0])) {
        ObjList *list = AS_LIST(args[0]);
        argCount = list->values.count;
        args = list->values.values;
    }

    for (int i = 0; i < argCount; ++i) {
        Value value = args[i];
        if (!IS_NUMBER(value)) {
            runtimeError(vm, "A non-number value passed to average()");
            return EMPTY_VAL;
        }
        average = average + AS_NUMBER(value);
    }

    return NUMBER_VAL(average / argCount);
}

static Value floorNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "floor() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to floor()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(floor(AS_NUMBER(args[0])));
}

static Value roundNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "round() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to round()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(round(AS_NUMBER(args[0])));
}

static Value ceilNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "ceil() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to ceil()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(ceil(AS_NUMBER(args[0])));
}

static Value absNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "abs() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to abs()");
        return EMPTY_VAL;
    }

    double absValue = AS_NUMBER(args[0]);

    if (absValue < 0)
        return NUMBER_VAL(absValue * -1);

    return NUMBER_VAL(absValue);
}

static Value maxNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount == 0) {
        return NUMBER_VAL(0);
    } else if (argCount == 1 && IS_LIST(args[0])) {
        ObjList *list = AS_LIST(args[0]);
        argCount = list->values.count;
        args = list->values.values;
    }

    double maximum = AS_NUMBER(args[0]);

    for (int i = 1; i < argCount; ++i) {
        Value value = args[i];
        if (!IS_NUMBER(value)) {
            runtimeError(vm, "A non-number value passed to max()");
            return EMPTY_VAL;
        }

        double current = AS_NUMBER(value);

        if (maximum < current) {
            maximum = current;
        }
    }

    return NUMBER_VAL(maximum);
}

static Value minNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount == 0) {
        return NUMBER_VAL(0);
    } else if (argCount == 1 && IS_LIST(args[0])) {
        ObjList *list = AS_LIST(args[0]);
        argCount = list->values.count;
        args = list->values.values;
    }

    double minimum = AS_NUMBER(args[0]);

    for (int i = 1; i < argCount; ++i) {
        Value value = args[i];
        if (!IS_NUMBER(value)) {
            runtimeError(vm, "A non-number value passed to min()");
            return EMPTY_VAL;
        }

        double current = AS_NUMBER(value);

        if (minimum > current) {
            minimum = current;
        }
    }

    return NUMBER_VAL(minimum);
}

static Value sumNative(DictuVM *vm, int argCount, Value *args) {
    double sum = 0;

    if (argCount == 0) {
        return NUMBER_VAL(0);
    } else if (argCount == 1 && IS_LIST(args[0])) {
        ObjList *list = AS_LIST(args[0]);
        argCount = list->values.count;
        args = list->values.values;
    }

    for (int i = 0; i < argCount; ++i) {
        Value value = args[i];
        if (!IS_NUMBER(value)) {
            runtimeError(vm, "A non-number value passed to sum()");
            return EMPTY_VAL;
        }
        sum = sum + AS_NUMBER(value);
    }

    return NUMBER_VAL(sum);
}

static Value sqrtNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "sqrt() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to sqrt()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(sqrt(AS_NUMBER(args[0])));
}

static Value sinNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "sin() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to sin()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(sin(AS_NUMBER(args[0])));
}

static Value cosNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "cos() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to cos()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(cos(AS_NUMBER(args[0])));
}

static Value tanNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "tan() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to tan()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(tan(AS_NUMBER(args[0])));
}

static Value asinNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "asin() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to asin()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(asin(AS_NUMBER(args[0])));
}

static Value acosNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "acos() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to acos()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(acos(AS_NUMBER(args[0])));
}

static Value atanNative(DictuVM *vm, int argCount, Value *args) {
    if (argCount != 1) {
        runtimeError(vm, "atan() takes 1 argument (%d given).", argCount);
        return EMPTY_VAL;
    }

    if (!IS_NUMBER(args[0])) {
        runtimeError(vm, "A non-number value passed to atan()");
        return EMPTY_VAL;
    }

    return NUMBER_VAL(atan(AS_NUMBER(args[0])));
}

static long long gcd(long long a, long long b) {
    long long r;
    while (b > 0) {
        r = a % b;
        a = b;
        b = r;
    }
    return a;
}

long long lcm(long long a, long long b) {
    return (a * b) / gcd(a, b);
}

Value createMathsModule(DictuVM *vm) {
    ObjString *name = copyString(vm, "Math", 4);
    push(vm, OBJ_VAL(name));
    ObjModule *module = newModule(vm, name);
    push(vm, OBJ_VAL(module));

    /**
     * Define Math methods
     */
    defineNative(vm, &module->values, "average", averageNative);
    defineNative(vm, &module->values, "floor", floorNative);
    defineNative(vm, &module->values, "round", roundNative);
    defineNative(vm, &module->values, "ceil", ceilNative);
    defineNative(vm, &module->values, "abs", absNative);
    defineNative(vm, &module->values, "max", maxNative);
    defineNative(vm, &module->values, "min", minNative);
    defineNative(vm, &module->values, "sum", sumNative);
    defineNative(vm, &module->values, "sqrt", sqrtNative);
    defineNative(vm, &module->values, "sin", sinNative);
    defineNative(vm, &module->values, "cos", cosNative);
    defineNative(vm, &module->values, "tan", tanNative);
    defineNative(vm, &module->values, "asin", asinNative);
    defineNative(vm, &module->values, "acos", acosNative);
    defineNative(vm, &module->values, "atan", atanNative);

    /**
     * Define Math properties
     */
    defineNativeProperty(vm, &module->values, "pi", NUMBER_VAL(3.14159265358979));
    defineNativeProperty(vm, &module->values, "e", NUMBER_VAL(2.71828182845905));
    defineNativeProperty(vm, &module->values, "phi", NUMBER_VAL(1.61803398874989));
    defineNativeProperty(vm, &module->values, "sqrt2", NUMBER_VAL(1.41421356237309));
    defineNativeProperty(vm, &module->values, "sqrte", NUMBER_VAL(1.61803398874989));
    defineNativeProperty(vm, &module->values, "sqrtpi", NUMBER_VAL(1.77245385090551));
    defineNativeProperty(vm, &module->values, "sqrtphi", NUMBER_VAL(1.27201964951406));
    defineNativeProperty(vm, &module->values, "ln2", NUMBER_VAL(0.69314718055994));
    defineNativeProperty(vm, &module->values, "ln10", NUMBER_VAL(2.30258509299404));
    pop(vm);
    pop(vm);

    return OBJ_VAL(module);
}
