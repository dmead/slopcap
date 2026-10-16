// Shim for the QHY SDK headers: qhyccdstruct.h does #include "config.h", which the packaged headers
// (indi-3rdparty-libs) don't ship. The headers need two of its settings: __CPP_MODE__ (C linkage for the SDK's
// functions) and CALLBACK_MODE_SUPPORT (declares the callback type). Keeping it here stops builds from picking up
// another SDK install's config.h.
#ifndef SLOPCAP_QHY_CONFIG_SHIM_H
#define SLOPCAP_QHY_CONFIG_SHIM_H
#define __CPP_MODE__ 1
#define CALLBACK_MODE_SUPPORT 1
#endif
