#pragma once
#include <android/binder_ibinder.h>
#include <functional>
namespace vibratorffinput {

bool attachSamsungSehExtension(AIBinder* serviceBinder,
                               std::function<void(int32_t, float)> playHint);
}
