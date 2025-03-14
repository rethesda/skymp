#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "papyrus-vm/OpcodesImplementation.h"
#include "papyrus-vm/Structures.h"
#include <cstdint>
#include <stdexcept>

TEST_CASE("test bool operators (<, <=, >, >=)", "[VarValue]")
{
  VarValue bool1(false);
  VarValue bool2(true);
  REQUIRE(bool(bool1 < bool2));
  REQUIRE(bool(bool1 <= bool2));
  REQUIRE(bool(bool2 > bool1));
  REQUIRE(bool(bool2 >= bool1));
}

TEST_CASE("operator= for owning objects", "[VarValue]")
{
  class MyObject : public IGameObject
  {
    const std::vector<std::shared_ptr<ActivePexInstance>>&
    ListActivePexInstances() const override
    {
      static const std::vector<std::shared_ptr<ActivePexInstance>>
        kEmptyScripts;
      return kEmptyScripts;
    }

    void AddScript(std::shared_ptr<ActivePexInstance> sctipt) noexcept override
    {
      spdlog::critical("VarValueTest.cpp: AddScript not implemented");
      std::terminate();
    }
  };

  VarValue var;
  var = VarValue(std::make_shared<MyObject>());

  REQUIRE(dynamic_cast<MyObject*>(static_cast<IGameObject*>(var)));
}

TEST_CASE("operator== for objects", "[VarValue]")
{
  class MyObject : public IGameObject
  {
  public:
    MyObject(int i) { this->i = i; }

    bool EqualsByValue(const IGameObject& rhs) const override
    {
      if (auto rhsMy = dynamic_cast<const MyObject*>(&rhs))
        return i == rhsMy->i;
      return false;
    }

    const std::vector<std::shared_ptr<ActivePexInstance>>&
    ListActivePexInstances() const override
    {
      static const std::vector<std::shared_ptr<ActivePexInstance>>
        kEmptyScripts;
      return kEmptyScripts;
    }

    void AddScript(std::shared_ptr<ActivePexInstance> sctipt) noexcept override
    {
      spdlog::critical("VarValueTest.cpp: AddScript not implemented");
      std::terminate();
    }

  private:
    int i;
  };

  REQUIRE(VarValue(std::make_shared<MyObject>(1)) ==
          VarValue(std::make_shared<MyObject>(1)));
  REQUIRE(VarValue(std::make_shared<MyObject>(1)) !=
          VarValue(std::make_shared<MyObject>(2)));
}

TEST_CASE("wrong types", "[VarValue]")
{
  // Cast Functions

  VarValue str1("string1");
  VarValue str2("string2");

  REQUIRE(str1.CastToInt() == VarValue(0));
  REQUIRE(VarValue("3").CastToInt() == VarValue(3));

  VarValue floatStr("1.02345");
  REQUIRE(abs(static_cast<double>(floatStr.CastToFloat()) - 1.02345) <=
          std::numeric_limits<double>::epsilon());
}

TEST_CASE("strcat implicit casts", "[VarValue]")
{
  StringTable stringTable;
  auto res = OpcodesImplementation::StrCat(VarValue::None(), VarValue("_abc"),
                                           stringTable);
  REQUIRE(res == VarValue("None_abc"));
}

TEST_CASE("String assign", "[VarValue]")
{
  VarValue x(std::string("123"));
  VarValue y;
  y = x;
  *x.stringHolder = "456";
  REQUIRE(static_cast<const char*>(y) == std::string("123"));
}

TEST_CASE("Mixed arithmetics", "[VarValue]")
{
  std::stringstream ss;
  ss << (VarValue(1.0) + VarValue(2)) << std::endl;
  ss << (VarValue(1.0) - VarValue(2)) << std::endl;
  ss << (VarValue(2.0) * VarValue(2)) << std::endl;
  ss << (VarValue(2.0) / VarValue(2)) << std::endl;
  REQUIRE(ss.str() == "[Float '3']\n[Float '-1']\n[Float '4']\n[Float '1']\n");
}

TEST_CASE("Cast to string", "[VarValue]")
{
  REQUIRE(VarValue::CastToString(VarValue(5.0)) == VarValue("5"));
  REQUIRE(VarValue::CastToString(VarValue(4278190080.0)) ==
          VarValue("4278190080"));

  VarValue arr((uint8_t)VarValue::kType_ObjectArray);
  arr.pArray.reset(new std::vector<VarValue>);
  arr.pArray->resize(2, VarValue::None());
  REQUIRE(VarValue::CastToString(arr) == VarValue("[None, None]"));
}

TEST_CASE("operator==", "[VarValue]")
{
  REQUIRE(VarValue("123") == VarValue("123"));
  REQUIRE(VarValue("123") != VarValue(123));
  REQUIRE(VarValue("123") != VarValue(999));
}
