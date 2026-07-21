#include "error.h"

#include <lighter/async/detail/libuv_helper.h>

namespace lighter {

std::string_view Error::message() const {
    if (code == 0) {
        return "success";
    } else if (code == k_operation_aborted.value()) {
        return "operation aborted";
    }

    auto msg = uv::strerror(code);
    if (!msg.empty()) {
        return msg;
    }

    return "unknown Error";
}

const Error Error::k_operation_aborted{-114514};

const Error Error::k_argument_list_too_long{UV_E2BIG};
const Error Error::k_permission_denied{UV_EACCES};
const Error Error::k_address_already_in_use{UV_EADDRINUSE};
const Error Error::k_address_not_available{UV_EADDRNOTAVAIL};
const Error Error::k_address_family_not_supported{UV_EAFNOSUPPORT};
const Error Error::k_resource_temporarily_unavailable{UV_EAGAIN};
const Error Error::k_addrinfo_address_family_not_supported{UV_EAI_ADDRFAMILY};
const Error Error::k_addrinfo_temporary_failure{UV_EAI_AGAIN};
const Error Error::k_addrinfo_bad_flags_value{UV_EAI_BADFLAGS};
const Error Error::k_addrinfo_invalid_value_for_hints{UV_EAI_BADHINTS};
const Error Error::k_addrinfo_request_canceled{UV_EAI_CANCELED};
const Error Error::k_addrinfo_permanent_failure{UV_EAI_FAIL};
const Error Error::k_addrinfo_family_not_supported{UV_EAI_FAMILY};
const Error Error::k_addrinfo_out_of_memory{UV_EAI_MEMORY};
const Error Error::k_addrinfo_no_address{UV_EAI_NODATA};
const Error Error::k_addrinfo_unknown_node_or_service{UV_EAI_NONAME};
const Error Error::k_addrinfo_argument_buffer_overflow{UV_EAI_OVERFLOW};
const Error Error::k_addrinfo_resolved_protocol_unknown{UV_EAI_PROTOCOL};
const Error Error::k_addrinfo_service_not_available_for_socket_type{UV_EAI_SERVICE};
const Error Error::k_addrinfo_socket_type_not_supported{UV_EAI_SOCKTYPE};
const Error Error::k_connection_already_in_progress{UV_EALREADY};
const Error Error::k_bad_file_descriptor{UV_EBADF};
const Error Error::k_resource_busy_or_locked{UV_EBUSY};
const Error Error::k_invalid_unicode_character{UV_ECHARSET};
const Error Error::k_software_caused_connection_abort{UV_ECONNABORTED};
const Error Error::k_connection_refused{UV_ECONNREFUSED};
const Error Error::k_connection_reset_by_peer{UV_ECONNRESET};
const Error Error::k_destination_address_required{UV_EDESTADDRREQ};
const Error Error::k_file_already_exists{UV_EEXIST};
const Error Error::k_bad_address_in_system_call_argument{UV_EFAULT};
const Error Error::k_file_too_large{UV_EFBIG};
const Error Error::k_host_is_unreachable{UV_EHOSTUNREACH};
const Error Error::k_interrupted_system_call{UV_EINTR};
const Error Error::k_invalid_argument{UV_EINVAL};
const Error Error::k_io_error{UV_EIO};
const Error Error::k_socket_is_already_connected{UV_EISCONN};
const Error Error::k_illegal_operation_on_a_directory{UV_EISDIR};
const Error Error::k_too_many_symbolic_links_encountered{UV_ELOOP};
const Error Error::k_too_many_open_files{UV_EMFILE};
const Error Error::k_message_too_long{UV_EMSGSIZE};
const Error Error::k_name_too_long{UV_ENAMETOOLONG};
const Error Error::k_network_is_down{UV_ENETDOWN};
const Error Error::k_network_is_unreachable{UV_ENETUNREACH};
const Error Error::k_file_table_overflow{UV_ENFILE};
const Error Error::k_no_buffer_space_available{UV_ENOBUFS};
const Error Error::k_no_such_device{UV_ENODEV};
const Error Error::k_no_such_file_or_directory{UV_ENOENT};
const Error Error::k_not_enough_memory{UV_ENOMEM};
const Error Error::k_machine_is_not_on_the_network{UV_ENONET};
const Error Error::k_protocol_not_available{UV_ENOPROTOOPT};
const Error Error::k_no_space_left_on_device{UV_ENOSPC};
const Error Error::k_function_not_implemented{UV_ENOSYS};
const Error Error::k_socket_is_not_connected{UV_ENOTCONN};
const Error Error::k_not_a_directory{UV_ENOTDIR};
const Error Error::k_directory_not_empty{UV_ENOTEMPTY};
const Error Error::k_socket_operation_on_non_socket{UV_ENOTSOCK};
const Error Error::k_operation_not_supported_on_socket{UV_ENOTSUP};
const Error Error::k_value_too_large_for_defined_data_type{UV_EOVERFLOW};
const Error Error::k_operation_not_permitted{UV_EPERM};
const Error Error::k_broken_pipe{UV_EPIPE};
const Error Error::k_protocol_error{UV_EPROTO};
const Error Error::k_protocol_not_supported{UV_EPROTONOSUPPORT};
const Error Error::k_protocol_wrong_type_for_socket{UV_EPROTOTYPE};
const Error Error::k_result_too_large{UV_ERANGE};
const Error Error::k_read_only_file_system{UV_EROFS};
const Error Error::k_cannot_send_after_transport_endpoint_shutdown{UV_ESHUTDOWN};
const Error Error::k_invalid_seek{UV_ESPIPE};
const Error Error::k_no_such_process{UV_ESRCH};
const Error Error::k_connection_timed_out{UV_ETIMEDOUT};
const Error Error::k_text_file_is_busy{UV_ETXTBSY};
const Error Error::k_cross_device_link_not_permitted{UV_EXDEV};
const Error Error::k_unknown_error{UV_UNKNOWN};
const Error Error::k_end_of_file{UV_EOF};
const Error Error::k_no_such_device_or_address{UV_ENXIO};
const Error Error::k_too_many_links{UV_EMLINK};
const Error Error::k_host_is_down{UV_EHOSTDOWN};
const Error Error::k_remote_io_error{UV_EREMOTEIO};
const Error Error::k_inappropriate_ioctl_for_device{UV_ENOTTY};
const Error Error::k_inappropriate_file_type_or_format{UV_EFTYPE};
const Error Error::k_illegal_byte_sequence{UV_EILSEQ};
const Error Error::k_socket_type_not_supported{UV_ESOCKTNOSUPPORT};
const Error Error::k_no_data_available{UV_ENODATA};
const Error Error::k_protocol_driver_not_attached{UV_EUNATCH};
const Error Error::k_exec_format_error{UV_ENOEXEC};

} // namespace lighter
