/*
---------------------------------------------------------------------------
Open Asset Import Library (assimp)
---------------------------------------------------------------------------

Copyright (c) 2006-2026, assimp team

All rights reserved.

Redistribution and use of this software in source and binary forms,
with or without modification, are permitted provided that the following
conditions are met:

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
---------------------------------------------------------------------------
*/
#include "AbstractImportExportBase.h"
#include "UnitTestPCH.h"
#include <assimp/DefaultIOSystem.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/texture.h>
#include <assimp/Exporter.hpp>
#include <assimp/Importer.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>

using namespace Assimp;

#ifndef ASSIMP_BUILD_NO_EXPORT

class utAssbinImportExport : public AbstractImportExportBase {
public:
    bool importerTest() override {
        Importer importer;
        const aiScene *scene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/spider.obj", aiProcess_ValidateDataStructure);

        Exporter exporter;
        EXPECT_EQ(aiReturn_SUCCESS, exporter.Export(scene, "assbin", ASSIMP_TEST_MODELS_DIR "/OBJ/spider_out.assbin"));
        const aiScene *newScene = importer.ReadFile(ASSIMP_TEST_MODELS_DIR "/OBJ/spider_out.assbin", aiProcess_ValidateDataStructure);

        return newScene != nullptr;
    }

protected:
    class ScopedCurrentPath {
    public:
        explicit ScopedCurrentPath(const std::filesystem::path &nextPath) :
                previousPath(std::filesystem::current_path()) {
            std::filesystem::current_path(nextPath);
        }

        ~ScopedCurrentPath() {
            std::error_code errorCode;
            std::filesystem::current_path(previousPath, errorCode);
        }

    private:
        std::filesystem::path previousPath;
    };

    static aiScene *CreateSceneWithCompressedEmbeddedTexture(
            const char *materialTexturePath,
            const char *embeddedTextureFilename,
            const char *formatHint,
            const unsigned char *textureBytes,
            size_t textureByteLength) {
        aiScene *scene = CreateBaseScene();

        scene->mMaterials[0] = new aiMaterial();
        aiString canonicalPath(materialTexturePath);
        scene->mMaterials[0]->AddProperty(&canonicalPath, AI_MATKEY_TEXTURE_DIFFUSE(0));

        aiString rawPath("*0");
        scene->mMaterials[0]->AddProperty(&rawPath, "$raw.DiffuseColor|file", aiTextureType_NONE, 0);

        scene->mNumTextures = 1;
        scene->mTextures = new aiTexture *[1];
        aiTexture *texture = scene->mTextures[0] = new aiTexture();
        texture->mWidth = static_cast<unsigned int>(textureByteLength);
        texture->mHeight = 0;
        texture->pcData = AllocateCompressedTextureData(textureBytes, textureByteLength);
        texture->mFilename = aiString(embeddedTextureFilename);
        if (nullptr != formatHint) {
            std::strncpy(texture->achFormatHint, formatHint, HINTMAXTEXTURELEN - 1);
        }
        return scene;
    }

    static aiScene *CreateSceneWithCompressedEmbeddedTexture() {
        static const unsigned char pngBytes[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
        return CreateSceneWithCompressedEmbeddedTexture(
                "source-textures/diffuse.png",
                "source-textures/diffuse.png",
                "png",
                pngBytes,
                sizeof(pngBytes));
    }

    static aiScene *CreateSceneWithCompressedEmbeddedTextureUsingFbmFilename() {
        static const unsigned char jpgBytes[] = {
            0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F'
        };
        return CreateSceneWithCompressedEmbeddedTexture(
                "source-textures/auto.fbm",
                "source-textures/auto.fbm",
                "fbm",
                jpgBytes,
                sizeof(jpgBytes));
    }

    static aiScene *CreateSceneWithDuplicateCompressedEmbeddedTextureNames() {
        aiScene *scene = CreateBaseScene();

        scene->mMaterials[0] = new aiMaterial();
        aiString diffuseEmbeddedPath("*0");
        aiString specularEmbeddedPath("*1");
        scene->mMaterials[0]->AddProperty(&diffuseEmbeddedPath, AI_MATKEY_TEXTURE_DIFFUSE(0));
        scene->mMaterials[0]->AddProperty(&specularEmbeddedPath, AI_MATKEY_TEXTURE_SPECULAR(0));

        scene->mNumTextures = 2;
        scene->mTextures = new aiTexture *[2];
        static const unsigned char pngBytes[] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };

        for (unsigned int index = 0; index < scene->mNumTextures; ++index) {
            aiTexture *texture = scene->mTextures[index] = new aiTexture();
            texture->mWidth = static_cast<unsigned int>(sizeof(pngBytes));
            texture->mHeight = 0;
            texture->pcData = AllocateCompressedTextureData(pngBytes, sizeof(pngBytes));
            texture->mFilename = aiString("source-textures/shared.png");
            std::strncpy(texture->achFormatHint, "png", HINTMAXTEXTURELEN - 1);
        }

        return scene;
    }

