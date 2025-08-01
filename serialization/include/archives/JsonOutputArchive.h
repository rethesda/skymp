#pragma once
#include "concepts/Concepts.h"
#include <algorithm>
#include <nlohmann/json.hpp>
#include <variant>
#include <vector>

class JsonOutputArchive
{
public:
  template <IntegralConstant T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    j[key] = T::value;
    return *this;
  }

  template <StringLike T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    j[key] = value;
    return *this;
  }

  template <ContainerLike T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& element : value) {
      JsonOutputArchive childArchive;
      childArchive.Serialize("element", element);
      arr.push_back(childArchive.j["element"]);
    }
    j[key] = arr;
    return *this;
  }

  template <Optional T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    if (value.has_value()) {
      Serialize(key, *value);
    } else {
      // enable this if you want to serialize nulls
      // j[key] = nlohmann::json{};
    }
    return *this;
  }

  template <Arithmetic T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    j[key] = value;
    return *this;
  }

  template <typename... Types>
  JsonOutputArchive& Serialize(const char* key,
                               const std::variant<Types...>& value)
  {
    auto serializeVisitor = [&](auto& v) {
      JsonOutputArchive childArchive;
      childArchive.Serialize("element", v);
      return childArchive.j["element"];
    };

    j[key] = std::visit(serializeVisitor, value);

    return *this;
  }

  template <Map T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [k, v] : value) {
      JsonOutputArchive childArchive;
      childArchive.Serialize("element", v);
      obj[k] = childArchive.j["element"];
    }
    j[key] = obj;
    return *this;
  }

  template <NoneOfTheAbove T>
  JsonOutputArchive& Serialize(const char* key, const T& value)
  {
    nlohmann::json arr = nlohmann::json::object();

    JsonOutputArchive childArchive;
    const_cast<T&>(value).Serialize(childArchive);

    j[key] = childArchive.j;
    return *this;
  }

  nlohmann::json j = nlohmann::json::object();
};
