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

/** @file  FBXMaterial.cpp
 *  @brief Assimp::FBX::Material and Assimp::FBX::Texture implementation
 */

#ifndef ASSIMP_BUILD_NO_FBX_IMPORTER

#include "FBXParser.h"
#include "FBXDocument.h"
#include "FBXImporter.h"
#include "FBXImportSettings.h"
#include "FBXDocumentUtil.h"
#include "FBXProperties.h"
#include <assimp/Exceptional.h>
#include <assimp/ByteSwapper.h>
#include <assimp/ParsingUtils.h>

#include "FBXUtil.h"

#include <array>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <cerrno>
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace Assimp {
namespace FBX {

using namespace Util;

namespace {

static std::string ExtractFileName(const std::string &path) {
    const size_t lastSeparator = path.find_last_of("/\\");
    if (lastSeparator == std::string::npos) {
        return path;
    }
    return path.substr(lastSeparator + 1);
}

static std::string ExtractFileStem(const std::string &path) {
    std::string fileName = ExtractFileName(path);
    const size_t lastDot = fileName.find_last_of('.');
    if (lastDot == std::string::npos || lastDot == 0) {
        return fileName;
    }
    return fileName.substr(0, lastDot);
}

static std::string ParentDirectory(const std::string &path) {
    const size_t lastSeparator = path.find_last_of("/\\");
    if (lastSeparator == std::string::npos) {
        return {};
    }
    if (lastSeparator == 0) {
        return path.substr(0, 1);
    }
    return path.substr(0, lastSeparator);
}

static std::string JoinPath(const std::string &left, const std::string &right) {
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/' || left.back() == '\\') {
        return left + right;
    }
    return left + "/" + right;
}

static std::string NormalizeTextureExtension(const std::string &fileName) {
    if (fileName.empty()) {
        return ".bin";
    }

    const std::string baseName = ExtractFileName(fileName);
    const size_t lastDot = baseName.find_last_of('.');
    std::string extension = lastDot == std::string::npos ? std::string() : baseName.substr(lastDot);
    if (extension.empty()) {
        return ".bin";
    }

    for (char &ch : extension) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }

    if (extension == ".jpeg") {
        return ".jpg";
    }
    if (extension == ".tiff") {
        return ".tif";
    }
    return extension;
}

static std::string SanitizeTextureFileStem(const std::string &value) {
    std::string stem = value.empty() ? std::string() : ExtractFileStem(value);

    std::string sanitized;
    sanitized.reserve(stem.length());
    for (unsigned char ch : stem) {
        if (ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            sanitized.push_back('_');
        } else {
            sanitized.push_back(static_cast<char>(ch));
        }
    }

    while (!sanitized.empty() && (sanitized.back() == ' ' || sanitized.back() == '.')) {
        sanitized.back() = '_';
    }

    if (sanitized.empty() || sanitized == "." || sanitized == "..") {
        sanitized = "embedded";
    }
    return sanitized;
}

static std::string BuildEmbeddedTextureSpillPath(
        const std::string &spillDirectory,
        uint64_t id,
        const std::string &relativeFileName,
        const std::string &fileName) {
    const std::string preferredName = relativeFileName.empty() ? fileName : relativeFileName;
    const std::string stem = SanitizeTextureFileStem(preferredName);
    const std::string extension = NormalizeTextureExtension(preferredName);
    const std::string outputFileName = stem + "_" + std::to_string(id) + extension;
    return JoinPath(spillDirectory, outputFileName);
}

static void EnsureEmbeddedTextureSpillDirectoryExists(const std::string &spillDirectory) {
#ifdef _WIN32
    const std::filesystem::path fsSpillDirectory = std::filesystem::u8path(spillDirectory);
    std::error_code errorCode;
    if (std::filesystem::is_directory(fsSpillDirectory, errorCode)) {
        return;
    }

    errorCode.clear();
    if (!std::filesystem::create_directories(fsSpillDirectory, errorCode) && errorCode) {
        throw DeadlyImportError("Unable to create embedded FBX texture spill directory: " + spillDirectory);
    }
#else
    if (spillDirectory.empty() || spillDirectory == "/") {
        return;
    }

    std::string partial;
    if (spillDirectory.front() == '/') {
        partial = "/";
    }

    size_t segmentStart = spillDirectory.front() == '/' ? 1 : 0;
    while (segmentStart <= spillDirectory.size()) {
        const size_t separator = spillDirectory.find('/', segmentStart);
        const std::string segment = spillDirectory.substr(
                segmentStart,
                separator == std::string::npos ? std::string::npos : separator - segmentStart);
        if (!segment.empty()) {
            partial = JoinPath(partial, segment);
            if (::mkdir(partial.c_str(), 0777) != 0 && errno != EEXIST) {
                throw DeadlyImportError("Unable to create embedded FBX texture spill directory: " + spillDirectory);
            }
        }

        if (separator == std::string::npos) {
            break;
        }
        segmentStart = separator + 1;
    }
#endif
}

static uint64_t DecodeBase64ToBinaryStream(const char *in, size_t inLength, std::ofstream &output) {
    if (inLength < 2) {
        return 0;
    }

    const size_t realLength = inLength - size_t(in[inLength - 1] == '=') - size_t(in[inLength - 2] == '=');
    std::array<char, 16 * 1024> buffer{};
    size_t bufferedBytes = 0;
    uint64_t totalBytes = 0;
    int value = 0;
    int valueBits = -8;

    for (size_t index = 0; index < realLength; ++index) {
        const uint8_t decoded = Util::DecodeBase64(in[index]);
        if (decoded == 255) {
            return 0;
        }

        value = (value << 6) + decoded;
        valueBits += 6;
        if (valueBits >= 0) {
            buffer[bufferedBytes++] = static_cast<char>((value >> valueBits) & 0xFF);
            valueBits -= 8;
            value &= 0xFFF;

            if (bufferedBytes == buffer.size()) {
                output.write(buffer.data(), static_cast<std::streamsize>(bufferedBytes));
                if (!output) {
                    throw DeadlyImportError("Unable to spill embedded FBX texture payload to disk");
                }
                totalBytes += bufferedBytes;
                bufferedBytes = 0;
            }
        }
    }

    if (bufferedBytes > 0) {
        output.write(buffer.data(), static_cast<std::streamsize>(bufferedBytes));
        if (!output) {
            throw DeadlyImportError("Unable to spill embedded FBX texture payload to disk");
        }
        totalBytes += bufferedBytes;
    }

    return totalBytes;
}

static void WriteBinaryBufferToStream(const uint8_t *data, uint64_t dataLength, std::ofstream &output) {
    ai_assert(data != nullptr || dataLength == 0);

    constexpr uint64_t kChunkSize = 16ull * 1024ull * 1024ull;
    uint64_t writtenBytes = 0;
    while (writtenBytes < dataLength) {
        const uint64_t remainingBytes = dataLength - writtenBytes;
        const uint64_t chunkBytes = remainingBytes < kChunkSize ? remainingBytes : kChunkSize;
        output.write(
                reinterpret_cast<const char *>(data + writtenBytes),
                static_cast<std::streamsize>(chunkBytes));
        if (!output) {
            throw DeadlyImportError("Unable to spill embedded FBX texture payload to disk");
        }
        writtenBytes += chunkBytes;
    }
}

static std::string SpillEmbeddedVideoBufferToFile(
        uint64_t id,
        const std::string &spillDirectory,
        const std::string &relativeFileName,
        const std::string &fileName,
        const uint8_t *content,
        uint64_t contentLength) {
    const std::string outputPath = BuildEmbeddedTextureSpillPath(spillDirectory, id, relativeFileName, fileName);
    EnsureEmbeddedTextureSpillDirectoryExists(ParentDirectory(outputPath));

#ifdef _WIN32
    std::ofstream output(std::filesystem::u8path(outputPath), std::ios::binary | std::ios::trunc);
#else
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
#endif
    if (!output) {
        throw DeadlyImportError("Unable to open embedded FBX texture spill file: " + outputPath);
    }

    try {
        WriteBinaryBufferToStream(content, contentLength, output);
    } catch (...) {
        output.close();
        std::remove(outputPath.c_str());
        throw;
    }

    output.close();
    if (!output) {
        std::remove(outputPath.c_str());
        throw DeadlyImportError("Unable to finalize embedded FBX texture spill file: " + outputPath);
    }

    return outputPath;
}

static std::string SpillEmbeddedVideoContentToFile(
        uint64_t id,
        const std::string &spillDirectory,
        const std::string &relativeFileName,
        const std::string &fileName,
        const Element &contentElement,
        const Element &element) {
    const std::string outputPath = BuildEmbeddedTextureSpillPath(spillDirectory, id, relativeFileName, fileName);
    EnsureEmbeddedTextureSpillDirectoryExists(ParentDirectory(outputPath));

#ifdef _WIN32
    std::ofstream output(std::filesystem::u8path(outputPath), std::ios::binary | std::ios::trunc);
#else
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
#endif
    if (!output) {
        throw DeadlyImportError("Unable to open embedded FBX texture spill file: " + outputPath);
    }

    try {
        const Token &token = GetRequiredToken(contentElement, 0);
        const char *data = token.begin();

        if (!token.IsBinary()) {
            uint64_t writtenBytes = 0;
            const size_t numTokens = contentElement.Tokens().size();
            for (size_t tokenIdx = 0; tokenIdx < numTokens; ++tokenIdx) {
                const Token &dataToken = GetRequiredToken(contentElement, static_cast<unsigned int>(tokenIdx));
                const size_t tokenLength = static_cast<size_t>(dataToken.end() - dataToken.begin());
                if (tokenLength < 2 || dataToken.begin()[0] != '"' || dataToken.end()[-1] != '"') {
                    DOMError("embedded content is not surrounded by quotation marks", &element);
                }

                const char *base64data = dataToken.begin() + 1;
                const size_t base64Length = tokenLength - 2;
                const size_t decodedSize = Util::ComputeDecodedSizeBase64(base64data, base64Length);
                if (decodedSize == 0) {
                    DOMError("Corrupted embedded content found", &element);
                }

                const uint64_t decodedBytes = DecodeBase64ToBinaryStream(base64data, base64Length, output);
                if (decodedBytes != decodedSize) {
                    DOMError("Corrupted embedded content found", &element);
                }
                writtenBytes += decodedBytes;
            }

            if (writtenBytes == 0) {
                DOMError("Corrupted embedded content found", &element);
            }
        } else if (static_cast<size_t>(token.end() - data) < 5) {
            DOMError("binary data array is too short, need five (5) bytes for type signature and element count", &element);
        } else if (*data != 'R') {
            DOMWarning("video content is not raw binary data, ignoring", &element);
            return {};
        } else {
            uint32_t len = 0;
            ::memcpy(&len, data + 1, sizeof(len));
            AI_SWAP4(len);
            output.write(data + 5, static_cast<std::streamsize>(len));
            if (!output) {
                throw DeadlyImportError("Unable to spill embedded FBX texture payload to disk");
            }
        }
    } catch (...) {
        output.close();
        std::remove(outputPath.c_str());
        throw;
    }

    output.close();
    if (!output) {
        std::remove(outputPath.c_str());
        throw DeadlyImportError("Unable to finalize embedded FBX texture spill file: " + outputPath);
    }

    return outputPath;
}

} // namespace

