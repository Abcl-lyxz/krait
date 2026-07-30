#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace krait::net {

// OpenSSH's "randomart" drunken-bishop walk over a key fingerprint.
//
// Why bother when the SHA256 fingerprint is right there: people do not compare
// 43 base64 characters. They compare pictures — badly, but reliably — and a
// changed key produces a visibly different picture. It is also what
// `ssh-keygen -lv` prints, so a user can check ours against theirs, which is
// the whole reason this follows OpenSSH's algorithm exactly rather than
// inventing a prettier one.
//
// `digest` is the RAW hash bytes (what ssh_get_publickey_hash hands back), not
// the base64 text. `title` goes in the top border, e.g. "ED25519 256";
// `hashName` in the bottom, e.g. "SHA256". Both are truncated to fit rather
// than allowed to break the box.
//
// Returns 11 rows (9 field rows plus two borders) of 19 characters, separated
// by '\n', with no trailing newline.
std::string randomart(const unsigned char* digest, std::size_t length, std::string_view title,
                      std::string_view hashName);

}  // namespace krait::net