    static aiScene *CreateSceneWithUncompressedEmbeddedTexture() {
        aiScene *scene = CreateBaseScene();

        scene->mMaterials[0] = new aiMaterial();
        aiString embeddedPath("*0");
        scene->mMaterials[0]->AddProperty(&embeddedPath, AI_MATKEY_TEXTURE_DIFFUSE(0));

        scene->mNumTextures = 1;
        scene->mTextures = new aiTexture *[1];
        aiTexture *texture = scene->mTextures[0] = new aiTexture();
        texture->mWidth = 1;
        texture->mHeight = 1;
        texture->pcData = new aiTexel[1];
        texture->pcData[0] = aiTexel{ 0x10, 0x20, 0x30, 0xff };
        texture->mFilename = aiString("source-textures/mask");
        return scene;
    }

    static aiScene *CreateSceneWithExternalTextureReferences(
            const char *diffuseTexturePath,
            const char *normalTexturePath) {
        aiScene *scene = CreateBaseScene();

        scene->mMaterials[0] = new aiMaterial();
        aiString diffusePath(diffuseTexturePath);
        aiString normalPath(normalTexturePath);
        scene->mMaterials[0]->AddProperty(&diffusePath, AI_MATKEY_TEXTURE_DIFFUSE(0));
        scene->mMaterials[0]->AddProperty(&normalPath, AI_MATKEY_TEXTURE_NORMALS(0));
        return scene;
    }

    static aiScene *CreateBaseScene() {
        aiScene *scene = new aiScene();

        scene->mRootNode = new aiNode();
        scene->mRootNode->mName = aiString("Root");
        scene->mRootNode->mNumMeshes = 1;
        scene->mRootNode->mMeshes = new unsigned int[1];
        scene->mRootNode->mMeshes[0] = 0;

        scene->mNumMeshes = 1;
        scene->mMeshes = new aiMesh *[1];
        aiMesh *mesh = scene->mMeshes[0] = new aiMesh();
        mesh->mName = aiString("Triangle");
        mesh->mPrimitiveTypes = aiPrimitiveType_TRIANGLE;
        mesh->mMaterialIndex = 0;
        mesh->mNumVertices = 3;
        mesh->mVertices = new aiVector3D[3]{
            aiVector3D(0.0f, 0.0f, 0.0f),
            aiVector3D(1.0f, 0.0f, 0.0f),
            aiVector3D(0.0f, 1.0f, 0.0f)
        };
        mesh->mTextureCoords[0] = new aiVector3D[3]{
            aiVector3D(0.0f, 0.0f, 0.0f),
            aiVector3D(1.0f, 0.0f, 0.0f),
            aiVector3D(0.0f, 1.0f, 0.0f)
        };
        mesh->mNumUVComponents[0] = 2;
        mesh->mNumFaces = 1;
        mesh->mFaces = new aiFace[1];
        mesh->mFaces[0].mNumIndices = 3;
        mesh->mFaces[0].mIndices = new unsigned int[3]{ 0, 1, 2 };

        scene->mNumMaterials = 1;
        scene->mMaterials = new aiMaterial *[1];
        return scene;
    }

    static aiTexel *AllocateCompressedTextureData(const unsigned char *bytes, size_t size) {
        const size_t texelCount = 1u + size / sizeof(aiTexel);
        aiTexel *data = new aiTexel[texelCount];
        std::memset(data, 0, texelCount * sizeof(aiTexel));
        std::memcpy(data, bytes, size);
        return data;
    }

