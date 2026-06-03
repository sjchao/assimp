/*
Open Asset Import Library (assimp)
----------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the
following conditions are met:

* Redistributions of source code must retain the above
  copyright notice, this list of conditions and the
  following disclaimer.
r
* Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the
  following disclaimer in the documentation and/or other
  materials provided with the distribution.

* Neither the name of the assimp team, nor the names of its
  contributors may be used to endorse or promote products
  derived from this software without specific prior
  written permission of the assimp team.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

----------------------------------------------------------------------
*/

/** @file  FBXImporter.cpp
 *  @brief Implementation of the FBX importer.
 */

#ifndef ASSIMP_BUILD_NO_FBX_IMPORTER

#include "FBXImporter.h"

#include "FBXConverter.h"
#include "FBXDocument.h"
#include "FBXParser.h"
#include "FBXTokenizer.h"
#include "FBXUtil.h"

#include <assimp/MemoryIOWrapper.h>
#include <assimp/StreamReader.h>
#include <assimp/importerdesc.h>
#include <assimp/Importer.hpp>

#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <limits.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Assimp {

template <>
const char *LogFunctions<FBXImporter>::Prefix() {
    return "FBX: ";
}

} // namespace Assimp

using namespace Assimp;
using namespace Assimp::Formatter;
using namespace Assimp::FBX;

namespace {
static constexpr char BinaryFbxMagic[] = "Kaydara FBX Binary";

static bool SupportsMappedBinaryFbx() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return false;
#else
    return true;
#endif
}

static std::string BuildDefaultEmbeddedTextureSpillDirectory(const std::string &filePath) {
#ifdef _WIN32
    std::error_code errorCode;
    std::filesystem::path absolutePath = std::filesystem::absolute(std::filesystem::u8path(filePath), errorCode);
    if (errorCode) {
        absolutePath = std::filesystem::u8path(filePath);
    }

    std::filesystem::path parentPath = absolutePath.parent_path();
    if (parentPath.empty()) {
        parentPath = std::filesystem::current_path(errorCode);
        if (errorCode) {
            parentPath.clear();
        }
    }

    return (parentPath / ".tmp" / "textures").lexically_normal().u8string();
#else
    std::string absolutePath = filePath;
    if (absolutePath.empty() || absolutePath.front() != '/') {
        char cwdBuffer[PATH_MAX] = {};
        if (::getcwd(cwdBuffer, sizeof(cwdBuffer)) != nullptr) {
            absolutePath.assign(cwdBuffer);
            if (!absolutePath.empty() && absolutePath.back() != '/') {
                absolutePath.push_back('/');
            }
            absolutePath += filePath;
        }
    }

    const size_t lastSeparator = absolutePath.find_last_of('/');
    std::string parentPath;
    if (lastSeparator == std::string::npos) {
        char cwdBuffer[PATH_MAX] = {};
        if (::getcwd(cwdBuffer, sizeof(cwdBuffer)) != nullptr) {
            parentPath.assign(cwdBuffer);
        }
    } else if (lastSeparator == 0) {
        parentPath = "/";
    } else {
        parentPath = absolutePath.substr(0, lastSeparator);
    }

    if (parentPath.empty()) {
        return ".tmp/textures";
    }
    if (parentPath.back() == '/') {
        return parentPath + ".tmp/textures";
    }
    return parentPath + "/.tmp/textures";
#endif
}

class MappedFileBuffer {
public:
    MappedFileBuffer() = default;

    ~MappedFileBuffer() {
        Close();
    }

