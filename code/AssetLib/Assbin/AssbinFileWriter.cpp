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
/** @file  AssbinFileWriter.cpp
 *  @brief Implementation of Assbin file writer.
 */

#include "AssbinFileWriter.h"
#include "Common/assbin_chunks.h"
#include "PostProcessing/ProcessHelper.h"

#include <assimp/DefaultIOSystem.h>
#include <assimp/Exceptional.h>
#include <assimp/material.h>
#include <assimp/types.h>
#include <assimp/version.h>
#include <assimp/IOStream.hpp>

#include "zlib.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#if _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4706)
#endif // _MSC_VER

namespace Assimp {

template <typename T>
size_t Write(IOStream *stream, const T &v) {
    return stream->Write(&v, sizeof(T), 1);
}

// -----------------------------------------------------------------------------------
// Serialize an aiString
template <>
inline size_t Write<aiString>(IOStream *stream, const aiString &s) {
    const size_t s2 = (uint32_t)s.length;
    stream->Write(&s, 4, 1);
    stream->Write(s.data, s2, 1);

    return s2 + 4;
}

// -----------------------------------------------------------------------------------
// Serialize an unsigned int as uint32_t
template <>
inline size_t Write<unsigned int>(IOStream *stream, const unsigned int &w) {
    const uint32_t t = (uint32_t)w;
    if (w > t) {
        // this shouldn't happen, integers in Assimp data structures never exceed 2^32
        throw DeadlyExportError("loss of data due to 64 -> 32 bit integer conversion");
    }

    stream->Write(&t, 4, 1);

    return 4;
}

// -----------------------------------------------------------------------------------
// Serialize an unsigned int as uint16_t
template <>
inline size_t Write<uint16_t>(IOStream *stream, const uint16_t &w) {
    static_assert(sizeof(uint16_t) == 2, "sizeof(uint16_t)==2");
    stream->Write(&w, 2, 1);

    return 2;
}

// -----------------------------------------------------------------------------------
// Serialize a float
template <>
inline size_t Write<float>(IOStream *stream, const float &f) {
    static_assert(sizeof(float) == 4, "sizeof(float)==4");
    stream->Write(&f, 4, 1);

    return 4;
}

// -----------------------------------------------------------------------------------
// Serialize a double
template <>
inline size_t Write<double>(IOStream *stream, const double &f) {
    static_assert(sizeof(double) == 8, "sizeof(double)==8");
    stream->Write(&f, 8, 1);

    return 8;
}

namespace {

struct TexturePropertySlotKey {
    unsigned int semantic{ 0 };
    unsigned int index{ 0 };

    bool operator<(const TexturePropertySlotKey &rhs) const {
        if (semantic != rhs.semantic) {
            return semantic < rhs.semantic;
        }
        return index < rhs.index;
    }
};

struct TexturePropertySlotState {
    std::set<int> embeddedIndices;
    std::vector<std::pair<const aiMaterialProperty *, int>> properties;
};

struct AssbinExternalTexturePlan {
    bool externalizeTextures{ false };
    std::vector<std::string> textureReferencesByIndex;
    std::unordered_map<const aiMaterialProperty *, std::vector<char>> propertyStringOverrides;
};

struct TextureAssetEntry {
    std::string absolutePath;
    std::string relativePath;
    std::string fileNameLower;
    mutable uintmax_t fileSize{ 0 };
    mutable bool hasFileSize{ false };
    mutable uint32_t contentHash{ 0 };
    mutable bool hasContentHash{ false };
};

struct TextureAssetIndex {
    std::string assetRootDirectory;
    std::vector<TextureAssetEntry> entries;
    std::unordered_map<std::string, std::vector<const TextureAssetEntry *>> entriesByFileName;
};

struct ResolvedExternalTextureReference {
    std::string absolutePath;
    std::string referencePath;
};

static bool StartsWith(const std::string &value, const char *prefix) {
    return value.rfind(prefix, 0) == 0;
}

static bool EndsWith(const std::string &value, const char *suffix) {
    const size_t suffixLength = std::strlen(suffix);
    return value.length() >= suffixLength &&
           value.compare(value.length() - suffixLength, suffixLength, suffix) == 0;
}

static bool IsTexturePropertyKey(const aiString &key) {
    const std::string keyString(key.C_Str());
    return keyString == _AI_MATKEY_TEXTURE_BASE || (StartsWith(keyString, "$raw.") && EndsWith(keyString, "|file"));
}

static unsigned int InferTexturePropertySemantic(const aiMaterialProperty *prop) {
    ai_assert(prop != nullptr);
    const std::string key(prop->mKey.C_Str());
    unsigned int semantic = prop->mSemantic;
    if (!StartsWith(key, "$raw.") || !EndsWith(key, "|file")) {
        return semantic;
    }
    if (key.find("NormalMap") != std::string::npos) {
        return aiTextureType_NORMALS;
    }
    if (key.find("DiffuseColor") != std::string::npos || key.find("Diffuse") != std::string::npos) {
        return aiTextureType_DIFFUSE;
    }
    if (key.find("SpecularColor") != std::string::npos || key.find("Specular") != std::string::npos) {
        return aiTextureType_SPECULAR;
    }
    if (key.find("Opacity") != std::string::npos || key.find("Alpha") != std::string::npos) {
        return aiTextureType_OPACITY;
    }
    if (key.find("Emissive") != std::string::npos) {
        return aiTextureType_EMISSIVE;
    }
    return semantic;
}

static bool TryReadMaterialPropertyString(const aiMaterialProperty *prop, aiString &out) {
    if (nullptr == prop || aiPTI_String != prop->mType || nullptr == prop->mData || prop->mDataLength < 5) {
        return false;
    }

    const uint32_t length = *reinterpret_cast<const uint32_t *>(prop->mData);
    if (length + 5 != prop->mDataLength || length >= AI_MAXLEN) {
        return false;
    }

    out.length = length;
    std::memcpy(out.data, prop->mData + 4, length);
    out.data[length] = '\0';
    return true;
}

static std::vector<char> BuildMaterialStringData(const std::string &value) {
    aiString stringValue(value);
    std::vector<char> encoded(stringValue.length + 5, '\0');
    const uint32_t length = stringValue.length;
    std::memcpy(encoded.data(), &length, sizeof(length));
    std::memcpy(encoded.data() + sizeof(length), stringValue.C_Str(), stringValue.length);
    return encoded;
}

static std::string GetOutputDirectory(const char *filePath) {
    ai_assert(filePath != nullptr);
    std::string path(filePath);
    const std::string::size_type separator = path.find_last_of("\\/");
    if (separator == std::string::npos) {
        return ".";
    }
    if (separator == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, separator);
}

static bool EnsureDirectoryExists(const std::string &directoryPath) {
    if (directoryPath.empty()) {
        return false;
    }
    std::error_code errorCode;
    const std::filesystem::path normalizedPath = std::filesystem::u8path(directoryPath);
    if (std::filesystem::is_directory(normalizedPath, errorCode)) {
        return true;
    }

    errorCode.clear();
    return std::filesystem::create_directories(normalizedPath, errorCode) || !errorCode;
}

static std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) -> char {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return static_cast<char>(ch);
    });
    return value;
}

static bool IsAbsolutePathString(const std::string &value) {
    return (value.length() >= 2 && std::isalpha(static_cast<unsigned char>(value[0])) && value[1] == ':') ||
           StartsWith(value, "\\\\") ||
           StartsWith(value, "/") ||
           StartsWith(value, "\\");
}

