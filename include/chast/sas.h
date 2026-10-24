#pragma once

// The Secure Attention Sequence (Ctrl+Alt+Del). Windows ignores a synthesized
// E0/E2/E4 chord: the sequence is only produced by SendSAS(), which a
// LocalSystem service (the service worker) or a process covered by the
// SoftwareSASGeneration policy may call.
//
// sas.dll is loaded dynamically so the tray build and the command-line tools
// keep running on systems without it; sas_available() reports whether the
// export can be resolved.

#include <string>

namespace chast {

bool sas_available();
// Calls SendSAS(FALSE). The API reports no status, so a true return only means
// the call was made; whether the secure desktop appeared is up to Windows.
bool send_secure_attention(std::string* error);

}  // namespace chast