    bool Open(const std::string &path, size_t size) {
        if (0 == size) {
            return false;
        }

        Close();

#ifdef _WIN32
        const std::filesystem::path fsPath = std::filesystem::u8path(path);
        fileHandle = ::CreateFileW(fsPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (INVALID_HANDLE_VALUE == fileHandle) {
            fileHandle = nullptr;
            return false;
        }

        mappingHandle = ::CreateFileMappingW(fileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (nullptr == mappingHandle) {
            Close();
            return false;
        }

        mappedData = ::MapViewOfFile(mappingHandle, FILE_MAP_READ, 0, 0, 0);
        if (nullptr == mappedData) {
            Close();
            return false;
        }
#else
        fileDescriptor = ::open(std::filesystem::u8path(path).c_str(), O_RDONLY);
        if (-1 == fileDescriptor) {
            return false;
        }

        mappedData = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fileDescriptor, 0);
        if (MAP_FAILED == mappedData) {
            mappedData = nullptr;
            Close();
            return false;
        }
#endif

        mappedSize = size;
        return true;
    }

    void Close() {
#ifdef _WIN32
        if (mappedData) {
            ::UnmapViewOfFile(mappedData);
            mappedData = nullptr;
        }
        if (mappingHandle) {
            ::CloseHandle(mappingHandle);
            mappingHandle = nullptr;
        }
        if (fileHandle) {
            ::CloseHandle(fileHandle);
            fileHandle = nullptr;
        }
#else
        if (mappedData) {
            ::munmap(mappedData, mappedSize);
            mappedData = nullptr;
        }
        if (-1 != fileDescriptor) {
            ::close(fileDescriptor);
            fileDescriptor = -1;
        }
#endif
        mappedSize = 0;
    }

    const char *Data() const {
        return static_cast<const char *>(mappedData);
    }

    size_t Size() const {
        return mappedSize;
    }

    bool IsOpen() const {
        return nullptr != mappedData;
    }

private:
#ifdef _WIN32
    HANDLE fileHandle{ nullptr };
    HANDLE mappingHandle{ nullptr };
#else
    int fileDescriptor{ -1 };
#endif
    void *mappedData{ nullptr };
    size_t mappedSize{ 0 };
};

static constexpr aiImporterDesc desc = {
    "Autodesk FBX Importer",
    "",
    "",
    "",
    aiImporterFlags_SupportTextFlavour,
    0,
    0,
    0,
    0,
    "fbx"
};
} // namespace

// ------------------------------------------------------------------------------------------------
// Returns whether the class can handle the format of the given file.
bool FBXImporter::CanRead(const std::string & pFile, IOSystem * pIOHandler, bool /*checkSig*/) const {
    // at least ASCII-FBX files usually have a 'FBX' somewhere in their head
    static const char *tokens[] = { " \n\r\n ", "fbx" };
    return SearchFileHeaderForToken(pIOHandler, pFile, tokens, AI_COUNT_OF(tokens));
}

// ------------------------------------------------------------------------------------------------
// List all extensions handled by this loader
const aiImporterDesc *FBXImporter::GetInfo() const {
    return &desc;
}

// ------------------------------------------------------------------------------------------------
// Setup configuration properties for the loader
void FBXImporter::SetupProperties(const Importer *pImp) {
    mSettings.readAllLayers = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ALL_GEOMETRY_LAYERS, true);
    mSettings.readAllMaterials = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ALL_MATERIALS, false);
    mSettings.readMaterials = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_MATERIALS, true);
    mSettings.readTextures = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
    mSettings.readCameras = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_CAMERAS, true);
    mSettings.readLights = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_LIGHTS, true);
    mSettings.readAnimations = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, true);
    mSettings.readWeights = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, true);
    mSettings.strictMode = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_STRICT_MODE, false);
    mSettings.preservePivots = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, true);
    mSettings.optimizeEmptyAnimationCurves = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_OPTIMIZE_EMPTY_ANIMATION_CURVES, true);
    mSettings.useLegacyEmbeddedTextureNaming = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_LEGACY_NAMING, false);
    mSettings.spillEmbeddedTextures = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_EMBEDDED_TEXTURES_SPILL, false);
    mSettings.removeEmptyBones = pImp->GetPropertyBool(AI_CONFIG_IMPORT_REMOVE_EMPTY_BONES, true);
    mSettings.convertToMeters = pImp->GetPropertyBool(AI_CONFIG_FBX_CONVERT_TO_M, false);
    mSettings.ignoreUpDirection = pImp->GetPropertyBool(AI_CONFIG_IMPORT_FBX_IGNORE_UP_DIRECTION, false);
    mSettings.useSkeleton = pImp->GetPropertyBool(AI_CONFIG_FBX_USE_SKELETON_BONE_CONTAINER, false);
}