static std::string TrimAsciiWhitespace(const std::string &value) {
    const std::string::size_type start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return {};
    }

    const std::string::size_type end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static std::string SanitizeTextureReference(const std::string &value) {
    std::string trimmed = TrimAsciiWhitespace(value);
    if (trimmed.empty() || StartsWith(trimmed, "*") || StartsWith(trimmed, "http://") || StartsWith(trimmed, "https://")) {
        return {};
    }

    for (std::string::size_type index = 1; index + 1 < trimmed.length(); ++index) {
        if (std::isalpha(static_cast<unsigned char>(trimmed[index - 1])) && trimmed[index] == ':' &&
                (trimmed[index + 1] == '\\' || trimmed[index + 1] == '/')) {
            return trimmed.substr(index - 1);
        }
    }

    return trimmed;
}

static std::string StripLeadingRelativeSegments(const std::string &value) {
    std::string normalized = value;
    while (StartsWith(normalized, "../") || StartsWith(normalized, "..\\") || StartsWith(normalized, "./") || StartsWith(normalized, ".\\")) {
        const std::string::size_type separator = normalized.find_first_of("\\/");
        if (separator == std::string::npos || separator + 1 >= normalized.length()) {
            return {};
        }
        normalized.erase(0, separator + 1);
    }
    return normalized;
}

static std::string NormalizePathSlashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

static bool IsRegularFilePath(const std::filesystem::path &filePath) {
    std::error_code errorCode;
    return std::filesystem::is_regular_file(filePath, errorCode);
}

static std::string BuildRelativeReferencePath(const std::string &assetRootDirectory, const std::filesystem::path &absolutePath) {
    if (assetRootDirectory.empty()) {
        return absolutePath.u8string();
    }

    const std::filesystem::path normalizedRoot = std::filesystem::u8path(assetRootDirectory).lexically_normal();
    const std::filesystem::path normalizedPath = absolutePath.lexically_normal();
    const std::filesystem::path relativePath = normalizedPath.lexically_relative(normalizedRoot);
    if (!relativePath.empty()) {
        const std::string normalizedRelative = relativePath.generic_u8string();
        if (!StartsWith(normalizedRelative, "../") && !StartsWith(normalizedRelative, "..\\")) {
            return normalizedRelative;
        }
    }

    return absolutePath.u8string();
}

static std::string NormalizeExtension(const std::string &extension);
static bool IsKnownTextureFileExtension(const std::string &extension);

static void BuildTextureAssetIndex(TextureAssetIndex &index, const std::string &assetRootDirectory) {
    index.assetRootDirectory = assetRootDirectory;
    index.entries.clear();
    index.entriesByFileName.clear();
    if (assetRootDirectory.empty()) {
        return;
    }

    std::error_code errorCode;
    const std::filesystem::path rootPath = std::filesystem::u8path(assetRootDirectory);
    if (!std::filesystem::is_directory(rootPath, errorCode)) {
        return;
    }

    std::filesystem::recursive_directory_iterator it(rootPath, std::filesystem::directory_options::skip_permission_denied, errorCode);
    const std::filesystem::recursive_directory_iterator end;
    while (!errorCode && it != end) {
        const std::filesystem::directory_entry &entry = *it;
        if (entry.is_regular_file(errorCode)) {
            const std::string extension = NormalizeExtension(entry.path().extension().u8string());
            if (IsKnownTextureFileExtension(extension)) {
                TextureAssetEntry textureEntry;
                textureEntry.absolutePath = entry.path().lexically_normal().u8string();
                textureEntry.relativePath = entry.path().lexically_relative(rootPath).generic_u8string();
                textureEntry.fileNameLower = ToLowerAscii(entry.path().filename().u8string());
                index.entries.push_back(textureEntry);
            }
        }

        errorCode.clear();
        it.increment(errorCode);
    }

    for (const TextureAssetEntry &entry : index.entries) {
        index.entriesByFileName[entry.fileNameLower].push_back(&entry);
    }
}

static uintmax_t GetTextureAssetFileSize(const TextureAssetEntry *entry) {
    ai_assert(entry != nullptr);
    if (!entry->hasFileSize) {
        std::error_code errorCode;
        entry->fileSize = std::filesystem::file_size(std::filesystem::u8path(entry->absolutePath), errorCode);
        entry->hasFileSize = !errorCode;
    }
    return entry->fileSize;
}

static uint32_t GetTextureAssetContentHash(const TextureAssetEntry *entry) {
    ai_assert(entry != nullptr);
    if (!entry->hasContentHash) {
        entry->contentHash = 0;
        std::ifstream stream(std::filesystem::u8path(entry->absolutePath), std::ios::binary);
        if (stream) {
            std::vector<char> buffer(64 * 1024);
            uint32_t hash = 0;
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize bytesRead = stream.gcount();
                if (bytesRead > 0) {
                    hash = SuperFastHash(buffer.data(), static_cast<int>(bytesRead), hash);
                }
            }
            entry->contentHash = hash;
        }
        entry->hasContentHash = true;
    }
    return entry->contentHash;
}

static const TextureAssetEntry *PickShortestRelativePathEntry(const std::vector<const TextureAssetEntry *> &entries) {
    if (entries.empty()) {
        return nullptr;
    }

    return *std::min_element(entries.begin(), entries.end(), [](const TextureAssetEntry *left, const TextureAssetEntry *right) {
        ai_assert(left != nullptr);
        ai_assert(right != nullptr);
        if (left->relativePath.length() != right->relativePath.length()) {
            return left->relativePath.length() < right->relativePath.length();
        }
        return left->relativePath < right->relativePath;
    });
}

static const TextureAssetEntry *PickEquivalentTextureEntry(const std::vector<const TextureAssetEntry *> &entries) {
    if (entries.empty()) {
        return nullptr;
    }
    if (entries.size() == 1) {
        return entries[0];
    }

    const uintmax_t expectedSize = GetTextureAssetFileSize(entries[0]);
    const uint32_t expectedHash = GetTextureAssetContentHash(entries[0]);
    for (size_t index = 1; index < entries.size(); ++index) {
        if (GetTextureAssetFileSize(entries[index]) != expectedSize) {
            return nullptr;
        }
        if (GetTextureAssetContentHash(entries[index]) != expectedHash) {
            return nullptr;
        }
    }

    return PickShortestRelativePathEntry(entries);
}

static bool TryResolveDirectTextureReference(
        const std::string &textureReference,
        const std::string &assetRootDirectory,
        ResolvedExternalTextureReference &resolvedReference) {
    std::filesystem::path candidatePath;
    if (IsAbsolutePathString(textureReference)) {
        candidatePath = std::filesystem::u8path(textureReference);
    } else {
        const std::string strippedReference = StripLeadingRelativeSegments(textureReference);
        if (strippedReference.empty()) {
            return false;
        }
        candidatePath = std::filesystem::u8path(assetRootDirectory) / std::filesystem::u8path(NormalizePathSlashes(strippedReference));
    }

    candidatePath = candidatePath.lexically_normal();
    if (!IsRegularFilePath(candidatePath)) {
        return false;
    }

    resolvedReference.absolutePath = candidatePath.u8string();
    resolvedReference.referencePath = BuildRelativeReferencePath(assetRootDirectory, candidatePath);
    return true;
}

