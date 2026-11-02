#ifndef _INCLUDE_SAFEPROXY_CALLBACKS_H_
#define _INCLUDE_SAFEPROXY_CALLBACKS_H_

#include "smsdk_ext.h"

#include <bitset>

namespace safeproxy {

void EvaluateHooks(const std::bitset<SM_MAXPLAYERS> &receiving);

} // namespace safeproxy

#endif // _INCLUDE_SAFEPROXY_CALLBACKS_H_
