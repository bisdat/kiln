// SPDX-License-Identifier: MIT
// kiln.log v1 (abi/services/kiln_log.h): when the game logged the line a log subscriber is being handed.
#include "../log.h"
#include "service.h"
#include "services/kiln_log.h"

namespace kiln::log {
namespace {

int64_t KILN_CALL l_line_time() { return current_line_time(); }

const KilnLogApi kTable = {{sizeof(KilnLogApi), KILN_LOG_VERSION}, &l_line_time};

}  // namespace
}  // namespace kiln::log

KILN_SERVICE(log, KILN_LOG_SERVICE, KILN_LOG_VERSION, &kiln::log::kTable, nullptr);
