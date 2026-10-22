// Link-time compatibility shims for the static FFmpeg build.
//
// FFmpeg's codec registry pulls in the QSV decoder and encoder objects, which
// reference the QSV user-plugin loader even though this project never selects
// QSV. The oneVPL dispatcher we link has no user-plugin support, so resolve
// that dead reference here; every consumer of chast.ffmpeg links this object.

extern "C" int __cdecl MFXVideoUSER_Load(void*, const void*, unsigned int) {
    return -3;  // MFX_ERR_UNSUPPORTED
}