static bool TryResolveTextureFileReference(
        const std::string &textureReference,
        const std::string &assetRootDirectory,
        TextureAssetIndex &assetIndex,
        bool &assetIndexBuilt,
        ResolvedExternalTextureReference &resolvedReference) {
    const std::string sanitizedReference = SanitizeTextureReference(textureReference);
    if (sanitizedReference.empty()) {
        return false;
    }

    if (TryResolveDirectTextureReference(sanitizedReference, assetRootDirectory, resolvedReference)) {
        return true;
    }

    const std::string fileNameLower = ToLowerAscii(DefaultIOSystem::fileName(sanitizedReference));
    if (fileNameLower.empty()) {
        return false;
    }

    if (!assetIndexBuilt) {
        BuildTextureAssetIndex(assetIndex, assetRootDirectory);
        assetIndexBuilt = true;
    }

    const auto entryIt = assetIndex.entriesByFileName.find(fileNameLower);
    if (entryIt == assetIndex.entriesByFileName.end()) {
        return false;
    }

    const TextureAssetEntry *resolvedEntry = PickEquivalentTextureEntry(entryIt->second);
    if (nullptr == resolvedEntry) {
        return false;
    }

    resolvedReference.absolutePath = resolvedEntry->absolutePath;
    resolvedReference.referencePath = resolvedEntry->relativePath;
    return true;
}

static std::string BuildExternalTextureOutputReferencePath(const std::string &resolvedReferencePath, const std::string &absolutePath) {
    const std::string normalizedReference = NormalizePathSlashes(resolvedReferencePath);
    if (normalizedReference.empty() || IsAbsolutePathString(normalizedReference)) {
        return std::string("textures/") + DefaultIOSystem::fileName(absolutePath);
    }

    std::vector<std::string> pathSegments;
    std::string currentSegment;
    for (char ch : normalizedReference) {
        if (ch == '/') {
            if (!currentSegment.empty() && currentSegment != "." && currentSegment != "..") {
                pathSegments.push_back(currentSegment);
            }
            currentSegment.clear();
            continue;
        }
        currentSegment.push_back(ch);
    }
    if (!currentSegment.empty() && currentSegment != "." && currentSegment != "..") {
        pathSegments.push_back(currentSegment);
    }

    if (pathSegments.empty()) {
        return std::string("textures/") + DefaultIOSystem::fileName(absolutePath);
    }
    if (pathSegments.front() == "textures") {
        return normalizedReference;
    }

    std::string outputReference = "textures";
    for (const std::string &segment : pathSegments) {
        outputReference += "/";
        outputReference += segment;
    }
    return outputReference;
}

static bool FilesHaveSameContent(const std::filesystem::path &leftPath, const std::filesystem::path &rightPath) {
    std::error_code errorCode;
    if (std::filesystem::file_size(leftPath, errorCode) != std::filesystem::file_size(rightPath, errorCode) || errorCode) {
        return false;
    }

    std::ifstream left(leftPath, std::ios::binary);
    std::ifstream right(rightPath, std::ios::binary);
    if (!left || !right) {
        return false;
    }

    std::vector<char> leftBuffer(64 * 1024);
    std::vector<char> rightBuffer(64 * 1024);
    while (left && right) {
        left.read(leftBuffer.data(), static_cast<std::streamsize>(leftBuffer.size()));
        right.read(rightBuffer.data(), static_cast<std::streamsize>(rightBuffer.size()));
        const std::streamsize leftRead = left.gcount();
        const std::streamsize rightRead = right.gcount();
        if (leftRead != rightRead) {
            return false;
        }
        if (leftRead <= 0) {
            break;
        }
        if (0 != std::memcmp(leftBuffer.data(), rightBuffer.data(), static_cast<size_t>(leftRead))) {
            return false;
        }
    }
    return true;
}

static std::string EnsureUniqueExternalTextureOutputPath(const std::string &absoluteSourcePath, const std::string &absoluteOutputPath) {
    const std::filesystem::path sourcePath = std::filesystem::u8path(absoluteSourcePath).lexically_normal();
    const std::filesystem::path desiredPath = std::filesystem::u8path(absoluteOutputPath).lexically_normal();
    if (sourcePath == desiredPath) {
        return desiredPath.u8string();
    }

    const std::filesystem::path parentPath = desiredPath.parent_path();
    const std::string stem = desiredPath.stem().u8string();
    const std::string extension = desiredPath.extension().u8string();

    for (unsigned int suffix = 0; ; ++suffix) {
        std::filesystem::path candidatePath = desiredPath;
        if (suffix > 0) {
            candidatePath = parentPath / std::filesystem::u8path(stem + "_" + std::to_string(suffix) + extension);
        }

        if (!IsRegularFilePath(candidatePath)) {
            return candidatePath.u8string();
        }
        if (FilesHaveSameContent(sourcePath, candidatePath)) {
            return candidatePath.u8string();
        }
    }
}

static std::string NormalizeExtension(const std::string &extension) {
    if (extension.empty()) {
        return {};
    }
    std::string normalized(extension);
    for (char &ch : normalized) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (normalized[0] != '.') {
        normalized.insert(normalized.begin(), '.');
    }
    return normalized;
}

static std::string GetExtensionFromFilename(const aiString &filename) {
    if (filename.length == 0) {
        return {};
    }

    const std::string shortFilename = DefaultIOSystem::fileName(filename.C_Str());
    const std::string::size_type dotIndex = shortFilename.find_last_of('.');
    if (dotIndex == std::string::npos || dotIndex == shortFilename.length() - 1) {
        return {};
    }
    return NormalizeExtension(shortFilename.substr(dotIndex));
}

static std::string CanonicalizeTextureFileExtension(const std::string &extension) {
    if (extension == ".jpeg") {
        return ".jpg";
    }
    if (extension == ".tiff") {
        return ".tif";
    }
    return extension;
}

static bool IsKnownTextureFileExtension(const std::string &extension) {
    static const std::set<std::string> knownTextureExtensions = {
        ".basis",
        ".bmp",
        ".dds",
        ".exr",
        ".gif",
        ".hdr",
        ".jpg",
        ".jpeg",
        ".ktx",
        ".ktx2",
        ".png",
        ".psd",
        ".tga",
        ".tif",
        ".tiff",
        ".webp"
    };
    return knownTextureExtensions.find(extension) != knownTextureExtensions.end();
}

static std::string GetExtensionFromFormatHint(const aiTexture *texture) {
    ai_assert(texture != nullptr);
    if (texture->mHeight != 0) {
        return ".tga";
    }

    std::string hint(texture->achFormatHint);
    if (hint.empty()) {
        return {};
    }

    for (char &ch : hint) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    if (hint == "jpeg") {
        hint = "jpg";
    }

    if (hint.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) {
        return {};
    }

    return NormalizeExtension(hint);
}

static bool MatchesMagic(const uint8_t *data, size_t size, const char *magic, size_t magicSize, size_t offset = 0) {
    return size >= offset + magicSize && 0 == std::memcmp(data + offset, magic, magicSize);
}

static std::string DetectCompressedTextureExtension(const aiTexture *texture) {
    ai_assert(texture != nullptr);
    if (texture->mHeight != 0 || texture->mWidth == 0 || nullptr == texture->pcData) {
        return {};
    }

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(texture->pcData);
    const size_t size = texture->mWidth;

    static const unsigned char pngMagic[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    static const unsigned char jpgMagic[] = { 0xff, 0xd8, 0xff };
    static const unsigned char gifMagic[] = { 'G', 'I', 'F', '8' };
    static const unsigned char bmpMagic[] = { 'B', 'M' };
    static const unsigned char ddsMagic[] = { 'D', 'D', 'S', ' ' };
    static const unsigned char ktxMagic[] = { 0xab, 'K', 'T', 'X', ' ', '1', '1', 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
    static const unsigned char ktx2Magic[] = { 0xab, 'K', 'T', 'X', ' ', '2', '0', 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
    static const unsigned char tiffLittleMagic[] = { 'I', 'I', 0x2a, 0x00 };
    static const unsigned char tiffBigMagic[] = { 'M', 'M', 0x00, 0x2a };

    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(pngMagic), sizeof(pngMagic))) {
        return ".png";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(jpgMagic), sizeof(jpgMagic))) {
        return ".jpg";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(gifMagic), sizeof(gifMagic))) {
        return ".gif";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(bmpMagic), sizeof(bmpMagic))) {
        return ".bmp";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(ddsMagic), sizeof(ddsMagic))) {
        return ".dds";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(ktxMagic), sizeof(ktxMagic)) ||
            MatchesMagic(bytes, size, reinterpret_cast<const char *>(ktx2Magic), sizeof(ktx2Magic))) {
        return ".ktx2";
    }
    if (MatchesMagic(bytes, size, "RIFF", 4, 0) && MatchesMagic(bytes, size, "WEBP", 4, 8)) {
        return ".webp";
    }
    if (MatchesMagic(bytes, size, reinterpret_cast<const char *>(tiffLittleMagic), sizeof(tiffLittleMagic)) ||
            MatchesMagic(bytes, size, reinterpret_cast<const char *>(tiffBigMagic), sizeof(tiffBigMagic))) {
        return ".tif";
    }
    return {};
}

