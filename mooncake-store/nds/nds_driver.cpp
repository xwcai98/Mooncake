// Keep the ACLFile driver open for the lifetime of the Store adapter.
#include "nds_driver.h"

#include <aclfile.h>
#include <aclfile/ascend.h>

bool InitializeNdsStoreDriver() {
    static const bool enabled = [] {
        if (aclFileDriverOpen().code != ACLFILE_SUCCESS)
            return false;
        const auto status = aclFileAscendEnable();
        if (status.code == ACLFILE_SUCCESS || status.code == ACLFILE_BUSY)
            return true;
        (void)aclFileDriverClose();
        return false;
    }();
    return enabled;
}