// ------------------------------------------------------------------------------------------------
Material::Material(uint64_t id, const Element& element, const Document& doc, const std::string& name) :
        Object(id,element,name) {
    const Scope& sc = GetRequiredScope(element);

    const Element* const ShadingModel = sc["ShadingModel"];
    const Element* const MultiLayer = sc["MultiLayer"];

    if(MultiLayer) {
        multilayer = !!ParseTokenAsInt(GetRequiredToken(*MultiLayer,0));
    }

    if(ShadingModel) {
        shading = ParseTokenAsString(GetRequiredToken(*ShadingModel,0));
    } else {
        DOMWarning("shading mode not specified, assuming phong",&element);
        shading = "phong";
    }

    // lower-case shading because Blender (for example) writes "Phong"
    for (size_t i = 0; i < shading.length(); ++i) {
        shading[i] = static_cast<char>(tolower(static_cast<unsigned char>(shading[i])));
    }
    std::string templateName;
    if(shading == "phong") {
        templateName = "Material.FbxSurfacePhong";
    } else if(shading == "lambert") {
        templateName = "Material.FbxSurfaceLambert";
    } else {
        DOMWarning("shading mode not recognized: " + shading,&element);
    }

    props = GetPropertyTable(doc,templateName,element,sc);

    // resolve texture links
    const std::vector<const Connection*>& conns = doc.GetConnectionsByDestinationSequenced(ID());
    for(const Connection* con : conns) {
        // texture link to properties, not objects
        if ( 0 == con->PropertyName().length()) {
            continue;
        }

        const Object* const ob = con->SourceObject();
        if(nullptr == ob) {
            DOMWarning("failed to read source object for texture link, ignoring",&element);
            continue;
        }

        const Texture* const tex = dynamic_cast<const Texture*>(ob);
        if(nullptr == tex) {
            const LayeredTexture* const layeredTexture = dynamic_cast<const LayeredTexture*>(ob);
            if(!layeredTexture) {
                DOMWarning("source object for texture link is not a texture or layered texture, ignoring",&element);
                continue;
            }
            const std::string& prop = con->PropertyName();
            if (layeredTextures.find(prop) != layeredTextures.end()) {
                DOMWarning("duplicate layered texture link: " + prop,&element);
            }

            layeredTextures[prop] = layeredTexture;
            ((LayeredTexture*)layeredTexture)->fillTexture(doc);
        } else {
            const std::string& prop = con->PropertyName();
            if (textures.find(prop) != textures.end()) {
                DOMWarning("duplicate texture link: " + prop,&element);
            }

            textures[prop] = tex;
        }
    }
}

