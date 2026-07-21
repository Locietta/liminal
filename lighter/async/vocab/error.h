#pragma once

#include <string>
#include <string_view>
#include <utility>

#include <lighter/scalar_types.hpp>
#include <lighter/async/vocab/outcome.h>

namespace lighter {

class Error {
public:
    constexpr Error() noexcept = default;

    constexpr Error(const Error &) noexcept = default;
    constexpr Error &operator=(const Error &) noexcept = default;

    constexpr explicit Error(i32 code) noexcept : code(code) {}

    constexpr i32 value() const noexcept {
        return code;
    }

    constexpr void clear() noexcept {
        code = 0;
    }

    constexpr bool has_error() const noexcept {
        return code != 0;
    }

    constexpr explicit operator bool() const noexcept {
        return has_error();
    }

    std::string_view message() const;

    friend constexpr bool operator==(const Error &lhs, const Error &rhs) noexcept = default;

    /// lighter-specific Error codes:
    const static Error k_operation_aborted;

    /// libuv Error codes:
    const static Error k_argument_list_too_long;
    const static Error k_permission_denied;
    const static Error k_address_already_in_use;
    const static Error k_address_not_available;
    const static Error k_address_family_not_supported;
    const static Error k_resource_temporarily_unavailable;
    const static Error k_addrinfo_address_family_not_supported;
    const static Error k_addrinfo_temporary_failure;
    const static Error k_addrinfo_bad_flags_value;
    const static Error k_addrinfo_invalid_value_for_hints;
    const static Error k_addrinfo_request_canceled;
    const static Error k_addrinfo_permanent_failure;
    const static Error k_addrinfo_family_not_supported;
    const static Error k_addrinfo_out_of_memory;
    const static Error k_addrinfo_no_address;
    const static Error k_addrinfo_unknown_node_or_service;
    const static Error k_addrinfo_argument_buffer_overflow;
    const static Error k_addrinfo_resolved_protocol_unknown;
    const static Error k_addrinfo_service_not_available_for_socket_type;
    const static Error k_addrinfo_socket_type_not_supported;
    const static Error k_connection_already_in_progress;
    const static Error k_bad_file_descriptor;
    const static Error k_resource_busy_or_locked;
    const static Error k_invalid_unicode_character;
    const static Error k_software_caused_connection_abort;
    const static Error k_connection_refused;
    const static Error k_connection_reset_by_peer;
    const static Error k_destination_address_required;
    const static Error k_file_already_exists;
    const static Error k_bad_address_in_system_call_argument;
    const static Error k_file_too_large;
    const static Error k_host_is_unreachable;
    const static Error k_interrupted_system_call;
    const static Error k_invalid_argument;
    const static Error k_io_error;
    const static Error k_socket_is_already_connected;
    const static Error k_illegal_operation_on_a_directory;
    const static Error k_too_many_symbolic_links_encountered;
    const static Error k_too_many_open_files;
    const static Error k_message_too_long;
    const static Error k_name_too_long;
    const static Error k_network_is_down;
    const static Error k_network_is_unreachable;
    const static Error k_file_table_overflow;
    const static Error k_no_buffer_space_available;
    const static Error k_no_such_device;
    const static Error k_no_such_file_or_directory;
    const static Error k_not_enough_memory;
    const static Error k_machine_is_not_on_the_network;
    const static Error k_protocol_not_available;
    const static Error k_no_space_left_on_device;
    const static Error k_function_not_implemented;
    const static Error k_socket_is_not_connected;
    const static Error k_not_a_directory;
    const static Error k_directory_not_empty;
    const static Error k_socket_operation_on_non_socket;
    const static Error k_operation_not_supported_on_socket;
    const static Error k_value_too_large_for_defined_data_type;
    const static Error k_operation_not_permitted;
    const static Error k_broken_pipe;
    const static Error k_protocol_error;
    const static Error k_protocol_not_supported;
    const static Error k_protocol_wrong_type_for_socket;
    const static Error k_result_too_large;
    const static Error k_read_only_file_system;
    const static Error k_cannot_send_after_transport_endpoint_shutdown;
    const static Error k_invalid_seek;
    const static Error k_no_such_process;
    const static Error k_connection_timed_out;
    const static Error k_text_file_is_busy;
    const static Error k_cross_device_link_not_permitted;
    const static Error k_unknown_error;
    const static Error k_end_of_file;
    const static Error k_no_such_device_or_address;
    const static Error k_too_many_links;
    const static Error k_host_is_down;
    const static Error k_remote_io_error;
    const static Error k_inappropriate_ioctl_for_device;
    const static Error k_inappropriate_file_type_or_format;
    const static Error k_illegal_byte_sequence;
    const static Error k_socket_type_not_supported;
    const static Error k_no_data_available;
    const static Error k_protocol_driver_not_attached;
    const static Error k_exec_format_error;

private:
    i32 code = 0;
};

struct Cancellation {
    std::string message;

    Cancellation() noexcept = default;

    explicit Cancellation(std::string reason) : message(std::move(reason)) {}

    std::string_view reason() const noexcept {
        return message;
    }
};

/// Result<T>: value-or-Error (no cancel channel). I/O functions use this.
template <typename T>
using Result = Outcome<T, Error, void>;

} // namespace lighter
