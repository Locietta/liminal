#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <lighter/http/common.h>

namespace lighter::http::detail {

bool iequals(std::string_view lhs, std::string_view rhs) noexcept;

void upsert_header(std::vector<Header> &headers, std::string name, std::string value);

void insert_header(std::vector<Header> &headers, std::string name, std::string value);

std::string trim_ascii(std::string_view text);

std::string lower_ascii(std::string_view text);

std::string percent_encode(std::string_view text);

std::string encode_pairs(const std::vector<QueryParam> &pairs);

std::string base64_encode(std::string_view text);

} // namespace lighter::http::detail
