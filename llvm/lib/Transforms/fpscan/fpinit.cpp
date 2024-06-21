#include "llvm/Transforms/fpscan/fpscan.h"
using namespace ::llvm;

const std::set<int> FpInit::EmptySet = {};

const std::set<int> &FpInit::getStructFp(StructType *structType)
{
    auto it = TypeFPOffset.find(structType);
    if (it != TypeFPOffset.end())
        return it->second;

    auto &structFPSet = TypeFPOffset[structType];
    for (int i = 0, end = structType->getNumElements(); i != end; ++i)
    {
        auto type = structType->getElementType(i);
        int offset = module->getDataLayout().getStructLayout(structType)->getElementOffset(i);
        // patch thread_struct.s[0], only s0 now
        if (i == 2 && structType->hasName() && structType->getName() == "struct.thread_struct")
        {
            // for (int i = 0; i < 12; ++i)
            // {
            //     structFPSet.insert(offset + 8 * i);
            // }
            structFPSet.insert(offset);
            continue;
        }
        if (fptag->isNoTaggedStructField(structType, i))
        {
            continue;
        }
        else if (isFuncPtr(type) || fptag->isTaggedStructField(structType, i))
        {
            structFPSet.insert(offset);
        }
        else if (type->isStructTy())
        {
            auto fpOffset = getStructFp(cast<StructType>(type));
            for (auto subStructOffset : fpOffset)
            {
                structFPSet.insert(offset + subStructOffset);
            }
        }
        else if (type->isArrayTy())
        {
            auto fpOffset = getArrayVecFp(type);
            for (auto subArrayOffset : fpOffset)
            {
                structFPSet.insert(offset + subArrayOffset);
            }
        }
    }
    return structFPSet;
}

const std::set<int> &FpInit::getArrayVecFp(Type *type)
{
    auto it = TypeFPOffset.find(type);
    if (it != TypeFPOffset.end())
        return it->second;

    auto arrayType = cast<ArrayType>(type);
    unsigned num = arrayType->getNumElements();
    Type *elementType = arrayType->getElementType();

    if (num == 0)
        return EmptySet;

    std::set<int> arrayFPSet;
    if (isFuncPtr(elementType))
    {
        arrayFPSet.insert(0);
    }
    else if (elementType->isStructTy())
    {
        auto fpOffset = getStructFp(cast<StructType>(elementType));
        for (auto subStructOffset : fpOffset)
        {
            arrayFPSet.insert(subStructOffset);
        }
    }
    else if (elementType->isArrayTy())
    {
        auto fpOffset = getArrayVecFp(elementType);
        for (auto subArrayOffset : fpOffset)
        {
            arrayFPSet.insert(subArrayOffset);
        }
    }

    unsigned elementSize = module->getDataLayout().getTypeAllocSize(elementType);
    std::set<int> &thisArrayTypeFPSet = TypeFPOffset[type];
    for (auto ElementOffset : arrayFPSet)
    {
        for (unsigned i = 0; i < num; ++i)
        {
            thisArrayTypeFPSet.insert(ElementOffset + i * elementSize);
        }
    }
    return thisArrayTypeFPSet;
}

const std::set<int> &FpInit::GetTypeFPOffset(Type *type)
{
    auto it = TypeFPOffset.find(type);
    if (it != TypeFPOffset.end())
    {
        return it->second;
    }
    else
    {
        if (isFuncPtr(type))
        {
            TypeFPOffset[type].insert(0);
            return TypeFPOffset[type];
        }
        else if (type->isStructTy())
        {
            return getStructFp(cast<StructType>(type));
        }
        else if (type->isArrayTy())
        {
            return getArrayVecFp(type);
        }
    }
    return EmptySet;
}

void FpInit::getAllFp()
{
    for (GlobalVariable &gv : module->getGlobalList())
    {
        std::string varName = gv.getName().data();
        // __exitcall(x) has no effect if the driver is statically compiled into the kernel
        if (varName.find("__exitcall_") != std::string::npos)
            continue;
        auto type = cast<PointerType>(gv.getType())->getElementType();
        if (fptag->isTaggedFPP(&gv))
        {
            GlobalFpInfo.emplace_back(&gv, 0);
        }
        else
        {
            auto fpOffsetSet = GetTypeFPOffset(type);
            for (auto offset : fpOffsetSet)
            {
                GlobalFpInfo.emplace_back(&gv, offset);
            }
        }
    }
}