static std::string DetermineTextureFileExtension(const aiTexture *texture) {
    ai_assert(texture != nullptr);
    if (texture->mHeight != 0) {
        return ".tga";
    }

    const std::string hintExtension = GetExtensionFromFormatHint(texture);
    const std::string canonicalHintExtension = CanonicalizeTextureFileExtension(hintExtension);
    if (IsKnownTextureFileExtension(canonicalHintExtension)) {
        return canonicalHintExtension;
    }

    const std::string detectedExtension = DetectCompressedTextureExtension(texture);
    if (!detectedExtension.empty()) {
        return CanonicalizeTextureFileExtension(detectedExtension);
    }

    const std::string filenameExtension = CanonicalizeTextureFileExtension(GetExtensionFromFilename(texture->mFilename));
    if (IsKnownTextureFileExtension(filenameExtension)) {
        return filenameExtension;
    }

    return ".bin";
}

static std::string GetTextureFilenameStem(const aiTexture *texture) {
    ai_assert(texture != nullptr);
    if (texture->mFilename.length == 0) {
        return {};
    }

    const std::string shortFilename = DefaultIOSystem::fileName(texture->mFilename.C_Str());
    const std::string::size_type dotIndex = shortFilename.find_last_of('.');
    if (dotIndex == std::string::npos) {
        return shortFilename;
    }
    return shortFilename.substr(0, dotIndex);
}

static std::string SanitizeTextureFilenameStem(const std::string &stem) {
    std::string sanitized;
    sanitized.reserve(stem.length());

    for (unsigned char ch : stem) {
        if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            sanitized.push_back('_');
            continue;
        }
        sanitized.push_back(static_cast<char>(ch));
    }

    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.back() = '_';
    }

    if (sanitized == "." || sanitized == "..") {
        return {};
    }

    return sanitized;
}

static std::string BuildTextureFileNameKey(const std::string &filename) {
    std::string key(filename);
    for (char &ch : key) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return key;
}

static std::string BuildTextureFileName(const aiTexture *texture, std::set<std::string> &usedTextureFileNames) {
    ai_assert(texture != nullptr);

    std::string stem = SanitizeTextureFilenameStem(GetTextureFilenameStem(texture));
    if (stem.empty()) {
        stem = "embedded";
    }

    const std::string extension = DetermineTextureFileExtension(texture);
    std::string candidate = stem + extension;
    std::string candidateKey = BuildTextureFileNameKey(candidate);
    for (unsigned int suffix = 1; usedTextureFileNames.find(candidateKey) != usedTextureFileNames.end(); ++suffix) {
        candidate = stem + "_" + std::to_string(suffix) + extension;
        candidateKey = BuildTextureFileNameKey(candidate);
    }

    usedTextureFileNames.insert(candidateKey);
    return candidate;
}

static void WriteTextureAsTga(IOStream *stream, const aiTexture *texture) {
    ai_assert(stream != nullptr);
    ai_assert(texture != nullptr);
    if (texture->mWidth > 0xffff || texture->mHeight > 0xffff) {
        throw DeadlyExportError("Unable to externalize embedded texture as TGA because dimensions exceed 16-bit limits");
    }

    unsigned char header[18] = {};
    header[2] = 2; // uncompressed true-color image
    header[12] = static_cast<unsigned char>(texture->mWidth & 0xffu);
    header[13] = static_cast<unsigned char>((texture->mWidth >> 8u) & 0xffu);
    header[14] = static_cast<unsigned char>(texture->mHeight & 0xffu);
    header[15] = static_cast<unsigned char>((texture->mHeight >> 8u) & 0xffu);
    header[16] = 32; // bits per pixel
    header[17] = 0x28; // 8-bit alpha + top-left origin

    stream->Write(header, sizeof(header), 1);
    stream->Write(texture->pcData, sizeof(aiTexel), texture->mWidth * texture->mHeight);
}

static void WriteTextureToExternalFile(IOSystem *ioSystem, const std::string &outputPath, const aiTexture *texture) {
    ai_assert(ioSystem != nullptr);
    ai_assert(texture != nullptr);

    std::unique_ptr<IOStream> output(ioSystem->Open(outputPath, "wb"));
    if (!output) {
        throw DeadlyExportError("Unable to open external texture file " + outputPath);
    }

    if (texture->mHeight == 0) {
        output->Write(texture->pcData, 1, texture->mWidth);
        return;
    }

    WriteTextureAsTga(output.get(), texture);
}

