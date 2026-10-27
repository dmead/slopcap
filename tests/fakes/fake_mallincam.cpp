// The fake ToupTek SDK (fake_toup.inc) under MallinCam's symbols.
#include "fakes/fake_sdk.hpp"

#include <libmallincam/mallincam.h>

#define FN(x) Mallincam_##x
#define TY(x) Mallincam##x
#define CO(x) MALLINCAM_##x
#define HANDLE HMallincam
#define EVENT_CB PMALLINCAM_EVENT_CALLBACK
#define STATE fake::mallincam
#include "fakes/fake_toup.inc"
