#include "runtime.h"

namespace lighter::http::detail {

curl::easy_error ensure_curl_runtime() noexcept {
    struct RuntimeState {
        RuntimeState() noexcept : code(curl::global_init()) {}

        ~RuntimeState() {
            if (curl::ok(code)) {
                curl::global_cleanup();
            }
        }

        curl::easy_error code = CURLE_OK;
    };

    static RuntimeState runtime;
    return runtime.code;
}

} // namespace lighter::http::detail