static AssbinExternalTexturePlan BuildExternalTexturePlan(const char *outputFile, IOSystem *ioSystem, const aiScene *scene) {
    ai_assert(outputFile != nullptr);
    ai_assert(ioSystem != nullptr);
    ai_assert(scene != nullptr);

    AssbinExternalTexturePlan plan;
    const char separator = ioSystem->getOsSeparator();
    const std::string outputDirectory = GetOutputDirectory(outputFile);
    const std::string textureDirectoryName = "textures";
    const std::string textureDirectoryPath = outputDirectory + separator + textureDirectoryName;
    bool textureDirectoryReady = false;
    const auto ensureTextureDirectory = [&]() {
        if (textureDirectoryReady) {
            return;
        }
        if (!EnsureDirectoryExists(textureDirectoryPath)) {
            throw DeadlyExportError("Unable to create external texture directory " + textureDirectoryPath);
        }
        textureDirectoryReady = true;
    };

    plan.externalizeTextures = scene->mNumTextures > 0;
    plan.textureReferencesByIndex.resize(scene->mNumTextures);
    std::set<std::string> usedTextureFileNames;

    for (unsigned int textureIndex = 0; textureIndex < scene->mNumTextures; ++textureIndex) {
        const aiTexture *texture = scene->mTextures[textureIndex];
        if (nullptr == texture || nullptr == texture->pcData) {
            throw DeadlyExportError("Unable to externalize embedded texture because texture data is missing");
        }

        ensureTextureDirectory();
        const std::string outputFileName = BuildTextureFileName(texture, usedTextureFileNames);
        const std::string outputPath = textureDirectoryPath + separator + outputFileName;
        WriteTextureToExternalFile(ioSystem, outputPath, texture);
        plan.textureReferencesByIndex[textureIndex] = textureDirectoryName + "/" + outputFileName;
    }

    for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex) {
        const aiMaterial *material = scene->mMaterials[materialIndex];
        if (nullptr == material) {
            continue;
        }

        std::map<TexturePropertySlotKey, TexturePropertySlotState> slots;
        for (unsigned int propertyIndex = 0; propertyIndex < material->mNumProperties; ++propertyIndex) {
            const aiMaterialProperty *property = material->mProperties[propertyIndex];
            if (nullptr == property || !IsTexturePropertyKey(property->mKey)) {
                continue;
            }

            aiString textureReference;
            if (!TryReadMaterialPropertyString(property, textureReference) || textureReference.length == 0) {
                continue;
            }

            const std::pair<const aiTexture *, int> embeddedTexture = scene->GetEmbeddedTextureAndIndex(textureReference.C_Str());
            const int embeddedIndex = embeddedTexture.first != nullptr ? embeddedTexture.second : -1;

            TexturePropertySlotKey slotKey;
            slotKey.semantic = InferTexturePropertySemantic(property);
            slotKey.index = property->mIndex;

            TexturePropertySlotState &slot = slots[slotKey];
            slot.properties.push_back(std::make_pair(property, embeddedIndex));
            if (embeddedIndex >= 0) {
                slot.embeddedIndices.insert(embeddedIndex);
            }
        }

        for (const auto &slotEntry : slots) {
            const TexturePropertySlotState &slot = slotEntry.second;
            if (slot.embeddedIndices.size() == 1) {
                const int embeddedIndex = *slot.embeddedIndices.begin();
                const std::vector<char> encodedPath = BuildMaterialStringData(plan.textureReferencesByIndex[static_cast<size_t>(embeddedIndex)]);
                for (const auto &propertyEntry : slot.properties) {
                    plan.propertyStringOverrides[propertyEntry.first] = encodedPath;
                }
                continue;
            }

            for (const auto &propertyEntry : slot.properties) {
                if (propertyEntry.second < 0) {
                    continue;
                }
                plan.propertyStringOverrides[propertyEntry.first] =
                        BuildMaterialStringData(plan.textureReferencesByIndex[static_cast<size_t>(propertyEntry.second)]);
            }
        }
    }

    std::unordered_map<std::string, std::string> copiedTextureReferencesByAbsolutePath;
    TextureAssetIndex assetIndex;
    bool assetIndexBuilt = false;
    std::error_code currentDirectoryError;
    const std::string assetRootDirectory = std::filesystem::current_path(currentDirectoryError).u8string();

    for (unsigned int materialIndex = 0; materialIndex < scene->mNumMaterials; ++materialIndex) {
        const aiMaterial *material = scene->mMaterials[materialIndex];
        if (nullptr == material) {
            continue;
        }

        for (unsigned int propertyIndex = 0; propertyIndex < material->mNumProperties; ++propertyIndex) {
            const aiMaterialProperty *property = material->mProperties[propertyIndex];
            if (nullptr == property || !IsTexturePropertyKey(property->mKey) ||
                    plan.propertyStringOverrides.find(property) != plan.propertyStringOverrides.end()) {
                continue;
            }

            aiString textureReference;
            if (!TryReadMaterialPropertyString(property, textureReference) || textureReference.length == 0) {
                continue;
            }

            if (scene->GetEmbeddedTextureAndIndex(textureReference.C_Str()).first != nullptr) {
                continue;
            }

            ResolvedExternalTextureReference resolvedReference;
            if (!TryResolveTextureFileReference(
                        textureReference.C_Str(),
                        assetRootDirectory,
                        assetIndex,
                        assetIndexBuilt,
                        resolvedReference)) {
                continue;
            }

            const auto cachedReferenceIt = copiedTextureReferencesByAbsolutePath.find(resolvedReference.absolutePath);
            if (cachedReferenceIt != copiedTextureReferencesByAbsolutePath.end()) {
                plan.propertyStringOverrides[property] = BuildMaterialStringData(cachedReferenceIt->second);
                continue;
            }

            ensureTextureDirectory();
            const std::string outputReferencePath = BuildExternalTextureOutputReferencePath(
                    resolvedReference.referencePath,
                    resolvedReference.absolutePath);
            const std::filesystem::path desiredOutputPath =
                    std::filesystem::u8path(outputDirectory) / std::filesystem::u8path(NormalizePathSlashes(outputReferencePath));
            const std::string uniqueOutputPath = EnsureUniqueExternalTextureOutputPath(
                    resolvedReference.absolutePath,
                    desiredOutputPath.u8string());
            const std::filesystem::path sourcePath = std::filesystem::u8path(resolvedReference.absolutePath).lexically_normal();
            const std::filesystem::path targetPath = std::filesystem::u8path(uniqueOutputPath).lexically_normal();
            if (sourcePath != targetPath && !IsRegularFilePath(targetPath)) {
                const std::filesystem::path targetDirectory = targetPath.parent_path();
                if (!targetDirectory.empty() && !EnsureDirectoryExists(targetDirectory.u8string())) {
                    throw DeadlyExportError("Unable to create external texture directory " + targetDirectory.u8string());
                }

                std::error_code copyError;
                std::filesystem::copy_file(sourcePath, targetPath, std::filesystem::copy_options::overwrite_existing, copyError);
                if (copyError) {
                    throw DeadlyExportError("Unable to copy external texture file " + resolvedReference.absolutePath + " to " + uniqueOutputPath);
                }
            }

            const std::string finalReferencePath =
                    targetPath.lexically_relative(std::filesystem::u8path(outputDirectory)).generic_string();
            copiedTextureReferencesByAbsolutePath[resolvedReference.absolutePath] = finalReferencePath;
            plan.propertyStringOverrides[property] = BuildMaterialStringData(finalReferencePath);
        }
    }

    return plan;
}

} // namespace

// -----------------------------------------------------------------------------------
// Serialize a vec3
template <>
inline size_t Write<aiVector3D>(IOStream *stream, const aiVector3D &v) {
    size_t t = Write<ai_real>(stream, v.x);
    t += Write<float>(stream, v.y);
    t += Write<float>(stream, v.z);

    return t;
}

// -----------------------------------------------------------------------------------
// Serialize a color value
template <>
inline size_t Write<aiColor3D>(IOStream *stream, const aiColor3D &v) {
    size_t t = Write<ai_real>(stream, v.r);
    t += Write<float>(stream, v.g);
    t += Write<float>(stream, v.b);

    return t;
}

// -----------------------------------------------------------------------------------
// Serialize a color value
template <>
inline size_t Write<aiColor4D>(IOStream *stream, const aiColor4D &v) {
    size_t t = Write<ai_real>(stream, v.r);
    t += Write<float>(stream, v.g);
    t += Write<float>(stream, v.b);
    t += Write<float>(stream, v.a);

    return t;
}

// -----------------------------------------------------------------------------------
// Serialize a quaternion
template <>
inline size_t Write<aiQuaternion>(IOStream *stream, const aiQuaternion &v) {
    size_t t = Write<ai_real>(stream, v.w);
    t += Write<float>(stream, v.x);
    t += Write<float>(stream, v.y);
    t += Write<float>(stream, v.z);
    ai_assert(t == 16);

    return t;
}

// -----------------------------------------------------------------------------------
// Serialize a vertex weight
template <>
inline size_t Write<aiVertexWeight>(IOStream *stream, const aiVertexWeight &v) {
    size_t t = Write<unsigned int>(stream, v.mVertexId);

    return t + Write<float>(stream, v.mWeight);
}

constexpr size_t MatrixSize = 64;

// -----------------------------------------------------------------------------------
// Serialize a mat4x4
template <>
inline size_t Write<aiMatrix4x4>(IOStream *stream, const aiMatrix4x4 &m) {
    for (unsigned int i = 0; i < 4; ++i) {
        for (unsigned int i2 = 0; i2 < 4; ++i2) {
            Write<ai_real>(stream, m[i][i2]);
        }
    }

    return MatrixSize;
}

// -----------------------------------------------------------------------------------
// Serialize an aiVectorKey
template <>
inline size_t Write<aiVectorKey>(IOStream *stream, const aiVectorKey &v) {
    const size_t t = Write<double>(stream, v.mTime);
    return t + Write<aiVector3D>(stream, v.mValue);
}

