//
// Created by jglrxavpok on 24/11/2021.
//

#pragma once

#include <filesystem>
#include "glslang/Public/ShaderLang.h"

namespace ShaderCompiler {
    class FileIncluder: public glslang::TShader::Includer {
    public:
        IncludeResult *includeSystem(const char *string, const char *string1, size_t size) override;

        IncludeResult *includeLocal(const char *string, const char *string1, size_t size) override;

        void releaseInclude(IncludeResult *result) override;

        std::vector<std::filesystem::path> includedFiles;

        ~FileIncluder() override;
    };
}
