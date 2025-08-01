#pragma once
#include "FunctionInfoProvider.h"

class VmProvider : public FunctionInfoProvider
{
public:
  static VmProvider& GetSingleton();

  // Must also search in base classes
  FunctionInfo_* GetFunctionInfo(const std::string& className,
                                const std::string& funcName) override;

  bool IsDerivedFrom(const char* derivedClassName,
                     const char* baseClassName) override;

private:
  VmProvider();

  struct Impl;
  std::shared_ptr<Impl> pImpl;
};
