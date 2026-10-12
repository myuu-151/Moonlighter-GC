#include "DiscData.h"

#include <stdexcept>

#include "Log.h"
#include "System/System.h"

namespace {

const char* const kRoot = "MLGC/Scripts/Data/";

} // namespace

std::string DiscData::Read(const std::string& path)
{
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData((std::string(kRoot) + path).c_str(), true, 0, data, size);
    if (data == nullptr) {
        LogError("mlgc: missing data file %s", path.c_str());
        throw std::runtime_error("missing data file: " + path);
    }
    std::string out(data, size);
    SYS_ReleaseFileData(data);
    return out;
}
