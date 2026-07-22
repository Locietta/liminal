#pragma once

#include <cassert>
#include <string_view>
#include <utility>

#include <curl/curl.h>

namespace lighter::curl {

using EasyError = CURLcode;
using MultiError = CURLMcode;
using ShareError = CURLSHcode;

inline bool ok(EasyError code) noexcept { return code == CURLE_OK; }

inline bool ok(MultiError code) noexcept { return code == CURLM_OK; }

inline bool ok(ShareError code) noexcept { return code == CURLSHE_OK; }

inline EasyError to_easy_error(MultiError code) noexcept { return ok(code) ? CURLE_OK : CURLE_FAILED_INIT; }

inline EasyError to_easy_error(ShareError code) noexcept { return ok(code) ? CURLE_OK : CURLE_FAILED_INIT; }

inline std::string_view message(EasyError code) noexcept {
    auto *text = ::curl_easy_strerror(code);
    return text ? std::string_view(text) : std::string_view();
}

inline std::string_view message(MultiError code) noexcept {
    auto *text = ::curl_multi_strerror(code);
    return text ? std::string_view(text) : std::string_view();
}

inline std::string_view message(ShareError code) noexcept {
    auto *text = ::curl_share_strerror(code);
    return text ? std::string_view(text) : std::string_view();
}

inline EasyError global_init(long flags = CURL_GLOBAL_DEFAULT) noexcept { return ::curl_global_init(flags); }

inline void global_cleanup() noexcept { ::curl_global_cleanup(); }

inline CURL *easy_init() noexcept { return ::curl_easy_init(); }

inline void easy_cleanup(CURL *handle) noexcept {
    if (handle) {
        ::curl_easy_cleanup(handle);
    }
}

inline CURLM *multi_init() noexcept { return ::curl_multi_init(); }

inline CURLSH *share_init() noexcept { return ::curl_share_init(); }

inline void multi_cleanup(CURLM *handle) noexcept {
    if (handle) {
        ::curl_multi_cleanup(handle);
    }
}

inline void share_cleanup(CURLSH *handle) noexcept {
    if (handle) {
        ::curl_share_cleanup(handle);
    }
}

inline void slist_free_all(curl_slist *list) noexcept {
    if (list) {
        ::curl_slist_free_all(list);
    }
}

template <typename T>
inline EasyError setopt(CURL *handle, CURLoption option, T value) noexcept {
    assert(handle != nullptr && "curl::setopt requires non-null easy handle");
    return static_cast<EasyError>(::curl_easy_setopt(handle, option, value));
}

template <typename T>
inline EasyError getinfo(CURL *handle, CURLINFO info, T value) noexcept {
    assert(handle != nullptr && "curl::getinfo requires non-null easy handle");
    return static_cast<EasyError>(::curl_easy_getinfo(handle, info, value));
}

template <typename T>
inline MultiError multi_setopt(CURLM *handle, CURLMoption option, T value) noexcept {
    assert(handle != nullptr && "curl::multi_setopt requires non-null multi handle");
    return static_cast<MultiError>(::curl_multi_setopt(handle, option, value));
}

template <typename T>
inline ShareError share_setopt(CURLSH *handle, CURLSHoption option, T value) noexcept {
    assert(handle != nullptr && "curl::share_setopt requires non-null share handle");
    return static_cast<ShareError>(::curl_share_setopt(handle, option, value));
}

inline MultiError multi_add_handle(CURLM *multi, CURL *easy) noexcept {
    assert(multi != nullptr && "curl::multi_add_handle requires non-null multi handle");
    assert(easy != nullptr && "curl::multi_add_handle requires non-null easy handle");
    return static_cast<MultiError>(::curl_multi_add_handle(multi, easy));
}

inline MultiError multi_remove_handle(CURLM *multi, CURL *easy) noexcept {
    assert(multi != nullptr && "curl::multi_remove_handle requires non-null multi handle");
    assert(easy != nullptr && "curl::multi_remove_handle requires non-null easy handle");
    return static_cast<MultiError>(::curl_multi_remove_handle(multi, easy));
}

inline MultiError multi_assign(CURLM *multi, curl_socket_t socket, void *ptr) noexcept {
    assert(multi != nullptr && "curl::multi_assign requires non-null multi handle");
    return static_cast<MultiError>(::curl_multi_assign(multi, socket, ptr));
}

inline MultiError multi_socket_action(CURLM *multi, curl_socket_t socket, int events, int *running_handles) noexcept {
    assert(multi != nullptr && "curl::multi_socket_action requires non-null multi handle");
    assert(running_handles != nullptr && "curl::multi_socket_action requires running counter");
    return static_cast<MultiError>(::curl_multi_socket_action(multi, socket, events, running_handles));
}

inline CURLMsg *multi_info_read(CURLM *multi, int *msgs_in_queue) noexcept {
    assert(multi != nullptr && "curl::multi_info_read requires non-null multi handle");
    assert(msgs_in_queue != nullptr && "curl::multi_info_read requires queue counter");
    return ::curl_multi_info_read(multi, msgs_in_queue);
}

inline curl_slist *slist_append(curl_slist *list, const char *text) noexcept {
    assert(text != nullptr && "curl::slist_append requires non-null text");
    return ::curl_slist_append(list, text);
}

struct EasyHandle {