// -----------------------------------------------------------------------------------
// Serialize an aiQuatKey
template <>
inline size_t Write<aiQuatKey>(IOStream *stream, const aiQuatKey &v) {
    const size_t t = Write<double>(stream, v.mTime);
    return t + Write<aiQuaternion>(stream, v.mValue);
}

template <typename T>
inline size_t WriteBounds(IOStream *stream, const T *in, unsigned int size) {
    T minc, maxc;
    ArrayBounds(in, size, minc, maxc);

    const size_t t = Write<T>(stream, minc);
    return t + Write<T>(stream, maxc);
}

// We use this to write out non-byte arrays so that we write using the specializations.
// This way we avoid writing out extra bytes that potentially come from struct alignment.
template <typename T>
inline size_t WriteArray(IOStream *stream, const T *in, unsigned int size) {
    size_t n = 0;
    for (unsigned int i = 0; i < size; i++)
        n += Write<T>(stream, in[i]);

    return n;
}

// ----------------------------------------------------------------------------------
/** @class  AssbinChunkWriter
 *  @brief  Chunk writer mechanism for the .assbin file structure
 *
 *  This is a standard in-memory IOStream (most of the code is based on BlobIOStream),
 *  the difference being that this takes another IOStream as a "container" in the
 *  constructor, and when it is destroyed, it appends the magic number, the chunk size,
 *  and the chunk contents to the container stream. This allows relatively easy chunk
 *  chunk construction, even recursively.
 */
class AssbinChunkWriter final : public IOStream {
public:
    AssbinChunkWriter(IOStream *container, uint32_t magic, size_t initial = 4096) :
            buffer(nullptr),
            magic(magic),
            container(container),
            cur_size(0),
            cursor(0),
            initial(initial) {
        // empty
    }

    ~AssbinChunkWriter() override {
        if (container) {
            container->Write(&magic, sizeof(uint32_t), 1);
            container->Write(&cursor, sizeof(uint32_t), 1);
            container->Write(buffer, 1, cursor);
        }
        if (buffer) delete[] buffer;
    }

    void *GetBufferPointer() { return buffer; }

    size_t Read(void * /*pvBuffer*/, size_t /*pSize*/, size_t /*pCount*/) override {
        return 0;
    }

    aiReturn Seek(size_t /*pOffset*/, aiOrigin /*pOrigin*/) override {
        return aiReturn_FAILURE;
    }

    size_t Tell() const override {
        return cursor;
    }

    void Flush() override {
        // not implemented
    }

    size_t FileSize() const override {
        return cursor;
    }

    size_t Write(const void *pvBuffer, size_t pSize, size_t pCount) override {
        pSize *= pCount;
        if (cursor + pSize > cur_size) {
            Grow(cursor + pSize);
        }

        memcpy(buffer + cursor, pvBuffer, pSize);
        cursor += pSize;

        return pCount;
    }

private:
    // -------------------------------------------------------------------
    void Grow(size_t need = 0) {
        size_t new_size = std::max(initial, std::max(need, cur_size + (cur_size >> 1)));

        const uint8_t *const old = buffer;
        buffer = new uint8_t[new_size];

        if (old) {
            memcpy(buffer, old, cur_size);
            delete[] old;
        }

        cur_size = new_size;
    }

private:
    uint8_t *buffer;
    uint32_t magic;
    IOStream *container;
    size_t cur_size, cursor, initial;
};

// ----------------------------------------------------------------------------------
/** @class  AssbinFileWriter
 *  @brief  Assbin file writer class
 *
 *  This class writes an .assbin file, and is responsible for the file layout.
 */
