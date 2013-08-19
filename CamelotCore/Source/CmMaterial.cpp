#include "CmMaterial.h"
#include "CmException.h"
#include "CmShader.h"
#include "CmTechnique.h"
#include "CmPass.h"
#include "CmRenderSystem.h"
#include "CmGpuProgramParams.h"
#include "CmHardwareBufferManager.h"
#include "CmGpuProgram.h"
#include "CmGpuParamBlockBuffer.h"
#include "CmGpuParamDesc.h"
#include "CmMaterialRTTI.h"
#include "CmMaterialManager.h"
#include "CmDebug.h"

namespace CamelotFramework
{
	Material::Material()
		:Resource(false), mRenderQueue(0)
	{

	}

	Material::~Material()
	{
		
	}

	void Material::setShader(ShaderPtr shader)
	{
		mShader = shader;

		initBestTechnique();
	}

	void Material::initBestTechnique()
	{
		mBestTechnique = nullptr;
		mParametersPerPass.clear();
		mFloatValues.clear();
		mVec2Values.clear();
		mVec3Values.clear();
		mVec4Values.clear();
		mMat3Values.clear();
		mMat4Values.clear();
		mStructValues.clear();
		mTextureValues.clear();
		mSamplerValues.clear();
		freeParamBuffers();

		if(mShader)
		{
			mBestTechnique = mShader->getBestTechnique();

			if(mBestTechnique == nullptr)
				return;

			mValidShareableParamBlocks.clear();
			mValidParams.clear();

			Vector<const GpuParamDesc*>::type allParamDescs;

			// Make sure all gpu programs are fully loaded
			for(UINT32 i = 0; i < mBestTechnique->getNumPasses(); i++)
			{
				PassPtr curPass = mBestTechnique->getPass(i);

				HGpuProgram vertProgram = curPass->getVertexProgram();
				if(vertProgram)
				{
					vertProgram.synchronize();
					allParamDescs.push_back(&vertProgram->getParamDesc());
				}

				HGpuProgram fragProgram = curPass->getFragmentProgram();
				if(fragProgram)
				{
					fragProgram.synchronize();
					allParamDescs.push_back(&fragProgram->getParamDesc());
				}

				HGpuProgram geomProgram = curPass->getGeometryProgram();
				if(geomProgram)
				{
					geomProgram.synchronize();
					allParamDescs.push_back(&geomProgram->getParamDesc());
				}

				HGpuProgram hullProgram = curPass->getHullProgram();
				if(hullProgram)
				{
					hullProgram.synchronize();
					allParamDescs.push_back(&hullProgram->getParamDesc());
				}

				HGpuProgram domainProgram = curPass->getDomainProgram();
				if(domainProgram)
				{
					domainProgram.synchronize();
					allParamDescs.push_back(&domainProgram->getParamDesc());
				}

				HGpuProgram computeProgram = curPass->getComputeProgram();
				if(computeProgram)
				{
					computeProgram.synchronize();
					allParamDescs.push_back(&computeProgram->getParamDesc());
				}
			}

			// Fill out various helper structures
			Map<String, const GpuParamDataDesc*>::type validDataParameters = determineValidDataParameters(allParamDescs);
			Set<String>::type validObjectParameters = determineValidObjectParameters(allParamDescs);

			Set<String>::type validShareableParamBlocks = determineValidShareableParamBlocks(allParamDescs);
			Map<String, String>::type paramToParamBlockMap = determineParameterToBlockMapping(allParamDescs);
			Map<String, GpuParamBlockBufferPtr>::type paramBlockBuffers;

			// Create param blocks
			const Map<String, SHADER_PARAM_BLOCK_DESC>::type& shaderDesc = mShader->getParamBlocks();
			for(auto iter = validShareableParamBlocks.begin(); iter != validShareableParamBlocks.end(); ++iter)
			{
				bool isShared = false;
				GpuParamBlockUsage usage = GPBU_STATIC;

				auto iterFind = shaderDesc.find(*iter);
				if(iterFind != shaderDesc.end())
				{
					isShared = iterFind->second.shared;
					usage = iterFind->second.usage;
				}

				GpuParamBlockDesc blockDesc;
				for(auto iter2 = allParamDescs.begin(); iter2 != allParamDescs.end(); ++iter2)
				{
					auto findParamBlockDesc = (*iter2)->paramBlocks.find(*iter);

					if(findParamBlockDesc != (*iter2)->paramBlocks.end())
					{
						blockDesc = findParamBlockDesc->second;
						break;
					}
				}

				GpuParamBlockBufferPtr newParamBlockBuffer;
				if(!isShared)
				{
					newParamBlockBuffer = HardwareBufferManager::instance().createGpuParamBlockBuffer(blockDesc.blockSize * sizeof(UINT32), usage);
					mParamBuffers.push_back(newParamBlockBuffer);
				}

				paramBlockBuffers[*iter] = newParamBlockBuffer;
				mValidShareableParamBlocks.insert(*iter);
			}

			// Create data param mappings
			const Map<String, SHADER_DATA_PARAM_DESC>::type& dataParamDesc = mShader->getDataParams();
			for(auto iter = dataParamDesc.begin(); iter != dataParamDesc.end(); ++iter)
			{
				auto findIter = validDataParameters.find(iter->second.gpuVariableName);

				// Not valid so we skip it
				if(findIter == validDataParameters.end())
					continue;

				if(findIter->second->type != iter->second.type)
				{
					LOGWRN("Ignoring shader parameter \"" + iter->first  +"\". Type doesn't match the one defined in the gpu program. "
						+ "Shader defined type: " + toString(iter->second.type) + " - Gpu program defined type: " + toString(findIter->second->type));
					continue;
				}

				if(findIter->second->arraySize != iter->second.arraySize)
				{
					LOGWRN("Ignoring shader parameter \"" + iter->first  +"\". Array size doesn't match the one defined in the gpu program."
						+ "Shader defined array size: " + toString(iter->second.arraySize) + " - Gpu program defined array size: " + toString(findIter->second->arraySize));
					continue;
				}

				auto findBlockIter = paramToParamBlockMap.find(iter->second.gpuVariableName);

				if(findBlockIter == paramToParamBlockMap.end())
					CM_EXCEPT(InternalErrorException, "Parameter doesn't exist in param to param block map but exists in valid param map.");

				String& paramBlockName = findBlockIter->second;
				mValidParams[iter->first] = iter->second.gpuVariableName;

				switch(iter->second.type)
				{
				case GPDT_FLOAT1:
					mFloatValues[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_FLOAT2:
					mVec2Values[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_FLOAT3:
					mVec3Values[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_FLOAT4:
					mVec4Values[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_MATRIX_3X3:
					mMat3Values[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_MATRIX_4X4:
					mMat4Values[iter->first].resize(iter->second.arraySize);
					break;
				case GPDT_STRUCT:
					mStructValues[iter->first].resize(iter->second.arraySize);
					break;
				default:
					CM_EXCEPT(InternalErrorException, "Unsupported data type.");
				}
			}

			// Create object param mappings
			const Map<String, SHADER_OBJECT_PARAM_DESC>::type& objectParamDesc = mShader->getObjectParams();
			for(auto iter = objectParamDesc.begin(); iter != objectParamDesc.end(); ++iter)
			{
				auto findIter = validObjectParameters.find(iter->second.gpuVariableName);

				// Not valid so we skip it
				if(findIter == validObjectParameters.end())
					continue;

				mValidParams[iter->first] = iter->second.gpuVariableName;

				if(Shader::isSampler(iter->second.type))
				{
					mSamplerValues[iter->first] = HSamplerState();
				}
				else if(Shader::isTexture(iter->second.type))
				{
					mTextureValues[iter->first] = HTexture();
				}
				else if(Shader::isBuffer(iter->second.type))
				{
					// TODO

					CM_EXCEPT(NotImplementedException, "Buffers not implemented.");
				}
				else
					CM_EXCEPT(InternalErrorException, "Invalid object param type.");
			}

			for(UINT32 i = 0; i < mBestTechnique->getNumPasses(); i++)
			{
				PassPtr curPass = mBestTechnique->getPass(i);
				PassParametersPtr params = PassParametersPtr(new PassParameters());

				HGpuProgram vertProgram = curPass->getVertexProgram();
				if(vertProgram)
					params->mVertParams = vertProgram->createParameters();

				HGpuProgram fragProgram = curPass->getFragmentProgram();
				if(fragProgram)
					params->mFragParams = fragProgram->createParameters();

				HGpuProgram geomProgram = curPass->getGeometryProgram();
				if(geomProgram)
					params->mGeomParams = geomProgram->createParameters();	

				HGpuProgram hullProgram = curPass->getHullProgram();
				if(hullProgram)
					params->mHullParams = hullProgram->createParameters();

				HGpuProgram domainProgram = curPass->getDomainProgram();
				if(domainProgram)
					params->mDomainParams = domainProgram->createParameters();

				HGpuProgram computeProgram = curPass->getComputeProgram();
				if(computeProgram)
					params->mComputeParams = computeProgram->createParameters();	

				mParametersPerPass.push_back(params);
			}

			// Assign param block buffers
			for(auto iter = mParametersPerPass.begin(); iter != mParametersPerPass.end(); ++iter)
			{
				PassParametersPtr params = *iter;

				for(UINT32 i = 0; i < params->getNumParams(); i++)
				{
					GpuParamsPtr& paramPtr = params->getParamByIdx(i);
					if(paramPtr)
					{
						// Assign shareable buffers
						for(auto iterBlock = mValidShareableParamBlocks.begin(); iterBlock != mValidShareableParamBlocks.end(); ++iterBlock)
						{
							const String& paramBlockName = *iterBlock;
							if(paramPtr->hasParamBlock(paramBlockName))
							{
								GpuParamBlockBufferPtr blockBuffer = paramBlockBuffers[paramBlockName];

								paramPtr->setParamBlockBuffer(paramBlockName, blockBuffer);
							}
						}

						// Create non-shareable ones
						const GpuParamDesc& desc = paramPtr->getParamDesc();
						for(auto iterBlockDesc = desc.paramBlocks.begin(); iterBlockDesc != desc.paramBlocks.end(); ++iterBlockDesc)
						{
							if(!iterBlockDesc->second.isShareable)
							{
								GpuParamBlockBufferPtr newParamBlockBuffer = HardwareBufferManager::instance().createGpuParamBlockBuffer(iterBlockDesc->second.blockSize * sizeof(UINT32));
								mParamBuffers.push_back(newParamBlockBuffer);

								paramPtr->setParamBlockBuffer(iterBlockDesc->first, newParamBlockBuffer);
							}
						}
					}
				}
			}
		}
	}

	Map<String, const GpuParamDataDesc*>::type Material::determineValidDataParameters(const Vector<const GpuParamDesc*>::type& paramDescs) const
	{
		Map<String, const GpuParamDataDesc*>::type foundDataParams;
		Map<String, bool>::type validParams;

		for(auto iter = paramDescs.begin(); iter != paramDescs.end(); ++iter)
		{
			const GpuParamDesc& curDesc = **iter;

			// Check regular data params
			for(auto iter2 = curDesc.params.begin(); iter2 != curDesc.params.end(); ++iter2)
			{
				bool isParameterValid = true;
				const GpuParamDataDesc& curParam = iter2->second;

				auto dataFindIter = validParams.find(iter2->first);
				if(dataFindIter == validParams.end())
				{
					validParams[iter2->first] = true;
					foundDataParams[iter2->first] = &curParam;
				}
				else
				{
					if(validParams[iter2->first])
					{
						auto dataFindIter2 = foundDataParams.find(iter2->first);

						const GpuParamDataDesc* otherParam = dataFindIter2->second;
						if(!areParamsEqual(curParam, *otherParam, true))
						{
							validParams[iter2->first] = false;
							foundDataParams.erase(dataFindIter2);
						}
					}
				}
			}
		}

		return foundDataParams;
	}

	
	Set<String>::type Material::determineValidObjectParameters(const Vector<const GpuParamDesc*>::type& paramDescs) const
	{
		Set<String>::type validParams;

		for(auto iter = paramDescs.begin(); iter != paramDescs.end(); ++iter)
		{
			const GpuParamDesc& curDesc = **iter;

			// Check sampler params
			for(auto iter2 = curDesc.samplers.begin(); iter2 != curDesc.samplers.end(); ++iter2)
			{
				if(validParams.find(iter2->first) == validParams.end())
					validParams.insert(iter2->first);
			}

			// Check texture params
			for(auto iter2 = curDesc.textures.begin(); iter2 != curDesc.textures.end(); ++iter2)
			{
				if(validParams.find(iter2->first) == validParams.end())
					validParams.insert(iter2->first);
			}

			// Check buffer params
			for(auto iter2 = curDesc.buffers.begin(); iter2 != curDesc.buffers.end(); ++iter2)
			{
				if(validParams.find(iter2->first) == validParams.end())
					validParams.insert(iter2->first);
			}
		}

		return validParams;
	}

	Set<String>::type Material::determineValidShareableParamBlocks(const Vector<const GpuParamDesc*>::type& paramDescs) const
	{
		// Make sure param blocks with the same name actually are the same
		Map<String, std::pair<String, const GpuParamDesc*>>::type uniqueParamBlocks;
		Map<String, bool>::type validParamBlocks;

		for(auto iter = paramDescs.begin(); iter != paramDescs.end(); ++iter)
		{
			const GpuParamDesc& curDesc = **iter;
			for(auto blockIter = curDesc.paramBlocks.begin(); blockIter != curDesc.paramBlocks.end(); ++blockIter)
			{
				bool isBlockValid = true;
				const GpuParamBlockDesc& curBlock = blockIter->second;

				if(!curBlock.isShareable) // Non-shareable buffers are handled differently, they're allowed same names
					continue;

				auto iterFind = uniqueParamBlocks.find(blockIter->first);
				if(iterFind == uniqueParamBlocks.end())
				{
					uniqueParamBlocks[blockIter->first] = std::make_pair(blockIter->first, *iter);
					validParamBlocks[blockIter->first] = true;
					continue;
				}

				String otherBlockName = iterFind->second.first;
				const GpuParamDesc* otherDesc = iterFind->second.second;

				for(auto myParamIter = curDesc.params.begin(); myParamIter != curDesc.params.end(); ++myParamIter)
				{
					const GpuParamDataDesc& myParam = myParamIter->second;

					if(myParam.paramBlockSlot != curBlock.slot)
						continue; // Param is in another block, so we will check it when its time for that block

					auto otherParamFind = otherDesc->params.find(myParamIter->first);

					// Cannot find other param, blocks aren't equal
					if(otherParamFind == otherDesc->params.end())
					{
						isBlockValid = false;
						break;
					}

					const GpuParamDataDesc& otherParam = otherParamFind->second;

					if(!areParamsEqual(myParam, otherParam) || curBlock.name != otherBlockName)
					{
						isBlockValid = false;
						break;
					}
				}

				if(!isBlockValid)
				{
					if(validParamBlocks[blockIter->first])
					{
						LOGWRN("Found two param blocks with the same name but different contents: " + blockIter->first);
						validParamBlocks[blockIter->first] = false;
					}
				}
			}
		}

		Set<String>::type validParamBlocksReturn;
		for(auto iter = validParamBlocks.begin(); iter != validParamBlocks.end(); ++iter)
		{
			if(iter->second)
				validParamBlocksReturn.insert(iter->first);
		}

		return validParamBlocksReturn;
	}

	Map<String, String>::type Material::determineParameterToBlockMapping(const Vector<const GpuParamDesc*>::type& paramDescs)
	{
		Map<String, String>::type paramToParamBlock;

		for(auto iter = paramDescs.begin(); iter != paramDescs.end(); ++iter)
		{
			const GpuParamDesc& curDesc = **iter;
			for(auto iter2 = curDesc.params.begin(); iter2 != curDesc.params.end(); ++iter2)
			{
				const GpuParamDataDesc& curParam = iter2->second;
				
				auto iterFind = paramToParamBlock.find(curParam.name);
				if(iterFind != paramToParamBlock.end())
					continue;

				for(auto iterBlock = curDesc.paramBlocks.begin(); iterBlock != curDesc.paramBlocks.end(); ++iterBlock)
				{
					if(iterBlock->second.slot == curParam.paramBlockSlot)
					{
						paramToParamBlock[curParam.name] = iterBlock->second.name;
						break;
					}
				}
			}
		}

		return paramToParamBlock;
	}

	bool Material::areParamsEqual(const GpuParamDataDesc& paramA, const GpuParamDataDesc& paramB, bool ignoreBufferOffsets) const
	{
		bool equal = paramA.arraySize == paramB.arraySize && paramA.elementSize == paramB.elementSize 
			&& paramA.type == paramB.type && paramA.arrayElementStride == paramB.arrayElementStride;

		if(!ignoreBufferOffsets)
			equal &= paramA.cpuMemOffset == paramB.cpuMemOffset && paramA.gpuMemOffset == paramB.gpuMemOffset;

		return equal;
	}

	void Material::throwIfNotInitialized() const
	{
		if(mShader == nullptr)
		{
			CM_EXCEPT(InternalErrorException, "Material does not have shader set.");
		}

		if(mBestTechnique == nullptr)
		{
			CM_EXCEPT(InternalErrorException, "Shader does not contain a supported technique.");
		}
	}

	void Material::setTexture(const String& name, const HTexture& value)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		for(auto iter = mParametersPerPass.begin(); iter != mParametersPerPass.end(); ++iter)
		{
			PassParametersPtr params = *iter;

			for(UINT32 i = 0; i < params->getNumParams(); i++)
			{
				GpuParamsPtr& paramPtr = params->getParamByIdx(i);
				if(paramPtr)
				{
					if(paramPtr->hasTexture(gpuVarName))
						paramPtr->setTexture(gpuVarName, value);
				}
			}
		}

		mTextureValues[name] = value;
	}

	void Material::setSamplerState(const String& name, const HSamplerState& samplerState)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		for(auto iter = mParametersPerPass.begin(); iter != mParametersPerPass.end(); ++iter)
		{
			PassParametersPtr params = *iter;

			for(UINT32 i = 0; i < params->getNumParams(); i++)
			{
				GpuParamsPtr& paramPtr = params->getParamByIdx(i);
				if(paramPtr)
				{
					if(paramPtr->hasSamplerState(gpuVarName))
						paramPtr->setSamplerState(gpuVarName, samplerState);
				}
			}
		}

		mSamplerValues[name] = samplerState;
	}

	void Material::setFloat(const String& name, float value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mFloatValues[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setColor(const String& name, const Color& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mVec4Values[name];
		savedValue[arrayIdx] = Vector4(value.r, value.g, value.b, value.a);
	}

	void Material::setVec2(const String& name, const Vector2& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mVec2Values[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setVec3(const String& name, const Vector3& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mVec3Values[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setVec4(const String& name, const Vector4& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mVec4Values[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setMat3(const String& name, const Matrix3& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mMat3Values[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setMat4(const String& name, const Matrix4& value, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		String& gpuVarName = iterFind->second;
		setParam(gpuVarName, value, arrayIdx);

		auto& savedValue = mMat4Values[name];
		savedValue[arrayIdx] = value;
	}

	void Material::setStructData(const String& name, void* value, UINT32 size, UINT32 arrayIdx)
	{
		throwIfNotInitialized();

		auto iterFind = mValidParams.find(name);
		if(iterFind == mValidParams.end())
		{
			LOGWRN("Material doesn't have a parameter named " + name);
			return;
		}

		const SHADER_DATA_PARAM_DESC& desc = mShader->getDataParamDesc(name);
		if(desc.elementSize != size)
		{
			CM_EXCEPT(InvalidParametersException,
				"Invalid size when writing a struct. Expected: " + toString(desc.elementSize) + ". Got: " + toString(size));
		}

		String& gpuVarName = iterFind->second;
		for(auto iter = mParametersPerPass.begin(); iter != mParametersPerPass.end(); ++iter)
		{
			PassParametersPtr params = *iter;

			for(UINT32 i = 0; i < params->getNumParams(); i++)
			{
				GpuParamsPtr& paramPtr = params->getParamByIdx(i);
				if(paramPtr)
				{
					if(paramPtr->hasParam(gpuVarName))
						paramPtr->setParam(gpuVarName, value, size, arrayIdx);
				}
			}
		}

		auto& savedValue = mStructValues[name];
		savedValue[arrayIdx] = StructData(value, size);
	}

	//void Material::setParamBlock(const String& name, GpuParamBlockPtr paramBlock)
	//{
	//	auto iterFind = mValidShareableParamBlocks.find(name);
	//	if(iterFind == mValidShareableParamBlocks.end())
	//	{
	//		LOGWRN("Material doesn't have a parameter block named " + name);
	//		return;
	//	}

	//	for(auto iter = mParametersPerPass.begin(); iter != mParametersPerPass.end(); ++iter)
	//	{
	//		PassParametersPtr params = *iter;

	//		for(UINT32 i = 0; i < params->getNumParams(); i++)
	//		{
	//			GpuParamsPtr& paramPtr = params->getParamByIdx(i);
	//			if(paramPtr)
	//			{
	//				if(paramPtr->hasParamBlock(name))
	//					paramPtr->setParam(name, paramBlock);
	//			}
	//		}
	//	}
	//}

	UINT32 Material::getNumPasses() const
	{
		throwIfNotInitialized();

		return mShader->getBestTechnique()->getNumPasses();
	}

	PassPtr Material::getPass(UINT32 passIdx) const
	{
		if(passIdx < 0 || passIdx >= mShader->getBestTechnique()->getNumPasses())
			CM_EXCEPT(InvalidParametersException, "Invalid pass index.");

		return mShader->getBestTechnique()->getPass(passIdx);
	}

	PassParametersPtr Material::getPassParameters(UINT32 passIdx) const
	{
		if(passIdx < 0 || passIdx >= mParametersPerPass.size())
			CM_EXCEPT(InvalidParametersException, "Invalid pass index.");

		PassParametersPtr params = mParametersPerPass[passIdx];

		return params;
	}

	HTexture Material::getTexture(const String& name) const
	{
		auto iterFind = mTextureValues.find(name);

		if(iterFind == mTextureValues.end())
			CM_EXCEPT(InternalErrorException, "No texture parameter with the name: " + name);

		return iterFind->second;
	}

	HSamplerState Material::getSamplerState(const String& name) const
	{
		auto iterFind = mSamplerValues.find(name);

		if(iterFind == mSamplerValues.end())
			CM_EXCEPT(InternalErrorException, "No sampler state parameter with the name: " + name);

		return iterFind->second;
	}

	float Material::getFloat(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mFloatValues.find(name);

		if(iterFind == mFloatValues.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	Vector2 Material::getVec2(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mVec2Values.find(name);

		if(iterFind == mVec2Values.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	Vector3 Material::getVec3(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mVec3Values.find(name);

		if(iterFind == mVec3Values.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	Vector4 Material::getVec4(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mVec4Values.find(name);

		if(iterFind == mVec4Values.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	Matrix3 Material::getMat3(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mMat3Values.find(name);

		if(iterFind == mMat3Values.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	Matrix4 Material::getMat4(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mMat4Values.find(name);

		if(iterFind == mMat4Values.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	const Material::StructData& Material::getStructData(const String& name, UINT32 arrayIdx) const
	{
		auto iterFind = mStructValues.find(name);

		if(iterFind == mStructValues.end())
			CM_EXCEPT(InternalErrorException, "No float parameter with the name: " + name);

		return iterFind->second.at(arrayIdx);
	}

	void Material::destroy_internal()
	{
		freeParamBuffers();

		Resource::destroy_internal();
	}

	void Material::freeParamBuffers()
	{
		for(auto& buffer : mParamBuffers)
		{
			buffer->destroy();
		}

		mParamBuffers.clear();
	}

	HMaterial Material::create()
	{
		MaterialPtr materialPtr = MaterialManager::instance().create();

		return static_resource_cast<Material>(Resource::_createResourceHandle(materialPtr));
	}

	HMaterial Material::create(ShaderPtr shader)
	{
		MaterialPtr materialPtr = MaterialManager::instance().create(shader);

		return static_resource_cast<Material>(Resource::_createResourceHandle(materialPtr));
	}

	RTTITypeBase* Material::getRTTIStatic()
	{
		return MaterialRTTI::instance();
	}

	RTTITypeBase* Material::getRTTI() const
	{
		return Material::getRTTIStatic();
	}
}