// ------------------------------------------------------------------------------------------------
Texture::Texture(uint64_t id, const Element& element, const Document& doc, const std::string& name) :
        Object(id,element,name),
        uvTrans(0.0f, 0.0f),
        uvScaling(1.0f,1.0f),
        uvRotation(0.0f),
        type(),
        relativeFileName(),
        fileName(),
        alphaSource(),
        props(),
        media(nullptr) {
    const Scope& sc = GetRequiredScope(element);

    const Element* const Type = sc["Type"];
    const Element* const FileName = sc["FileName"];
    const Element* const RelativeFilename = sc["RelativeFilename"];
    const Element* const ModelUVTranslation = sc["ModelUVTranslation"];
    const Element* const ModelUVScaling = sc["ModelUVScaling"];
    const Element* const Texture_Alpha_Source = sc["Texture_Alpha_Source"];
    const Element* const Cropping = sc["Cropping"];

    if(Type) {
        type = ParseTokenAsString(GetRequiredToken(*Type,0));
    }

    if(FileName) {
        fileName = ParseTokenAsString(GetRequiredToken(*FileName,0));
    }

    if(RelativeFilename) {
        relativeFileName = ParseTokenAsString(GetRequiredToken(*RelativeFilename,0));
    }

    if(ModelUVTranslation) {
        uvTrans = aiVector2D(ParseTokenAsFloat(GetRequiredToken(*ModelUVTranslation,0)),
            ParseTokenAsFloat(GetRequiredToken(*ModelUVTranslation,1))
        );
    }

    if(ModelUVScaling) {
        uvScaling = aiVector2D(ParseTokenAsFloat(GetRequiredToken(*ModelUVScaling,0)),
            ParseTokenAsFloat(GetRequiredToken(*ModelUVScaling,1))
        );
    }

    if(Cropping) {
        crop[0] = ParseTokenAsInt(GetRequiredToken(*Cropping,0));
        crop[1] = ParseTokenAsInt(GetRequiredToken(*Cropping,1));
        crop[2] = ParseTokenAsInt(GetRequiredToken(*Cropping,2));
        crop[3] = ParseTokenAsInt(GetRequiredToken(*Cropping,3));
    } else {
        // vc8 doesn't support the crop() syntax in initialization lists
        // (and vc9 WARNS about the new (i.e. compliant) behaviour).
        crop[0] = crop[1] = crop[2] = crop[3] = 0;
    }

    if(Texture_Alpha_Source) {
        alphaSource = ParseTokenAsString(GetRequiredToken(*Texture_Alpha_Source,0));
    }

    props = GetPropertyTable(doc,"Texture.FbxFileTexture",element,sc);

    // 3DS Max and FBX SDK use "Scaling" and "Translation" instead of "ModelUVScaling" and "ModelUVTranslation". Use these properties if available.
    bool ok;
    const aiVector3D& scaling = PropertyGet<aiVector3D>(*props, "Scaling", ok);
    if (ok) {
        uvScaling.x = scaling.x;
        uvScaling.y = scaling.y;
    }

    const aiVector3D& trans = PropertyGet<aiVector3D>(*props, "Translation", ok);
    if (ok) {
        uvTrans.x = trans.x;
        uvTrans.y = trans.y;
    }

    const aiVector3D &rotation = PropertyGet<aiVector3D>(*props, "Rotation", ok);
    if (ok) {
        uvRotation = rotation.z;
    }

    // resolve video links
    if(doc.Settings().readTextures || doc.HasEmbeddedTextureSpillDirectory()) {
        const std::vector<const Connection*>& conns = doc.GetConnectionsByDestinationSequenced(ID());
        for(const Connection* con : conns) {
            const Object* const ob = con->SourceObject();
            if (nullptr == ob) {
                DOMWarning("failed to read source object for texture link, ignoring",&element);
                continue;
            }

            const Video* const video = dynamic_cast<const Video*>(ob);
            if(video) {
                media = video;
            }
        }
    }
}