    EasyHandle() noexcept = default;

    explicit EasyHandle(CURL *handle) noexcept : handle(handle) {}

    ~EasyHandle() { reset(); }

    EasyHandle(const EasyHandle &) = delete;
    EasyHandle &operator=(const EasyHandle &) = delete;

    EasyHandle(EasyHandle &&other) noexcept : handle(other.release()) {}

    EasyHandle &operator=(EasyHandle &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    static EasyHandle create() noexcept { return EasyHandle(easy_init()); }

    CURL *get() const noexcept { return handle; }

    explicit operator bool() const noexcept { return handle != nullptr; }

    CURL *release() noexcept { return std::exchange(handle, nullptr); }

    void reset(CURL *next = nullptr) noexcept {
        easy_cleanup(handle);
        handle = next;
    }

private:
    CURL *handle = nullptr;
};

struct MultiHandle {

    MultiHandle() noexcept = default;

    explicit MultiHandle(CURLM *handle) noexcept : handle(handle) {}

    ~MultiHandle() { reset(); }

    MultiHandle(const MultiHandle &) = delete;
    MultiHandle &operator=(const MultiHandle &) = delete;

    MultiHandle(MultiHandle &&other) noexcept : handle(other.release()) {}

    MultiHandle &operator=(MultiHandle &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    static MultiHandle create() noexcept { return MultiHandle(multi_init()); }

    CURLM *get() const noexcept { return handle; }

    explicit operator bool() const noexcept { return handle != nullptr; }

    CURLM *release() noexcept { return std::exchange(handle, nullptr); }

    void reset(CURLM *next = nullptr) noexcept {
        multi_cleanup(handle);
        handle = next;
    }

private:
    CURLM *handle = nullptr;
};

struct ShareHandle {

    ShareHandle() noexcept = default;

    explicit ShareHandle(CURLSH *handle) noexcept : handle(handle) {}

    ~ShareHandle() { reset(); }

    ShareHandle(const ShareHandle &) = delete;
    ShareHandle &operator=(const ShareHandle &) = delete;

    ShareHandle(ShareHandle &&other) noexcept : handle(other.release()) {}

    ShareHandle &operator=(ShareHandle &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    static ShareHandle create() noexcept { return ShareHandle(share_init()); }

    CURLSH *get() const noexcept { return handle; }

    explicit operator bool() const noexcept { return handle != nullptr; }

    CURLSH *release() noexcept { return std::exchange(handle, nullptr); }

    void reset(CURLSH *next = nullptr) noexcept {
        share_cleanup(handle);
        handle = next;
    }

private:
    CURLSH *handle = nullptr;
};

struct SList {

    SList() noexcept = default;

    explicit SList(curl_slist *list) noexcept : head(list) {}

    ~SList() { reset(); }

    SList(const SList &) = delete;
    SList &operator=(const SList &) = delete;

    SList(SList &&other) noexcept : head(other.release()) {}

    SList &operator=(SList &&other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    curl_slist *get() const noexcept { return head; }

    explicit operator bool() const noexcept { return head != nullptr; }

    curl_slist *release() noexcept { return std::exchange(head, nullptr); }

    void reset(curl_slist *next = nullptr) noexcept {
        slist_free_all(head);
        head = next;
    }

    bool append(const char *text) noexcept {
        auto *next = slist_append(head, text);
        if (next == nullptr) {
            return false;
        }
        head = next;
        return true;
    }

private:
    curl_slist *head = nullptr;
};

} // namespace lighter::curl
