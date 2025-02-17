//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// ProgramVk.cpp:
//    Implements the class methods for ProgramVk.
//

#include "libANGLE/renderer/vulkan/ProgramVk.h"

#include "common/debug.h"
#include "common/utilities.h"
#include "libANGLE/Context.h"
#include "libANGLE/ProgramLinkedResources.h"
#include "libANGLE/renderer/glslang_wrapper_utils.h"
#include "libANGLE/renderer/renderer_utils.h"
#include "libANGLE/renderer/vulkan/BufferVk.h"
#include "libANGLE/renderer/vulkan/GlslangWrapperVk.h"
#include "libANGLE/renderer/vulkan/TextureVk.h"

namespace rx
{

namespace
{
// Identical to Std140 encoder in all aspects, except it ignores opaque uniform types.
class VulkanDefaultBlockEncoder : public sh::Std140BlockEncoder
{
  public:
    void advanceOffset(GLenum type,
                       const std::vector<unsigned int> &arraySizes,
                       bool isRowMajorMatrix,
                       int arrayStride,
                       int matrixStride) override
    {
        if (gl::IsOpaqueType(type))
        {
            return;
        }

        sh::Std140BlockEncoder::advanceOffset(type, arraySizes, isRowMajorMatrix, arrayStride,
                                              matrixStride);
    }
};

void InitDefaultUniformBlock(const std::vector<sh::ShaderVariable> &uniforms,
                             sh::BlockLayoutMap *blockLayoutMapOut,
                             size_t *blockSizeOut)
{
    if (uniforms.empty())
    {
        *blockSizeOut = 0;
        return;
    }

    VulkanDefaultBlockEncoder blockEncoder;
    sh::GetActiveUniformBlockInfo(uniforms, "", &blockEncoder, blockLayoutMapOut);

    size_t blockSize = blockEncoder.getCurrentOffset();

    // TODO(jmadill): I think we still need a valid block for the pipeline even if zero sized.
    if (blockSize == 0)
    {
        *blockSizeOut = 0;
        return;
    }

    *blockSizeOut = blockSize;
    return;
}

template <typename T>
void UpdateDefaultUniformBlock(GLsizei count,
                               uint32_t arrayIndex,
                               int componentCount,
                               const T *v,
                               const sh::BlockMemberInfo &layoutInfo,
                               angle::MemoryBuffer *uniformData)
{
    const int elementSize = sizeof(T) * componentCount;

    uint8_t *dst = uniformData->data() + layoutInfo.offset;
    if (layoutInfo.arrayStride == 0 || layoutInfo.arrayStride == elementSize)
    {
        uint32_t arrayOffset = arrayIndex * layoutInfo.arrayStride;
        uint8_t *writePtr    = dst + arrayOffset;
        ASSERT(writePtr + (elementSize * count) <= uniformData->data() + uniformData->size());
        memcpy(writePtr, v, elementSize * count);
    }
    else
    {
        // Have to respect the arrayStride between each element of the array.
        int maxIndex = arrayIndex + count;
        for (int writeIndex = arrayIndex, readIndex = 0; writeIndex < maxIndex;
             writeIndex++, readIndex++)
        {
            const int arrayOffset = writeIndex * layoutInfo.arrayStride;
            uint8_t *writePtr     = dst + arrayOffset;
            const T *readPtr      = v + (readIndex * componentCount);
            ASSERT(writePtr + elementSize <= uniformData->data() + uniformData->size());
            memcpy(writePtr, readPtr, elementSize);
        }
    }
}

template <typename T>
void ReadFromDefaultUniformBlock(int componentCount,
                                 uint32_t arrayIndex,
                                 T *dst,
                                 const sh::BlockMemberInfo &layoutInfo,
                                 const angle::MemoryBuffer *uniformData)
{
    ASSERT(layoutInfo.offset != -1);

    const int elementSize = sizeof(T) * componentCount;
    const uint8_t *source = uniformData->data() + layoutInfo.offset;

    if (layoutInfo.arrayStride == 0 || layoutInfo.arrayStride == elementSize)
    {
        const uint8_t *readPtr = source + arrayIndex * layoutInfo.arrayStride;
        memcpy(dst, readPtr, elementSize);
    }
    else
    {
        // Have to respect the arrayStride between each element of the array.
        const int arrayOffset  = arrayIndex * layoutInfo.arrayStride;
        const uint8_t *readPtr = source + arrayOffset;
        memcpy(dst, readPtr, elementSize);
    }
}

class Std140BlockLayoutEncoderFactory : public gl::CustomBlockLayoutEncoderFactory
{
  public:
    sh::BlockLayoutEncoder *makeEncoder() override { return new sh::Std140BlockEncoder(); }
};

void SetupDefaultPipelineState(const ContextVk *contextVk,
                               size_t outputVariablesCount,
                               gl::PrimitiveMode mode,
                               vk::GraphicsPipelineDesc *graphicsPipelineDescOut)
{
    graphicsPipelineDescOut->initDefaults(contextVk);
    graphicsPipelineDescOut->setTopology(mode);
    graphicsPipelineDescOut->setRenderPassSampleCount(1);

    constexpr angle::FormatID kDefaultColorAttachmentFormat = angle::FormatID::R8G8B8A8_UNORM;
    for (size_t colorAttachmentIndex = 0; colorAttachmentIndex < outputVariablesCount;
         colorAttachmentIndex++)
    {
        graphicsPipelineDescOut->setRenderPassColorAttachmentFormat(colorAttachmentIndex,
                                                                    kDefaultColorAttachmentFormat);
    }
}
}  // anonymous namespace

// ProgramVk implementation.
ProgramVk::ProgramVk(const gl::ProgramState &state) : ProgramImpl(state)
{
    GlslangWrapperVk::ResetGlslangProgramInterfaceInfo(&mGlslangProgramInterfaceInfo);
}

ProgramVk::~ProgramVk() = default;

void ProgramVk::destroy(const gl::Context *context)
{
    ContextVk *contextVk = vk::GetImpl(context);
    reset(contextVk);
}

void ProgramVk::reset(ContextVk *contextVk)
{
    GlslangWrapperVk::ResetGlslangProgramInterfaceInfo(&mGlslangProgramInterfaceInfo);

    mExecutable.reset(contextVk);
}

std::unique_ptr<rx::LinkEvent> ProgramVk::load(const gl::Context *context,
                                               gl::BinaryInputStream *stream,
                                               gl::InfoLog &infoLog)
{
    ContextVk *contextVk = vk::GetImpl(context);

    reset(contextVk);

    return mExecutable.load(contextVk, mState.getExecutable(), stream);
}

void ProgramVk::save(const gl::Context *context, gl::BinaryOutputStream *stream)
{
    mExecutable.save(stream);
}

void ProgramVk::setBinaryRetrievableHint(bool retrievable)
{
    // Nothing to do here yet.
}

void ProgramVk::setSeparable(bool separable)
{
    // Nothing to do here yet.
}

std::unique_ptr<LinkEvent> ProgramVk::link(const gl::Context *context,
                                           const gl::ProgramLinkedResources &resources,
                                           gl::InfoLog &infoLog,
                                           const gl::ProgramMergedVaryings &mergedVaryings)
{
    ANGLE_TRACE_EVENT0("gpu.angle", "ProgramVk::link");

    ContextVk *contextVk = vk::GetImpl(context);
    // Link resources before calling GetShaderSource to make sure they are ready for the set/binding
    // assignment done in that function.
    linkResources(resources);

    reset(contextVk);
    mExecutable.clearVariableInfoMap();

    // Gather variable info and compiled SPIR-V binaries.
    gl::ShaderMap<const angle::spirv::Blob *> spirvBlobs;
    GlslangWrapperVk::GetShaderCode(contextVk->getFeatures(), mState, resources,
                                    &mGlslangProgramInterfaceInfo, &spirvBlobs,
                                    &mExecutable.mVariableInfoMap);

    if (contextVk->getFeatures().enablePrecisionQualifiers.enabled)
    {
        mExecutable.resolvePrecisionMismatch(mergedVaryings);
    }

    // Compile the shaders.
    const gl::ProgramExecutable &programExecutable = mState.getExecutable();
    angle::Result status                           = mExecutable.mOriginalShaderInfo.initShaders(
                                  programExecutable.getLinkedShaderStages(), spirvBlobs, mExecutable.mVariableInfoMap);
    if (status != angle::Result::Continue)
    {
        return std::make_unique<LinkEventDone>(status);
    }

    status = initDefaultUniformBlocks(context);
    if (status != angle::Result::Continue)
    {
        return std::make_unique<LinkEventDone>(status);
    }

    // TODO(jie.a.chen@intel.com): Parallelize linking.
    // http://crbug.com/849576
    status = mExecutable.createPipelineLayout(contextVk, programExecutable, nullptr);

    // Create pipeline with default state
    if ((status == angle::Result::Continue) &&
        contextVk->getFeatures().createPipelineDuringLink.enabled)
    {
        PipelineCacheAccess pipelineCache;
        status = contextVk->getRenderer()->getPipelineCache(&pipelineCache);
        if (status != angle::Result::Continue)
        {
            return std::make_unique<LinkEventDone>(status);
        }

        status = createGraphicsPipelineWithDefaultState(context, &pipelineCache);
    }

    return std::make_unique<LinkEventDone>(status);
}

angle::Result ProgramVk::createGraphicsPipelineWithDefaultState(const gl::Context *context,
                                                                PipelineCacheAccess *pipelineCache)
{
    const gl::ProgramExecutable &glExecutable = mState.getExecutable();

    // NOOP if -
    // 1. Program is separable
    // 2. Program has a compute shader
    // 3. Program has greater than 3 output variables
    bool isProgramSeperable = mState.isSeparable();
    bool hasComputeShader   = glExecutable.hasLinkedShaderStage(gl::ShaderType::Compute);
    if (isProgramSeperable || hasComputeShader || glExecutable.getOutputVariables().size() > 3)
    {
        return angle::Result::Continue;
    }

    ContextVk *contextVk                    = vk::GetImpl(context);
    const vk::GraphicsPipelineDesc *descPtr = nullptr;
    vk::PipelineHelper *pipeline            = nullptr;
    vk::GraphicsPipelineDesc graphicsPipelineDesc;

    // It is only at drawcall time that we will have complete information required to build the
    // graphics pipeline descriptor. Use the most "commonly seen" state values and create the
    // pipeline. This attempts to improve shader binary cache hits in the underlying ICD since it is
    // common for the same shader to be used across different pipelines.
    gl::PrimitiveMode mode = (glExecutable.hasLinkedShaderStage(gl::ShaderType::TessControl) ||
                              glExecutable.hasLinkedShaderStage(gl::ShaderType::TessEvaluation))
                                 ? gl::PrimitiveMode::Patches
                                 : gl::PrimitiveMode::TriangleStrip;
    SetupDefaultPipelineState(contextVk, glExecutable.getOutputVariables().size(), mode,
                              &graphicsPipelineDesc);
    return mExecutable.getGraphicsPipeline(contextVk, mode, pipelineCache, PipelineSource::WarmUp,
                                           graphicsPipelineDesc, glExecutable, &descPtr, &pipeline);
}

void ProgramVk::linkResources(const gl::ProgramLinkedResources &resources)
{
    Std140BlockLayoutEncoderFactory std140EncoderFactory;
    gl::ProgramLinkedResourcesLinker linker(&std140EncoderFactory);

    linker.linkResources(mState, resources);
}

angle::Result ProgramVk::initDefaultUniformBlocks(const gl::Context *glContext)
{
    ContextVk *contextVk = vk::GetImpl(glContext);

    // Process vertex and fragment uniforms into std140 packing.
    gl::ShaderMap<sh::BlockLayoutMap> layoutMap;
    gl::ShaderMap<size_t> requiredBufferSize;
    requiredBufferSize.fill(0);

    generateUniformLayoutMapping(layoutMap, requiredBufferSize);
    initDefaultUniformLayoutMapping(layoutMap);

    // All uniform initializations are complete, now resize the buffers accordingly and return
    return mExecutable.resizeUniformBlockMemory(contextVk, mState.getExecutable(),
                                                requiredBufferSize);
}

void ProgramVk::generateUniformLayoutMapping(gl::ShaderMap<sh::BlockLayoutMap> &layoutMap,
                                             gl::ShaderMap<size_t> &requiredBufferSize)
{
    const gl::ProgramExecutable &glExecutable = mState.getExecutable();

    for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
    {
        gl::Shader *shader = mState.getAttachedShader(shaderType);

        if (shader)
        {
            const std::vector<sh::ShaderVariable> &uniforms = shader->getUniforms();
            InitDefaultUniformBlock(uniforms, &layoutMap[shaderType],
                                    &requiredBufferSize[shaderType]);
        }
    }
}

void ProgramVk::initDefaultUniformLayoutMapping(gl::ShaderMap<sh::BlockLayoutMap> &layoutMap)
{
    // Init the default block layout info.
    const auto &uniforms                      = mState.getUniforms();
    const gl::ProgramExecutable &glExecutable = mState.getExecutable();

    for (const gl::VariableLocation &location : mState.getUniformLocations())
    {
        gl::ShaderMap<sh::BlockMemberInfo> layoutInfo;

        if (location.used() && !location.ignored)
        {
            const auto &uniform = uniforms[location.index];
            if (uniform.isInDefaultBlock() && !uniform.isSampler() && !uniform.isImage() &&
                !uniform.isFragmentInOut)
            {
                std::string uniformName = uniform.name;
                if (uniform.isArray())
                {
                    // Gets the uniform name without the [0] at the end.
                    uniformName = gl::StripLastArrayIndex(uniformName);
                    ASSERT(uniformName.size() != uniform.name.size());
                }

                bool found = false;

                for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
                {
                    auto it = layoutMap[shaderType].find(uniformName);
                    if (it != layoutMap[shaderType].end())
                    {
                        found                  = true;
                        layoutInfo[shaderType] = it->second;
                    }
                }

                ASSERT(found);
            }
        }

        for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
        {
            mExecutable.mDefaultUniformBlocks[shaderType]->uniformLayout.push_back(
                layoutInfo[shaderType]);
        }
    }
}

GLboolean ProgramVk::validate(const gl::Caps &caps, gl::InfoLog *infoLog)
{
    // No-op. The spec is very vague about the behavior of validation.
    return GL_TRUE;
}

template <typename T>
void ProgramVk::setUniformImpl(GLint location, GLsizei count, const T *v, GLenum entryPointType)
{
    const gl::VariableLocation &locationInfo  = mState.getUniformLocations()[location];
    const gl::LinkedUniform &linkedUniform    = mState.getUniforms()[locationInfo.index];
    const gl::ProgramExecutable &glExecutable = mState.getExecutable();

    ASSERT(!linkedUniform.isSampler());

    if (linkedUniform.typeInfo->type == entryPointType)
    {
        for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
        {
            DefaultUniformBlock &uniformBlock     = *mExecutable.mDefaultUniformBlocks[shaderType];
            const sh::BlockMemberInfo &layoutInfo = uniformBlock.uniformLayout[location];

            // Assume an offset of -1 means the block is unused.
            if (layoutInfo.offset == -1)
            {
                continue;
            }

            const GLint componentCount = linkedUniform.typeInfo->componentCount;
            UpdateDefaultUniformBlock(count, locationInfo.arrayIndex, componentCount, v, layoutInfo,
                                      &uniformBlock.uniformData);
            mExecutable.mDefaultUniformBlocksDirty.set(shaderType);
        }
    }
    else
    {
        for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
        {
            DefaultUniformBlock &uniformBlock     = *mExecutable.mDefaultUniformBlocks[shaderType];
            const sh::BlockMemberInfo &layoutInfo = uniformBlock.uniformLayout[location];

            // Assume an offset of -1 means the block is unused.
            if (layoutInfo.offset == -1)
            {
                continue;
            }

            const GLint componentCount = linkedUniform.typeInfo->componentCount;

            ASSERT(linkedUniform.typeInfo->type == gl::VariableBoolVectorType(entryPointType));

            GLint initialArrayOffset =
                locationInfo.arrayIndex * layoutInfo.arrayStride + layoutInfo.offset;
            for (GLint i = 0; i < count; i++)
            {
                GLint elementOffset = i * layoutInfo.arrayStride + initialArrayOffset;
                GLint *dst =
                    reinterpret_cast<GLint *>(uniformBlock.uniformData.data() + elementOffset);
                const T *source = v + i * componentCount;

                for (int c = 0; c < componentCount; c++)
                {
                    dst[c] = (source[c] == static_cast<T>(0)) ? GL_FALSE : GL_TRUE;
                }
            }

            mExecutable.mDefaultUniformBlocksDirty.set(shaderType);
        }
    }
}

template <typename T>
void ProgramVk::getUniformImpl(GLint location, T *v, GLenum entryPointType) const
{
    const gl::VariableLocation &locationInfo = mState.getUniformLocations()[location];
    const gl::LinkedUniform &linkedUniform   = mState.getUniforms()[locationInfo.index];

    ASSERT(!linkedUniform.isSampler() && !linkedUniform.isImage());

    const gl::ShaderType shaderType = linkedUniform.getFirstShaderTypeWhereActive();
    ASSERT(shaderType != gl::ShaderType::InvalidEnum);

    const DefaultUniformBlock &uniformBlock = *mExecutable.mDefaultUniformBlocks[shaderType];
    const sh::BlockMemberInfo &layoutInfo   = uniformBlock.uniformLayout[location];

    ASSERT(linkedUniform.typeInfo->componentType == entryPointType ||
           linkedUniform.typeInfo->componentType == gl::VariableBoolVectorType(entryPointType));

    if (gl::IsMatrixType(linkedUniform.type))
    {
        const uint8_t *ptrToElement = uniformBlock.uniformData.data() + layoutInfo.offset +
                                      (locationInfo.arrayIndex * layoutInfo.arrayStride);
        GetMatrixUniform(linkedUniform.type, v, reinterpret_cast<const T *>(ptrToElement), false);
    }
    else
    {
        ReadFromDefaultUniformBlock(linkedUniform.typeInfo->componentCount, locationInfo.arrayIndex,
                                    v, layoutInfo, &uniformBlock.uniformData);
    }
}

void ProgramVk::setUniform1fv(GLint location, GLsizei count, const GLfloat *v)
{
    setUniformImpl(location, count, v, GL_FLOAT);
}

void ProgramVk::setUniform2fv(GLint location, GLsizei count, const GLfloat *v)
{
    setUniformImpl(location, count, v, GL_FLOAT_VEC2);
}

void ProgramVk::setUniform3fv(GLint location, GLsizei count, const GLfloat *v)
{
    setUniformImpl(location, count, v, GL_FLOAT_VEC3);
}

void ProgramVk::setUniform4fv(GLint location, GLsizei count, const GLfloat *v)
{
    setUniformImpl(location, count, v, GL_FLOAT_VEC4);
}

void ProgramVk::setUniform1iv(GLint location, GLsizei count, const GLint *v)
{
    const gl::VariableLocation &locationInfo = mState.getUniformLocations()[location];
    const gl::LinkedUniform &linkedUniform   = mState.getUniforms()[locationInfo.index];
    if (linkedUniform.isSampler())
    {
        // We could potentially cache some indexing here. For now this is a no-op since the mapping
        // is handled entirely in ContextVk.
        return;
    }

    setUniformImpl(location, count, v, GL_INT);
}

void ProgramVk::setUniform2iv(GLint location, GLsizei count, const GLint *v)
{
    setUniformImpl(location, count, v, GL_INT_VEC2);
}

void ProgramVk::setUniform3iv(GLint location, GLsizei count, const GLint *v)
{
    setUniformImpl(location, count, v, GL_INT_VEC3);
}

void ProgramVk::setUniform4iv(GLint location, GLsizei count, const GLint *v)
{
    setUniformImpl(location, count, v, GL_INT_VEC4);
}

void ProgramVk::setUniform1uiv(GLint location, GLsizei count, const GLuint *v)
{
    setUniformImpl(location, count, v, GL_UNSIGNED_INT);
}

void ProgramVk::setUniform2uiv(GLint location, GLsizei count, const GLuint *v)
{
    setUniformImpl(location, count, v, GL_UNSIGNED_INT_VEC2);
}

void ProgramVk::setUniform3uiv(GLint location, GLsizei count, const GLuint *v)
{
    setUniformImpl(location, count, v, GL_UNSIGNED_INT_VEC3);
}

void ProgramVk::setUniform4uiv(GLint location, GLsizei count, const GLuint *v)
{
    setUniformImpl(location, count, v, GL_UNSIGNED_INT_VEC4);
}

template <int cols, int rows>
void ProgramVk::setUniformMatrixfv(GLint location,
                                   GLsizei count,
                                   GLboolean transpose,
                                   const GLfloat *value)
{
    const gl::VariableLocation &locationInfo  = mState.getUniformLocations()[location];
    const gl::LinkedUniform &linkedUniform    = mState.getUniforms()[locationInfo.index];
    const gl::ProgramExecutable &glExecutable = mState.getExecutable();

    for (const gl::ShaderType shaderType : glExecutable.getLinkedShaderStages())
    {
        DefaultUniformBlock &uniformBlock     = *mExecutable.mDefaultUniformBlocks[shaderType];
        const sh::BlockMemberInfo &layoutInfo = uniformBlock.uniformLayout[location];

        // Assume an offset of -1 means the block is unused.
        if (layoutInfo.offset == -1)
        {
            continue;
        }

        SetFloatUniformMatrixGLSL<cols, rows>::Run(
            locationInfo.arrayIndex, linkedUniform.getArraySizeProduct(), count, transpose, value,
            uniformBlock.uniformData.data() + layoutInfo.offset);

        mExecutable.mDefaultUniformBlocksDirty.set(shaderType);
    }
}

void ProgramVk::setUniformMatrix2fv(GLint location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value)
{
    setUniformMatrixfv<2, 2>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix3fv(GLint location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value)
{
    setUniformMatrixfv<3, 3>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix4fv(GLint location,
                                    GLsizei count,
                                    GLboolean transpose,
                                    const GLfloat *value)
{
    setUniformMatrixfv<4, 4>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix2x3fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<2, 3>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix3x2fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<3, 2>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix2x4fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<2, 4>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix4x2fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<4, 2>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix3x4fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<3, 4>(location, count, transpose, value);
}

void ProgramVk::setUniformMatrix4x3fv(GLint location,
                                      GLsizei count,
                                      GLboolean transpose,
                                      const GLfloat *value)
{
    setUniformMatrixfv<4, 3>(location, count, transpose, value);
}

void ProgramVk::getUniformfv(const gl::Context *context, GLint location, GLfloat *params) const
{
    getUniformImpl(location, params, GL_FLOAT);
}

void ProgramVk::getUniformiv(const gl::Context *context, GLint location, GLint *params) const
{
    getUniformImpl(location, params, GL_INT);
}

void ProgramVk::getUniformuiv(const gl::Context *context, GLint location, GLuint *params) const
{
    getUniformImpl(location, params, GL_UNSIGNED_INT);
}
}  // namespace rx