Texture::~Texture() = default;

LayeredTexture::LayeredTexture(uint64_t id, const Element& element, const Document& /*doc*/, const std::string& name) :
        Object(id,element,name),
        blendMode(BlendMode_Modulate),
        alpha(1) {
    const Scope& sc = GetRequiredScope(element);

    const Element* const BlendModes = sc["BlendModes"];
    const Element* const Alphas = sc["Alphas"];

    if (nullptr != BlendModes) {
        blendMode = (BlendMode)ParseTokenAsInt(GetRequiredToken(*BlendModes,0));
    }
    if (nullptr != Alphas) {
        alpha = ParseTokenAsFloat(GetRequiredToken(*Alphas,0));
    }
}

LayeredTexture::~LayeredTexture() = default;

void LayeredTexture::fillTexture(const Document& doc) {
    const std::vector<const Connection*>& conns = doc.GetConnectionsByDestinationSequenced(ID());
    for(size_t i = 0; i < conns.size();++i) {
        const Connection* con = conns.at(i);

        const Object* const ob = con->SourceObject();
        if (nullptr == ob) {
            DOMWarning("failed to read source object for texture link, ignoring",&element);
            continue;
        }

        const Texture* const tex = dynamic_cast<const Texture*>(ob);

        textures.push_back(tex);
    }
}