class AssbinFileWriter {
private:
    bool shortened;
    bool compressed;
    AssbinExternalTexturePlan externalTexturePlan;

protected:
    // -----------------------------------------------------------------------------------
    void WriteBinaryNode(IOStream *container, const aiNode *node) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AINODE);

        unsigned int nb_metadata = (node->mMetaData != nullptr ? node->mMetaData->mNumProperties : 0);

        Write<aiString>(&chunk, node->mName);
        Write<aiMatrix4x4>(&chunk, node->mTransformation);
        Write<unsigned int>(&chunk, node->mNumChildren);
        Write<unsigned int>(&chunk, node->mNumMeshes);
        Write<unsigned int>(&chunk, nb_metadata);

        for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
            Write<unsigned int>(&chunk, node->mMeshes[i]);
        }

        for (unsigned int i = 0; i < node->mNumChildren; ++i) {
            WriteBinaryNode(&chunk, node->mChildren[i]);
        }

        for (unsigned int i = 0; i < nb_metadata; ++i) {
            const aiString &key = node->mMetaData->mKeys[i];
            aiMetadataType type = node->mMetaData->mValues[i].mType;
            void *value = node->mMetaData->mValues[i].mData;

            Write<aiString>(&chunk, key);
            Write<uint16_t>(&chunk, (uint16_t)type);

            switch (type) {
            case AI_BOOL:
                Write<bool>(&chunk, *((bool *)value));
                break;
            case AI_INT32:
                Write<int32_t>(&chunk, *((int32_t *)value));
                break;
            case AI_UINT64:
                Write<uint64_t>(&chunk, *((uint64_t *)value));
                break;
            case AI_FLOAT:
                Write<float>(&chunk, *((float *)value));
                break;
            case AI_DOUBLE:
                Write<double>(&chunk, *((double *)value));
                break;
            case AI_AISTRING:
                Write<aiString>(&chunk, *((aiString *)value));
                break;
            case AI_AIVECTOR3D:
                Write<aiVector3D>(&chunk, *((aiVector3D *)value));
                break;
#ifdef SWIG
                case FORCE_32BIT:
#endif // SWIG
            default:
                break;
            }
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryTexture(IOStream *container, const aiTexture *tex) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AITEXTURE);

        Write<unsigned int>(&chunk, tex->mWidth);
        Write<unsigned int>(&chunk, tex->mHeight);
        // Write the texture format, but don't include the null terminator.
        chunk.Write(tex->achFormatHint, sizeof(char), HINTMAXTEXTURELEN - 1);

        if (!shortened) {
            if (!tex->mHeight) {
                chunk.Write(tex->pcData, 1, tex->mWidth);
            } else {
                chunk.Write(tex->pcData, 1, tex->mWidth * tex->mHeight * 4);
            }
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryBone(IOStream *container, const aiBone *b) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AIBONE);

        Write<aiString>(&chunk, b->mName);
        Write<unsigned int>(&chunk, b->mNumWeights);
        Write<aiMatrix4x4>(&chunk, b->mOffsetMatrix);

        // for the moment we write dumb min/max values for the bones, too.
        // maybe I'll add a better, hash-like solution later
        if (shortened) {
            WriteBounds(&chunk, b->mWeights, b->mNumWeights);
        } // else write as usual
        else
            WriteArray<aiVertexWeight>(&chunk, b->mWeights, b->mNumWeights);
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryMesh(IOStream *container, const aiMesh *mesh) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AIMESH);

        Write<unsigned int>(&chunk, mesh->mPrimitiveTypes);
        Write<unsigned int>(&chunk, mesh->mNumVertices);
        Write<unsigned int>(&chunk, mesh->mNumFaces);
        Write<unsigned int>(&chunk, mesh->mNumBones);
        Write<unsigned int>(&chunk, mesh->mMaterialIndex);

        // first of all, write bits for all existent vertex components
        unsigned int c = 0;
        if (mesh->mVertices) {
            c |= ASSBIN_MESH_HAS_POSITIONS;
        }
        if (mesh->mNormals) {
            c |= ASSBIN_MESH_HAS_NORMALS;
        }
        if (mesh->mTangents && mesh->mBitangents) {
            c |= ASSBIN_MESH_HAS_TANGENTS_AND_BITANGENTS;
        }
        for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++n) {
            if (!mesh->mTextureCoords[n]) {
                break;
            }
            c |= ASSBIN_MESH_HAS_TEXCOORD(n);
        }
        for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_COLOR_SETS; ++n) {
            if (!mesh->mColors[n]) {
                break;
            }
            c |= ASSBIN_MESH_HAS_COLOR(n);
        }
        Write<unsigned int>(&chunk, c);

        aiVector3D minVec, maxVec;
        if (mesh->mVertices) {
            if (shortened) {
                WriteBounds(&chunk, mesh->mVertices, mesh->mNumVertices);
            } // else write as usual
            else
                WriteArray<aiVector3D>(&chunk, mesh->mVertices, mesh->mNumVertices);
        }
        if (mesh->mNormals) {
            if (shortened) {
                WriteBounds(&chunk, mesh->mNormals, mesh->mNumVertices);
            } // else write as usual
            else
                WriteArray<aiVector3D>(&chunk, mesh->mNormals, mesh->mNumVertices);
        }
        if (mesh->mTangents && mesh->mBitangents) {
            if (shortened) {
                WriteBounds(&chunk, mesh->mTangents, mesh->mNumVertices);
                WriteBounds(&chunk, mesh->mBitangents, mesh->mNumVertices);
            } // else write as usual
            else {
                WriteArray<aiVector3D>(&chunk, mesh->mTangents, mesh->mNumVertices);
                WriteArray<aiVector3D>(&chunk, mesh->mBitangents, mesh->mNumVertices);
            }
        }
        for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_COLOR_SETS; ++n) {
            if (!mesh->mColors[n])
                break;

            if (shortened) {
                WriteBounds(&chunk, mesh->mColors[n], mesh->mNumVertices);
            } // else write as usual
            else
                WriteArray<aiColor4D>(&chunk, mesh->mColors[n], mesh->mNumVertices);
        }
        for (unsigned int n = 0; n < AI_MAX_NUMBER_OF_TEXTURECOORDS; ++n) {
            if (!mesh->mTextureCoords[n])
                break;

            // write number of UV components
            Write<unsigned int>(&chunk, mesh->mNumUVComponents[n]);

            if (shortened) {
                WriteBounds(&chunk, mesh->mTextureCoords[n], mesh->mNumVertices);
            } // else write as usual
            else
                WriteArray<aiVector3D>(&chunk, mesh->mTextureCoords[n], mesh->mNumVertices);
        }

        // write faces. There are no floating-point calculations involved
        // in these, so we can write a simple hash over the face data
        // to the dump file. We generate a single 32 Bit hash for 512 faces
        // using Assimp's standard hashing function.
        if (shortened) {
            unsigned int processed = 0;
            for (unsigned int job; (job = std::min(mesh->mNumFaces - processed, 512u)); processed += job) {
                uint32_t hash = 0;
                for (unsigned int a = 0; a < job; ++a) {

                    const aiFace &f = mesh->mFaces[processed + a];
                    uint32_t tmp = f.mNumIndices;
                    hash = SuperFastHash(reinterpret_cast<const char *>(&tmp), sizeof tmp, hash);
                    for (unsigned int i = 0; i < f.mNumIndices; ++i) {
                        static_assert(AI_MAX_VERTICES <= 0xffffffff, "AI_MAX_VERTICES <= 0xffffffff");
                        tmp = static_cast<uint32_t>(f.mIndices[i]);
                        hash = SuperFastHash(reinterpret_cast<const char *>(&tmp), sizeof tmp, hash);
                    }
                }
                Write<unsigned int>(&chunk, hash);
            }
        } else // else write as usual
        {
            // if there are less than 2^16 vertices, we can simply use 16 bit integers ...
            for (unsigned int i = 0; i < mesh->mNumFaces; ++i) {
                const aiFace &f = mesh->mFaces[i];

                static_assert(AI_MAX_FACE_INDICES <= 0xffff, "AI_MAX_FACE_INDICES <= 0xffff");
                Write<uint16_t>(&chunk, static_cast<uint16_t>(f.mNumIndices));

                for (unsigned int a = 0; a < f.mNumIndices; ++a) {
                    if (mesh->mNumVertices < (1u << 16)) {
                        Write<uint16_t>(&chunk, static_cast<uint16_t>(f.mIndices[a]));
                    } else {
                        Write<unsigned int>(&chunk, f.mIndices[a]);
                    }
                }
            }
        }

        // write bones
        if (mesh->mNumBones) {
            for (unsigned int a = 0; a < mesh->mNumBones; ++a) {
                const aiBone *b = mesh->mBones[a];
                WriteBinaryBone(&chunk, b);
            }
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryMaterialProperty(IOStream *container, const aiMaterialProperty *prop) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AIMATERIALPROPERTY);

        Write<aiString>(&chunk, prop->mKey);
        Write<unsigned int>(&chunk, prop->mSemantic);
        Write<unsigned int>(&chunk, prop->mIndex);

        const auto overrideIt = externalTexturePlan.propertyStringOverrides.find(prop);
        if (overrideIt != externalTexturePlan.propertyStringOverrides.end()) {
            const std::vector<char> &encodedValue = overrideIt->second;
            Write<unsigned int>(&chunk, static_cast<unsigned int>(encodedValue.size()));
            Write<unsigned int>(&chunk, static_cast<unsigned int>(aiPTI_String));
            chunk.Write(encodedValue.data(), 1, encodedValue.size());
            return;
        }

        Write<unsigned int>(&chunk, prop->mDataLength);
        Write<unsigned int>(&chunk, (unsigned int)prop->mType);
        chunk.Write(prop->mData, 1, prop->mDataLength);
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryMaterial(IOStream *container, const aiMaterial *mat) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AIMATERIAL);

        Write<unsigned int>(&chunk, mat->mNumProperties);
        for (unsigned int i = 0; i < mat->mNumProperties; ++i) {
            WriteBinaryMaterialProperty(&chunk, mat->mProperties[i]);
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryNodeAnim(IOStream *container, const aiNodeAnim *nd) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AINODEANIM);

        Write<aiString>(&chunk, nd->mNodeName);
        Write<unsigned int>(&chunk, nd->mNumPositionKeys);
        Write<unsigned int>(&chunk, nd->mNumRotationKeys);
        Write<unsigned int>(&chunk, nd->mNumScalingKeys);
        Write<unsigned int>(&chunk, nd->mPreState);
        Write<unsigned int>(&chunk, nd->mPostState);

        if (nd->mPositionKeys) {
            if (shortened) {
                WriteBounds(&chunk, nd->mPositionKeys, nd->mNumPositionKeys);

            } // else write as usual
            else
                WriteArray<aiVectorKey>(&chunk, nd->mPositionKeys, nd->mNumPositionKeys);
        }
        if (nd->mRotationKeys) {
            if (shortened) {
                WriteBounds(&chunk, nd->mRotationKeys, nd->mNumRotationKeys);

            } // else write as usual
            else
                WriteArray<aiQuatKey>(&chunk, nd->mRotationKeys, nd->mNumRotationKeys);
        }
        if (nd->mScalingKeys) {
            if (shortened) {
                WriteBounds(&chunk, nd->mScalingKeys, nd->mNumScalingKeys);

            } // else write as usual
            else
                WriteArray<aiVectorKey>(&chunk, nd->mScalingKeys, nd->mNumScalingKeys);
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryAnim(IOStream *container, const aiAnimation *anim) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AIANIMATION);

        Write<aiString>(&chunk, anim->mName);
        Write<double>(&chunk, anim->mDuration);
        Write<double>(&chunk, anim->mTicksPerSecond);
        Write<unsigned int>(&chunk, anim->mNumChannels);

        for (unsigned int a = 0; a < anim->mNumChannels; ++a) {
            const aiNodeAnim *nd = anim->mChannels[a];
            WriteBinaryNodeAnim(&chunk, nd);
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryLight(IOStream *container, const aiLight *l) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AILIGHT);

        Write<aiString>(&chunk, l->mName);
        Write<unsigned int>(&chunk, l->mType);

        Write<aiVector3D>(&chunk, l->mPosition);
        Write<aiVector3D>(&chunk, l->mDirection);
        Write<aiVector3D>(&chunk, l->mUp);

        if (l->mType != aiLightSource_DIRECTIONAL) {
            Write<float>(&chunk, l->mAttenuationConstant);
            Write<float>(&chunk, l->mAttenuationLinear);
            Write<float>(&chunk, l->mAttenuationQuadratic);
        }

        Write<aiColor3D>(&chunk, l->mColorDiffuse);
        Write<aiColor3D>(&chunk, l->mColorSpecular);
        Write<aiColor3D>(&chunk, l->mColorAmbient);

        if (l->mType == aiLightSource_SPOT) {
            Write<float>(&chunk, l->mAngleInnerCone);
            Write<float>(&chunk, l->mAngleOuterCone);
        }
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryCamera(IOStream *container, const aiCamera *cam) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AICAMERA);

        Write<aiString>(&chunk, cam->mName);
        Write<aiVector3D>(&chunk, cam->mPosition);
        Write<aiVector3D>(&chunk, cam->mLookAt);
        Write<aiVector3D>(&chunk, cam->mUp);
        Write<float>(&chunk, cam->mHorizontalFOV);
        Write<float>(&chunk, cam->mClipPlaneNear);
        Write<float>(&chunk, cam->mClipPlaneFar);
        Write<float>(&chunk, cam->mAspect);
    }

    // -----------------------------------------------------------------------------------
    void WriteBinaryScene(IOStream *container, const aiScene *scene) {
        AssbinChunkWriter chunk(container, ASSBIN_CHUNK_AISCENE);

        // basic scene information
        Write<unsigned int>(&chunk, scene->mFlags);
        Write<unsigned int>(&chunk, scene->mNumMeshes);
        Write<unsigned int>(&chunk, scene->mNumMaterials);
        Write<unsigned int>(&chunk, scene->mNumAnimations);
        Write<unsigned int>(&chunk, externalTexturePlan.externalizeTextures ? 0u : scene->mNumTextures);
        Write<unsigned int>(&chunk, scene->mNumLights);
        Write<unsigned int>(&chunk, scene->mNumCameras);

        // write node graph
        WriteBinaryNode(&chunk, scene->mRootNode);

        // write all meshes
        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh *mesh = scene->mMeshes[i];
            WriteBinaryMesh(&chunk, mesh);
        }

        // write materials
        for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
            const aiMaterial *mat = scene->mMaterials[i];
            WriteBinaryMaterial(&chunk, mat);
        }

        // write all animations
        for (unsigned int i = 0; i < scene->mNumAnimations; ++i) {
            const aiAnimation *anim = scene->mAnimations[i];
            WriteBinaryAnim(&chunk, anim);
        }

        // write all textures
        if (!externalTexturePlan.externalizeTextures) {
            for (unsigned int i = 0; i < scene->mNumTextures; ++i) {
                const aiTexture *mesh = scene->mTextures[i];
                WriteBinaryTexture(&chunk, mesh);
            }
        }

        // write lights
        for (unsigned int i = 0; i < scene->mNumLights; ++i) {
            const aiLight *l = scene->mLights[i];
            WriteBinaryLight(&chunk, l);
        }

        // write cameras
        for (unsigned int i = 0; i < scene->mNumCameras; ++i) {
            const aiCamera *cam = scene->mCameras[i];
            WriteBinaryCamera(&chunk, cam);
        }
    }

