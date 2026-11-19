// The PSN account ID a console is registered for: a 64-bit number, written
// either in decimal or as the base64 of its eight bytes, least significant first.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "proto/regist.h"

namespace psvp::app {

// Accepts both written forms; nothing for anything else.
[[nodiscard]] std::optional<proto::AccountId> parse_account_id(std::string_view text);

[[nodiscard]] std::string account_id_base64(const proto::AccountId& id);
[[nodiscard]] std::string account_id_decimal(const proto::AccountId& id);

}  // namespace psvp::app