// ------------------------------------------------------------------------------------------------
Video::Video(uint64_t id, const Element &element, const Document &doc, const std::string &name) :
        Object(id, element, name),
        contentLength(0),
        content(nullptr) {
    const Scope& sc = GetRequiredScope(element);

    const Element* const Type = sc["Type"];
    const Element* const FileName = sc.FindElementCaseInsensitive("FileName");  //some files retain the information as "Filename", others "FileName", who knows
    const Element* const RelativeFilename = sc["RelativeFilename"];
    const Element* const Content = sc["Content"];

    if(Type) {
        type = ParseTokenAsString(GetRequiredToken(*Type,0));
    }

    if(FileName) {
        fileName = ParseTokenAsString(GetRequiredToken(*FileName,0));
    }

    if(RelativeFilename) {
        relativeFileName = ParseTokenAsString(GetRequiredToken(*RelativeFilename,0));
    }

    if(Content && !Content->Tokens().empty()) {
        //this field is omitted when the embedded texture is already loaded, let's ignore if it's not found
        try {
            if (doc.HasEmbeddedTextureSpillDirectory()) {
                try {
                    externalizedContentPath = SpillEmbeddedVideoContentToFile(
                            ID(),
                            doc.EmbeddedTextureSpillDirectory(),
                            relativeFileName,
                            fileName,
                            *Content,
                            element);
                } catch (const DeadlyImportError&) {
                    throw;
                } catch (const runtime_error& runtimeError) {
                    ASSIMP_LOG_VERBOSE_DEBUG(
                            "Streaming embedded FBX texture spill failed, falling back to buffered decode: ",
                            runtimeError.what());
                }
            }

            if (externalizedContentPath.empty()) {
                const Token& token = GetRequiredToken(*Content, 0);
                const char* data = token.begin();
                if (!token.IsBinary()) {
                    if (*data != '"') {
                        DOMError("embedded content is not surrounded by quotation marks", &element);
                    } else {
                        size_t targetLength = 0;
                        auto numTokens = Content->Tokens().size();
                        // First time compute size (it could be large like 64Gb and it is good to allocate it once)
                        for (uint32_t tokenIdx = 0; tokenIdx < numTokens; ++tokenIdx) {
                            const Token& dataToken = GetRequiredToken(*Content, tokenIdx);
                            size_t tokenLength = dataToken.end() - dataToken.begin() - 2; // ignore double quotes
                            const char* base64data = dataToken.begin() + 1;
                            const size_t outLength = Util::ComputeDecodedSizeBase64(base64data, tokenLength);
                            if (outLength == 0) {
                                DOMError("Corrupted embedded content found", &element);
                            }
                            targetLength += outLength;
                        }
                        if (targetLength == 0) {
                            DOMError("Corrupted embedded content found", &element);
                        }
                        content = new uint8_t[targetLength];
                        contentLength = static_cast<uint64_t>(targetLength);
                        size_t dst_offset = 0;
                        for (uint32_t tokenIdx = 0; tokenIdx < numTokens; ++tokenIdx) {
                            const Token& dataToken = GetRequiredToken(*Content, tokenIdx);
                            size_t tokenLength = dataToken.end() - dataToken.begin() - 2; // ignore double quotes
                            const char* base64data = dataToken.begin() + 1;
                            dst_offset += Util::DecodeBase64(base64data, tokenLength, content + dst_offset, targetLength - dst_offset);
                        }
                        if (targetLength != dst_offset) {
                            delete[] content;
                            content = nullptr;
                            contentLength = 0;
                            DOMError("Corrupted embedded content found", &element);
                        }
                    }
                } else if (static_cast<size_t>(token.end() - data) < 5) {
                    DOMError("binary data array is too short, need five (5) bytes for type signature and element count", &element);
                } else if (*data != 'R') {
                    DOMWarning("video content is not raw binary data, ignoring", &element);
                } else {
                    // read number of elements
                    uint32_t len = 0;
                    ::memcpy(&len, data + 1, sizeof(len));
                    AI_SWAP4(len);

                    contentLength = len;

                    content = new uint8_t[len];
                    ::memcpy(content, data + 5, len);
                }

                if (doc.HasEmbeddedTextureSpillDirectory() && content != nullptr && contentLength > 0) {
                    try {
                        externalizedContentPath = SpillEmbeddedVideoBufferToFile(
                                ID(),
                                doc.EmbeddedTextureSpillDirectory(),
                                relativeFileName,
                                fileName,
                                content,
                                contentLength);
                        delete[] content;
                        content = nullptr;
                        contentLength = 0;
                    } catch (const DeadlyImportError&) {
                        throw;
                    } catch (const runtime_error& runtimeError) {
                        ASSIMP_LOG_VERBOSE_DEBUG(
                                "Buffered embedded FBX texture spill failed, keeping texture payload in memory: ",
                                runtimeError.what());
                    }
                }
            }
        } catch (const DeadlyImportError&) {
            throw;
        } catch (const runtime_error& runtimeError) {
            //we don't need the content data for contents that has already been loaded
            ASSIMP_LOG_VERBOSE_DEBUG("Caught exception in FBXMaterial (likely because content was already loaded): ",
                    runtimeError.what());
        }
    }

    props = GetPropertyTable(doc,"Video.FbxVideo",element,sc);
}

Video::~Video() {
    if (content != nullptr) {
        delete[] content;
    }
}

} //!FBX
} //!Assimp

#endif // ASSIMP_BUILD_NO_FBX_IMPORTER