// ------------------------------------------------------------------------------------------------
// Imports the given file into the given scene structure.
void FBXImporter::InternReadFile(const std::string &pFile, aiScene *pScene, IOSystem *pIOHandler) {
    auto streamCloser = [&](IOStream *pStream) {
        pIOHandler->Close(pStream);
    };
    std::unique_ptr<IOStream, decltype(streamCloser)> stream(pIOHandler->Open(pFile, "rb"), streamCloser);
    if (!stream) {
        ThrowException("Could not open file for reading");
    }

    ASSIMP_LOG_DEBUG("Reading FBX file");
    const std::string embeddedTextureSpillDirectory = mSettings.spillEmbeddedTextures ?
            BuildDefaultEmbeddedTextureSpillDirectory(pFile) :
            std::string();

    const size_t fileSize = stream->FileSize();
    char header[sizeof(BinaryFbxMagic)] = {};
    const size_t headerLength = fileSize < (sizeof(BinaryFbxMagic) - 1) ? fileSize : (sizeof(BinaryFbxMagic) - 1);
    stream->Read(header, 1, headerLength);
    stream->Seek(0, aiOrigin_SET);

    const bool is_binary = headerLength == sizeof(BinaryFbxMagic) - 1 &&
                           !std::strncmp(header, BinaryFbxMagic, sizeof(BinaryFbxMagic) - 1);

    MappedFileBuffer mappedContents;
    std::vector<char> contents;
    const char *begin = nullptr;
    size_t bufferLength = fileSize;

    if (is_binary && SupportsMappedBinaryFbx() && mappedContents.Open(pFile, fileSize)) {
        begin = mappedContents.Data();
        bufferLength = mappedContents.Size();
    } else {
        // ASCII FBX tokenization expects a trailing NUL byte, so keep a copied
        // buffer for textual files or when memory mapping is unavailable.
        contents.resize(fileSize + 1);
        stream->Read(contents.data(), 1, fileSize);
        contents[fileSize] = 0;
        begin = contents.data();
        bufferLength = fileSize;
    }

    // broad-phase tokenized pass in which we identify the core
    // syntax elements of FBX (brackets, commas, key:value mappings)
    TokenList tokens;
    Assimp::StackAllocator tempAllocator;
    try {
        if (is_binary) {
            TokenizeBinary(tokens, begin, bufferLength, tempAllocator);
        } else {
            Tokenize(tokens, begin, tempAllocator);
        }

        // use this information to construct a very rudimentary
        // parse-tree representing the FBX scope structure
        Parser parser(tokens, tempAllocator, is_binary);

        // take the raw parse-tree and convert it to a FBX DOM
        Document doc(parser, mSettings, embeddedTextureSpillDirectory);

        // convert the FBX DOM to aiScene
        ConvertToAssimpScene(pScene, doc, mSettings.removeEmptyBones);

        // size relative to cm
        float size_relative_to_cm = doc.GlobalSettings().UnitScaleFactor();
        if (size_relative_to_cm == 0.0) {
            // BaseImporter later asserts that fileScale is non-zero.
            ThrowException("The UnitScaleFactor must be non-zero");
        }

        // Set FBX file scale is relative to CM must be converted to M for
        // assimp universal format (M)
        SetFileScale(size_relative_to_cm * 0.01f);

        // This collection does not own the memory for the tokens, but we need to call their d'tor
        std::for_each(tokens.begin(), tokens.end(), Util::destructor_fun<Token>());

    } catch (std::exception &) {
        std::for_each(tokens.begin(), tokens.end(), Util::destructor_fun<Token>());
        throw;
    }
}

#endif // !ASSIMP_BUILD_NO_FBX_IMPORTER
