// A fake of ZWO's filter wheel SDK (see fake_efw.hpp).
#include "fakes/fake_efw.hpp"

#include <libasi/EFW_filter.h>

#include <cstring>

namespace fake {
Efw& efw() {
    static Efw e;
    return e;
}
}  // namespace fake

using fake::efw;

extern "C" {

int EFWGetNum() { return efw().count; }

EFW_ERROR_CODE EFWGetID(int index, int* id) {
    if (index < 0 || index >= efw().count) return EFW_ERROR_INVALID_INDEX;
    *id = index + 3;   // ids aren't indexes
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWOpen(int id) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    if (efw().removed) return EFW_ERROR_REMOVED;
    efw().open = true;
    ++efw().opens;
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWGetProperty(int id, EFW_INFO* info) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    if (efw().open && efw().busyProperty > 0) {
        --efw().busyProperty;
        return EFW_ERROR_MOVING;
    }
    info->ID = id;
    std::strncpy(info->Name, efw().name.c_str(), sizeof info->Name - 1);
    info->slotNum = efw().open ? efw().slotCount : 0;
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWGetSerialNumber(int id, EFW_SN* sn) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    for (int i = 0; i < 8; ++i) sn->id[i] = static_cast<unsigned char>(0xa0 + i);
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWGetPosition(int id, int* position) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    if (!efw().open) return EFW_ERROR_CLOSED;
    if (efw().removed) return EFW_ERROR_REMOVED;
    if (efw().errorState) return EFW_ERROR_ERROR_STATE;
    auto& e = efw();
    if (e.at != e.target) {
        e.at = (e.at + 1) % e.slotCount;   // one slot per look, one way round
        *position = -1;
        return EFW_SUCCESS;
    }
    *position = e.at;
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWSetPosition(int id, int position) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    if (!efw().open) return EFW_ERROR_CLOSED;
    if (position < 0 || position >= efw().slotCount) return EFW_ERROR_INVALID_VALUE;
    if (efw().at != efw().target) return EFW_ERROR_MOVING;
    efw().target = position;
    ++efw().moves;
    return EFW_SUCCESS;
}

EFW_ERROR_CODE EFWClose(int id) {
    if (id != 3) return EFW_ERROR_INVALID_ID;
    efw().open = false;
    ++efw().closes;
    return EFW_SUCCESS;
}

}  // extern "C"