public:
    AssbinFileWriter(bool shortened, bool compressed) :
            shortened(shortened), compressed(compressed) {
    }

    // -----------------------------------------------------------------------------------
    // Write a binary model dump
    void WriteBinaryDump(const char *pFile, const char *cmd, IOSystem *pIOSystem, const aiScene *pScene) {
        externalTexturePlan = BuildExternalTexturePlan(pFile, pIOSystem, pScene);

        IOStream *out = pIOSystem->Open(pFile, "wb");
        if (!out)
            throw std::runtime_error("Unable to open output file " + std::string(pFile) + '\n');

        auto CloseIOStream = [&]() {
            if (out) {
                pIOSystem->Close(out);
                out = nullptr; // Ensure this is only done once.
            }
        };

        try {
            time_t tt = time(nullptr);
#if _WIN32
            tm *p = gmtime(&tt);
#else
            struct tm now;
            tm *p = gmtime_r(&tt, &now);
#endif

            // header
            char s[64];
            memset(s, 0, 64);
#if _MSC_VER >= 1400
            sprintf_s(s, "ASSIMP.binary-dump.%s", asctime(p));
#else
            ai_snprintf(s, 64, "ASSIMP.binary-dump.%s", asctime(p));
#endif
            out->Write(s, 44, 1);
            // == 44 bytes

            Write<unsigned int>(out, ASSBIN_VERSION_MAJOR);
            Write<unsigned int>(out, ASSBIN_VERSION_MINOR);
            Write<unsigned int>(out, aiGetVersionRevision());
            Write<unsigned int>(out, aiGetCompileFlags());
            Write<uint16_t>(out, shortened);
            Write<uint16_t>(out, compressed);
            // ==  20 bytes

            char buff[256] = { 0 };
            ai_snprintf(buff, 256, "%s", pFile);
            out->Write(buff, sizeof(char), 256);

            memset(buff, 0, sizeof(buff));
            ai_snprintf(buff, 128, "%s", cmd);
            out->Write(buff, sizeof(char), 128);

            // leave 64 bytes free for future extensions
            memset(buff, 0xcd, 64);
            out->Write(buff, sizeof(char), 64);
            // == 435 bytes

            // ==== total header size: 512 bytes
            ai_assert(out->Tell() == ASSBIN_HEADER_LENGTH);

            // Up to here the data is uncompressed. For compressed files, the rest
            // is compressed using standard DEFLATE from zlib.
            if (compressed) {
                AssbinChunkWriter uncompressedStream(nullptr, 0);
                WriteBinaryScene(&uncompressedStream, pScene);

                uLongf uncompressedSize = static_cast<uLongf>(uncompressedStream.Tell());
                uLongf compressedSize = (uLongf)compressBound(uncompressedSize);
                uint8_t *compressedBuffer = new uint8_t[compressedSize];

                int res = compress2(compressedBuffer, &compressedSize, (const Bytef *)uncompressedStream.GetBufferPointer(), uncompressedSize, 9);
                if (res != Z_OK) {
                    delete[] compressedBuffer;
                    throw DeadlyExportError("Compression failed.");
                }

                out->Write(&uncompressedSize, sizeof(uint32_t), 1);
                out->Write(compressedBuffer, sizeof(char), compressedSize);

                delete[] compressedBuffer;
            } else {
                WriteBinaryScene(out, pScene);
            }

            CloseIOStream();
        } catch (...) {
            CloseIOStream();
            throw;
        }
    }
};

void DumpSceneToAssbin(
        const char *pFile, const char *cmd, IOSystem *pIOSystem,
        const aiScene *pScene, bool shortened, bool compressed) {
    AssbinFileWriter fileWriter(shortened, compressed);
    fileWriter.WriteBinaryDump(pFile, cmd, pIOSystem, pScene);
}
#if _MSC_VER
#pragma warning(pop)
#endif // _MSC_VER

} // end of namespace Assimp
