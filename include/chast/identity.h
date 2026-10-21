#pragma once

// Server identity helpers.
//
// The server presents a self-signed certificate that lives in the user's
// certificate store, so its key and therefore its fingerprint survive a
// restart. The client does not trust a CA: it pins the SHA-256 of the
// certificate's DER encoding, which is the identity check that works before
// there is a PKI or a rendezvous service.
//
// Fingerprints are compared in canonical form: 64 uppercase hex digits. The
// parsers accept the usual presentations (lowercase, colons, spaces) so a
// value copied from the server's log or from OpenSSL needs no reformatting.

#include <string>
#include <string_view>

namespace chast {

// Canonical 64-digit uppercase hex form of `text`, or an empty string when the
// text is not a SHA-256 fingerprint.
std::string normalize_fingerprint(std::string_view text);

// "AB12CD..." -> "AB:12:CD:...". Returns the input unchanged when it is not a
// canonical fingerprint.
std::string format_fingerprint(std::string_view hex);

// True when both sides name the same fingerprint. Invalid input never matches,
// so a corrupt pin cannot make every server acceptable.
bool fingerprint_matches(std::string_view pinned, std::string_view actual);

}  // namespace chast