    static void WriteTestTextureFile(const std::filesystem::path &filePath, const char *content) {
        std::filesystem::create_directories(filePath.parent_path());
        std::ofstream output(filePath, std::ios::binary);
        output.write(content, static_cast<std::streamsize>(std::strlen(content)));
    }
};

TEST_F(utAssbinImportExport, import3ExportAssbinDFromFileTest) {
    EXPECT_TRUE(importerTest());
}

TEST_F(utAssbinImportExport, exportAssbinExternalizesCompressedEmbeddedTextures) {
    std::unique_ptr<aiScene> scene(CreateSceneWithCompressedEmbeddedTexture());

    const std::string outputPath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/assbin_externalized_compressed_out.assbin";
    const std::string expectedTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/diffuse.png";

    Exporter exporter;
    ASSERT_EQ(aiReturn_SUCCESS, exporter.Export(scene.get(), "assbin", outputPath));

    DefaultIOSystem ioSystem;
    EXPECT_TRUE(ioSystem.Exists(expectedTexturePath.c_str()));

    Importer importer;
    const aiScene *importedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, importedScene);
    EXPECT_EQ(0u, importedScene->mNumTextures);

    aiString canonicalPath;
    const aiReturn canonicalGetResult = importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), canonicalPath);
    ASSERT_EQ(aiReturn_SUCCESS, canonicalGetResult);
    EXPECT_STREQ("textures/diffuse.png", canonicalPath.C_Str());

    aiString rawPath;
    const aiReturn rawGetResult = importedScene->mMaterials[0]->Get("$raw.DiffuseColor|file", aiTextureType_NONE, 0, rawPath);
    ASSERT_EQ(aiReturn_SUCCESS, rawGetResult);
    EXPECT_STREQ("textures/diffuse.png", rawPath.C_Str());
}

TEST_F(utAssbinImportExport, exportAssbinUsesDetectedImageExtensionInsteadOfFbxResourceSuffix) {
    std::unique_ptr<aiScene> scene(CreateSceneWithCompressedEmbeddedTextureUsingFbmFilename());

    const std::string outputPath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/assbin_externalized_fbm_suffix_out.assbin";
    const std::string expectedTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/auto.jpg";

    Exporter exporter;
    ASSERT_EQ(aiReturn_SUCCESS, exporter.Export(scene.get(), "assbin", outputPath));

    DefaultIOSystem ioSystem;
    EXPECT_TRUE(ioSystem.Exists(expectedTexturePath.c_str()));

    Importer importer;
    const aiScene *importedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, importedScene);
    EXPECT_EQ(0u, importedScene->mNumTextures);

    aiString canonicalPath;
    const aiReturn canonicalGetResult = importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), canonicalPath);
    ASSERT_EQ(aiReturn_SUCCESS, canonicalGetResult);
    EXPECT_STREQ("textures/auto.jpg", canonicalPath.C_Str());

    aiString rawPath;
    const aiReturn rawGetResult = importedScene->mMaterials[0]->Get("$raw.DiffuseColor|file", aiTextureType_NONE, 0, rawPath);
    ASSERT_EQ(aiReturn_SUCCESS, rawGetResult);
    EXPECT_STREQ("textures/auto.jpg", rawPath.C_Str());
}

TEST_F(utAssbinImportExport, exportAssbinOnlyRenamesTexturesWhenOriginalNamesConflict) {
    std::unique_ptr<aiScene> scene(CreateSceneWithDuplicateCompressedEmbeddedTextureNames());

    const std::string outputPath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/assbin_externalized_duplicate_names_out.assbin";
    const std::string diffuseTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/shared.png";
    const std::string specularTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/shared_1.png";

    Exporter exporter;
    ASSERT_EQ(aiReturn_SUCCESS, exporter.Export(scene.get(), "assbin", outputPath));

    DefaultIOSystem ioSystem;
    EXPECT_TRUE(ioSystem.Exists(diffuseTexturePath.c_str()));
    EXPECT_TRUE(ioSystem.Exists(specularTexturePath.c_str()));

    Importer importer;
    const aiScene *importedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, importedScene);
    EXPECT_EQ(0u, importedScene->mNumTextures);

    aiString diffusePath;
    const aiReturn diffuseGetResult = importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), diffusePath);
    ASSERT_EQ(aiReturn_SUCCESS, diffuseGetResult);
    EXPECT_STREQ("textures/shared.png", diffusePath.C_Str());

    aiString specularPath;
    const aiReturn specularGetResult = importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_SPECULAR(0), specularPath);
    ASSERT_EQ(aiReturn_SUCCESS, specularGetResult);
    EXPECT_STREQ("textures/shared_1.png", specularPath.C_Str());
}

