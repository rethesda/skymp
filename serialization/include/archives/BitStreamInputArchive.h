#pragma once
#include "concepts/Concepts.h"
#include <optional>
#include <slikenet/BitStream.h>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "../impl/BitStreamUtil.h"
#include "../impl/BitStreamUtil.ipp"

#include <spdlog/spdlog.h>

class BitStreamInputArchive
{
public:
  explicit BitStreamInputArchive(SLNet::BitStream& bitStream)
    : bs(bitStream)
  {
  }

  template <IntegralConstant T>
  BitStreamInputArchive& Serialize(const char* key, T&)
  {
    // Compile time constant. Do nothing
    // Maybe worth adding equality check
    bs.IgnoreBytes(sizeof(typename T::value_type));
    // spdlog::info("!!! deserialized integral constant {}", key);
    return *this;
  }

  template <StringLike T>
  BitStreamInputArchive& Serialize(const char* key, T& value)
  {
    value.clear();

    uint32_t n = 0;
    Serialize("size", n);

    // TODO: check n before resizing, so that we don't allocate a huge vector
    for (size_t i = 0; i < n; ++i) {
      typename T::value_type element;
      Serialize("element", element);
      value.push_back(element);
    }
    return *this;
  }

  // Specialization for std::array
  template <typename T, std::size_t N>
  BitStreamInputArchive& Serialize(const char* key, std::array<T, N>& value)
  {
    for (size_t i = 0; i < N; ++i) {
      Serialize("element", value[i]);
    }
    return *this;
  }

  template <ContainerLike T>
  BitStreamInputArchive& Serialize(const char* key, T& value)
  {
    value.clear();

    uint32_t n = 0;
    Serialize("size", n);

    // TODO: check n before resizing, so that we don't allocate a huge vector
    for (size_t i = 0; i < n; ++i) {
      typename T::value_type element;
      Serialize("element", element);
      value.push_back(element);
    }
    return *this;
  }

  template <Optional T>
  BitStreamInputArchive& Serialize(const char* key, T& value)
  {
    bool hasValue = false;
    Serialize("hasValue", hasValue);
    if (hasValue) {
      typename T::value_type actualValue;
      Serialize("value", actualValue);
      value = actualValue;
    } else {
      value = std::nullopt;
    }
    return *this;
  }

  template <Arithmetic T>
  BitStreamInputArchive& Serialize(const char* key, T& value)
  {
    SerializationUtil::ReadFromBitStream(bs, value);
    // spdlog::info("!!! deserialized arithmetic {} {}", key, value);
    return *this;
  }

  template <typename... Types>
  BitStreamInputArchive& Serialize(const char* key,
                                   std::variant<Types...>& value)
  {
    uint32_t typeIndex = 0;

    Serialize("typeIndex", typeIndex);

    if (typeIndex >= sizeof...(Types)) {
      throw std::runtime_error(
        "Invalid type index for std::variant deserialization");
    }

    // Helper lambda to visit and deserialize the correct type
    auto deserializeVisitor = [this](auto indexTag,
                                     std::variant<Types...>& variant) {
      using SelectedType =
        typename std::variant_alternative<decltype(indexTag)::value,
                                          std::variant<Types...>>::type;
      SelectedType value;
      Serialize("value", value);
      variant = std::move(value);
    };

    // Visit the type corresponding to the typeIndex
    [&]<std::size_t... Is>(std::index_sequence<Is...>)
    {
      ((typeIndex == Is ? deserializeVisitor(
                            std::integral_constant<std::size_t, Is>{}, value)
                        : void()),
       ...);
    }
    (std::make_index_sequence<sizeof...(Types)>{});

    return *this;
  }

  template <NoneOfTheAbove T>
  BitStreamInputArchive& Serialize(const char* key, T& value)
  {
    value.Serialize(*this);
    // spdlog::info("!!! deserialized none of the above {}", key);
    return *this;
  }

  SLNet::BitStream& bs;
};
