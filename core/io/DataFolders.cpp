#include "io/DataFolders.h"

#include <algorithm>
#include <cctype>

DataKind DataKindOf(const std::string &fileName, bool inStatesDir)
{
    if (fileName.find('/') != std::string::npos)
        return DataKind::Games; // (captures/, recordings/, colorpacks/...)
    std::string ext;
    if (const size_t dot = fileName.rfind('.'); dot != std::string::npos)
        ext = fileName.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!inStatesDir)
        return ext == "srm" ? DataKind::Saves : DataKind::Games;
    // "<game>.state", ".state2", ".stateimg", ".statepng3"... (Emulator::StateFileName)
    return ext.compare(0, 5, "state") == 0 ? DataKind::States : DataKind::Settings;
}