TEST_F(utAssbinImportExport, exportAssbinExternalizesUncompressedEmbeddedTexturesAsTga) {
    std::unique_ptr<aiScene> scene(CreateSceneWithUncompressedEmbeddedTexture());

    const std::string outputPath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/assbin_externalized_uncompressed_out.assbin";
    const std::string expectedTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/mask.tga";

    Exporter exporter;
    ASSERT_EQ(aiReturn_SUCCESS, exporter.Export(scene.get(), "assbin", outputPath));

    DefaultIOSystem ioSystem;
    EXPECT_TRUE(ioSystem.Exists(expectedTexturePath.c_str()));

    Importer importer;
    const aiScene *importedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, importedScene);
    EXPECT_EQ(0u, importedScene->mNumTextures);

    aiString canonicalPath;
    const aiReturn canonicalGetResult = importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), canonicalPath);
    ASSERT_EQ(aiReturn_SUCCESS, canonicalGetResult);
    EXPECT_STREQ("textures/mask.tga", canonicalPath.C_Str());
}

TEST_F(utAssbinImportExport, exportAssbinCopiesExternalTextureReferencesIntoTexturesDirectory) {
    std::unique_ptr<aiScene> scene(CreateSceneWithExternalTextureReferences(
            "..\\..\\..\\source-textures\\external-diffuse.png",
            "/missing/external-normal.png"));

    const std::filesystem::path assetRootDir =
            std::filesystem::path(ASSIMP_TEST_MODELS_DIR).parent_path().parent_path() / ".tmp" / "assbin_external_asset_root";
    const std::filesystem::path diffuseSourcePath = assetRootDir / "source-textures" / "external-diffuse.png";
    const std::filesystem::path normalSourcePath = assetRootDir / "variants" / "external-normal.png";
    WriteTestTextureFile(diffuseSourcePath, "diffuse-texture");
    WriteTestTextureFile(normalSourcePath, "normal-texture");

    const std::string outputPath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/assbin_externalized_external_refs_out.assbin";
    const std::string diffuseTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/source-textures/external-diffuse.png";
    const std::string normalTexturePath = std::string(ASSIMP_TEST_MODELS_DIR) + "/OBJ/textures/variants/external-normal.png";

    std::filesystem::remove(std::filesystem::u8path(diffuseTexturePath));
    std::filesystem::remove(std::filesystem::u8path(normalTexturePath));

    ScopedCurrentPath scopedCurrentPath(assetRootDir);
    Exporter exporter;
    ASSERT_EQ(aiReturn_SUCCESS, exporter.Export(scene.get(), "assbin", outputPath));

    DefaultIOSystem ioSystem;
    EXPECT_TRUE(ioSystem.Exists(diffuseTexturePath.c_str()));
    EXPECT_TRUE(ioSystem.Exists(normalTexturePath.c_str()));

    Importer importer;
    const aiScene *importedScene = importer.ReadFile(outputPath, aiProcess_ValidateDataStructure);
    ASSERT_NE(nullptr, importedScene);
    EXPECT_EQ(0u, importedScene->mNumTextures);

    aiString diffusePath;
    ASSERT_EQ(aiReturn_SUCCESS, importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_DIFFUSE(0), diffusePath));
    EXPECT_STREQ("textures/source-textures/external-diffuse.png", diffusePath.C_Str());

    aiString normalPath;
    ASSERT_EQ(aiReturn_SUCCESS, importedScene->mMaterials[0]->Get(AI_MATKEY_TEXTURE_NORMALS(0), normalPath));
    EXPECT_STREQ("textures/variants/external-normal.png", normalPath.C_Str());
}

#endif // #ifndef ASSIMP_BUILD_NO_EXPORT
