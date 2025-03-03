#ifndef __CONFIG_H__
#define __CONFIG_H__

#include "LanguageBarrier.h"
#include "lbjson.h"

#ifdef DEFINE_CONFIG
#define CONFIG_GLOBAL
#else
#define CONFIG_GLOBAL extern
#endif

namespace lb {
CONFIG_GLOBAL json config;
CONFIG_GLOBAL json rawConfig;
void configInit();
const std::string configGetGameName();
const std::string configGetPatchName();
}  // namespace lb

#endif  // !__CONFIG_H__
