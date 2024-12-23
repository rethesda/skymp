#pragma once
#include "FormCallbacks.h"
#include "MpActor.h"
#include "MsgType.h"
#include "PartOne.h"
#include "script_compatibility_policies/IPapyrusCompatibilityPolicy.h"
#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>
#include <thread>

// Utilities for testing

enum class Constants : uint32_t
{
  kWhiterunCell = 0x1a26f,
  kBarrelInWhiterun = 0x4cc2d,
  kTamriel = 0x3c
};

std::underlying_type_t<Constants> ToUnderlying(Constants constant);

bool IsCmakeOptionSpecified(const std::string& optionValue);

const char* GetDataDir();

std::string MakeMessage(const nlohmann::json& j);

void DoMessage(PartOne& partOne, Networking::UserId id,
               const nlohmann::json& j);

void DoConnect(PartOne& partOne, Networking::UserId id);

void DoDisconnect(PartOne& partOne, Networking::UserId id);

void DoUpdateMovement(PartOne& partOne, uint32_t actorFormId,
                      Networking::UserId userId);

static const auto jMovement =
  nlohmann::json{ { "t", MsgType::UpdateMovement },
                  { "idx", 0 },
                  { "data",
                    { { "worldOrCell", 0x3c },
                      { "pos", { 1.f, -1.f, 1.f } },
                      { "rot", { 0.f, 0.f, 179.f } },
                      { "runMode", "Standing" },
                      { "direction", 0.f },
                      { "healthPercentage", 1.f },
                      { "speed", 0.f },
                      { "isInJumpState", false },
                      { "isSneaking", false },
                      { "isBlocking", false },
                      { "isDead", false },
                      { "isWeapDrawn", false } } } };

static const auto jAppearance = nlohmann::json{
  { "t", MsgType::UpdateAppearance },
  { "idx", 0 },
  { "data",
    { { "isFemale", false },
      { "raceId", 0x00000001 },
      { "weight", 99.9f },
      { "skinColor", -1 },
      { "hairColor", -1 },
      { "headpartIds", nlohmann::json::array() },
      { "headTextureSetId", 0x00000000 },
      { "tints", nlohmann::json::array() },
      { "name", "Oberyn" },
      { "options",
        nlohmann::json::array({ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0 }) },                  // size=19
      { "presets", nlohmann::json::array({ 0, 0, 0, 0 }) } } } // size=4
};

static const auto jEquipment = nlohmann::json{
  { "t", MsgType::UpdateEquipment },
  { "idx", 0 },
  { "data",
    { { "numChanges", 0 },
      { "inv", { { "entries", nlohmann::json::array() } } } } }
};

class FakeListener : public PartOne::Listener
{
public:
  static std::shared_ptr<FakeListener> New();

  void OnConnect(Networking::UserId userId) override;

  void OnDisconnect(Networking::UserId userId) override;

  void OnCustomPacket(Networking::UserId userId,
                      const simdjson::dom::element& content) override;

  bool OnMpApiEvent(const GameModeEvent& event) override;

  std::string str();

  void clear();

private:
  std::stringstream ss;
};
