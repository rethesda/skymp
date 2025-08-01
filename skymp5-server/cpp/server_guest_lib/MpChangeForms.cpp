#include "MpChangeForms.h"
#include "JsonUtils.h"
#include <spdlog/spdlog.h>

namespace {
std::vector<std::string> ToStringArray(const std::vector<FormDesc>& formDescs)
{
  std::vector<std::string> res(formDescs.size());
  std::transform(formDescs.begin(), formDescs.end(), res.begin(),
                 [](const FormDesc& v) { return v.ToString(); });
  return res;
}

std::vector<FormDesc> ToFormDescsArray(const std::vector<std::string>& strings)
{
  std::vector<FormDesc> res(strings.size());
  std::transform(strings.begin(), strings.end(), res.begin(),
                 [](const std::string& v) { return FormDesc::FromString(v); });
  return res;
}
}

nlohmann::json MpChangeForm::ToJson(const MpChangeForm& changeForm)
{
  auto res = nlohmann::json::object();
  res["recType"] = static_cast<int>(changeForm.recType);
  res["formDesc"] = changeForm.formDesc.ToString();
  res["baseDesc"] = changeForm.baseDesc.ToString();
  res["position"] = { changeForm.position[0], changeForm.position[1],
                      changeForm.position[2] };
  res["angle"] = { changeForm.angle[0], changeForm.angle[1],
                   changeForm.angle[2] };
  res["worldOrCellDesc"] = changeForm.worldOrCellDesc.ToString();
  res["inv"] = changeForm.inv.ToJson();
  res["isHarvested"] = changeForm.isHarvested;
  res["isOpen"] = changeForm.isOpen;
  res["baseContainerAdded"] = changeForm.baseContainerAdded;
  res["nextRelootDatetime"] = changeForm.nextRelootDatetime;
  res["isDisabled"] = changeForm.isDisabled;
  res["profileId"] = changeForm.profileId;
  res["isDeleted"] = changeForm.isDeleted;
  res["count"] = changeForm.count;
  res["isRaceMenuOpen"] = changeForm.isRaceMenuOpen;
  res["dynamicFields"] = changeForm.dynamicFields.GetAsJson();

  if (changeForm.appearanceDump.empty()) {
    res["appearanceDump"] = nullptr;
  } else {
    res["appearanceDump"] = nlohmann::json::parse(changeForm.appearanceDump);
  }

  res["equipmentDump"] = changeForm.equipment.ToJson();

  res["learnedSpells"] = changeForm.learnedSpells.GetLearnedSpells();

  res["healthPercentage"] = changeForm.actorValues.healthPercentage;
  res["magickaPercentage"] = changeForm.actorValues.magickaPercentage;
  res["staminaPercentage"] = changeForm.actorValues.staminaPercentage;

  res["healthRespawnPercentage"] = changeForm.healthRespawnPercentage;
  res["magickaRespawnPercentage"] = changeForm.magickaRespawnPercentage;
  res["staminaRespawnPercentage"] = changeForm.staminaRespawnPercentage;

  res["isDead"] = changeForm.isDead;

  res["consoleCommandsAllowed"] = changeForm.consoleCommandsAllowed;

  res["spawnPoint_pos"] = { changeForm.spawnPoint.pos[0],
                            changeForm.spawnPoint.pos[1],
                            changeForm.spawnPoint.pos[2] };
  res["spawnPoint_rot"] = { changeForm.spawnPoint.rot[0],
                            changeForm.spawnPoint.rot[1],
                            changeForm.spawnPoint.rot[2] };
  res["spawnPoint_cellOrWorldDesc"] =
    changeForm.spawnPoint.cellOrWorldDesc.ToString();

  res["spawnDelay"] = changeForm.spawnDelay;
  res["effects"] = changeForm.activeMagicEffects.ToJson();

  if (!changeForm.templateChain.empty()) {
    res["templateChain"] = ToStringArray(changeForm.templateChain);
  }

  // TODO: uncomment when add script vars save feature
  // if (changeForm.lastAnimation.has_value()) {
  //   res["lastAnimation"] = *changeForm.lastAnimation;
  // }

  if (changeForm.setNodeTextureSet.has_value()) {
    res["setNodeTextureSet"] = *changeForm.setNodeTextureSet;
  }

  if (changeForm.setNodeScale.has_value()) {
    res["setNodeScale"] = *changeForm.setNodeScale;
  }

  if (changeForm.displayName.has_value()) {
    res["displayName"] = *changeForm.displayName;
  }

  if (changeForm.factions.has_value() &&
      !changeForm.factions.value().empty()) {
    auto factionsJson = nlohmann::json::array();
    for (int i = 0; i < static_cast<int>(changeForm.factions.value().size());
         ++i) {
      nlohmann::json obj = {
        { "formDesc", changeForm.factions.value()[i].formDesc.ToString() },
        { "rank", (uint32_t)changeForm.factions.value()[i].rank }
      };
      factionsJson.push_back(obj);
    }
    res["factions"] = { { "entries", factionsJson } };
  }

  return res;
}

MpChangeForm MpChangeForm::JsonToChangeForm(simdjson::dom::element& element)
{
  static const JsonPointer recType("recType");
  static const JsonPointer formDesc("formDesc");
  static const JsonPointer baseDesc("baseDesc");
  static const JsonPointer position("position");
  static const JsonPointer angle("angle");
  static const JsonPointer worldOrCellDesc("worldOrCellDesc");
  static const JsonPointer inv("inv");
  static const JsonPointer isHarvested("isHarvested");
  static const JsonPointer isOpen("isOpen");
  static const JsonPointer baseContainerAdded("baseContainerAdded");
  static const JsonPointer nextRelootDatetime("nextRelootDatetime");
  static const JsonPointer isDisabled("isDisabled");
  static const JsonPointer profileId("profileId");
  static const JsonPointer isDeleted("isDeleted");
  static const JsonPointer count("count");
  static const JsonPointer isRaceMenuOpen("isRaceMenuOpen");
  static const JsonPointer appearanceDump("appearanceDump");
  static const JsonPointer equipmentDump("equipmentDump");
  static const JsonPointer learnedSpells("learnedSpells");
  static const JsonPointer dynamicFields("dynamicFields");
  static const JsonPointer healthPercentage("healthPercentage");
  static const JsonPointer magickaPercentage("magickaPercentage");
  static const JsonPointer staminaPercentage("staminaPercentage");
  static const JsonPointer isDead("isDead");
  static const JsonPointer consoleCommandsAllowed("consoleCommandsAllowed");
  static const JsonPointer spawnPointPos("spawnPoint_pos");
  static const JsonPointer spawnPointRot("spawnPoint_rot");
  static const JsonPointer spawnPointCellOrWorldDesc(
    "spawnPoint_cellOrWorldDesc");
  static const JsonPointer spawnDelay("spawnDelay");
  static const JsonPointer effects("effects");
  static const JsonPointer templateChain("templateChain");
  static const JsonPointer lastAnimation("lastAnimation");
  static const JsonPointer setNodeTextureSet("setNodeTextureSet");
  static const JsonPointer setNodeScale("setNodeScale");
  static const JsonPointer displayName("displayName");
  static const JsonPointer factions("factions");
  static const JsonPointer healthRespawnPercentage("healthRespawnPercentage");
  static const JsonPointer magickaRespawnPercentage(
    "magickaRespawnPercentage");
  static const JsonPointer staminaRespawnPercentage(
    "staminaRespawnPercentage");

  MpChangeForm res;
  ReadEx(element, recType, &res.recType);

  const char* tmp;
  simdjson::dom::element jTmp;

  ReadEx(element, formDesc, &tmp);
  res.formDesc = FormDesc::FromString(tmp);

  ReadEx(element, baseDesc, &tmp);
  res.baseDesc = FormDesc::FromString(tmp);

  ReadEx(element, position, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.position[i]);
  }

  ReadEx(element, angle, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.angle[i]);
  }

  ReadEx(element, worldOrCellDesc, &tmp);
  res.worldOrCellDesc = FormDesc::FromString(tmp);

  ReadEx(element, inv, &jTmp);
  res.inv = Inventory::FromJson(jTmp);

  ReadEx(element, isHarvested, &res.isHarvested);
  ReadEx(element, isOpen, &res.isOpen);
  ReadEx(element, baseContainerAdded, &res.baseContainerAdded);
  ReadEx(element, nextRelootDatetime, &res.nextRelootDatetime);
  ReadEx(element, isDisabled, &res.isDisabled);
  ReadEx(element, profileId, &res.profileId);

  if (element.at_pointer(isDeleted.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, isDeleted, &res.isDeleted);
  }

  if (element.at_pointer(count.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, count, &res.count);
  }

  ReadEx(element, isRaceMenuOpen, &res.isRaceMenuOpen);

  ReadEx(element, appearanceDump, &jTmp);
  res.appearanceDump = simdjson::minify(jTmp);
  if (res.appearanceDump == "null") {
    res.appearanceDump.clear();
  }

  ReadEx(element, equipmentDump, &jTmp);
  std::string eqDump = simdjson::minify(jTmp);
  if (eqDump == "null") {
    eqDump.clear();
  } else {
    auto equipment = nlohmann::json::parse(eqDump);
    if (!equipment.contains("numChanges")) {
      equipment["numChanges"] = 0;
      eqDump = equipment.dump();
      spdlog::info("MpChangeForm::JsonToChangeForm {} - Missing 'numChanges' "
                   "key, setting to 0",
                   res.formDesc.ToString());
    }
  }

  if (eqDump.size() > 0) {
    res.equipment = Equipment::FromJson(nlohmann::json::parse(eqDump));
  } else {
    res.equipment = Equipment();
  }

  if (element.at_pointer(learnedSpells.GetData()).error() ==
      simdjson::error_code::SUCCESS) {

    std::vector<LearnedSpells::Data::key_type> learnedSpellsData;

    ReadVector(element, learnedSpells, &learnedSpellsData);

    for (const auto spellId : learnedSpellsData) {
      res.learnedSpells.LearnSpell(spellId);
    }
  }

  ReadEx(element, healthPercentage, &res.actorValues.healthPercentage);
  ReadEx(element, magickaPercentage, &res.actorValues.magickaPercentage);
  ReadEx(element, staminaPercentage, &res.actorValues.staminaPercentage);

  if (element.at_pointer(healthRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, healthRespawnPercentage, &res.healthRespawnPercentage);
  }

  if (element.at_pointer(magickaRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, magickaRespawnPercentage, &res.magickaRespawnPercentage);
  }

  if (element.at_pointer(staminaRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, staminaRespawnPercentage, &res.staminaRespawnPercentage);
  }

  ReadEx(element, isDead, &res.isDead);

  if (element.at_pointer(consoleCommandsAllowed.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, consoleCommandsAllowed, &res.consoleCommandsAllowed);
  }

  simdjson::dom::element jDynamicFields;
  ReadEx(element, dynamicFields, &jDynamicFields);
  res.dynamicFields = DynamicFields::FromJson(nlohmann::json::parse(
    static_cast<std::string>(simdjson::minify(jDynamicFields))));

  ReadEx(element, spawnPointPos, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.spawnPoint.pos[i]);
  }

  ReadEx(element, spawnPointRot, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.spawnPoint.rot[i]);
  }

  ReadEx(element, spawnPointCellOrWorldDesc, &tmp);
  res.spawnPoint.cellOrWorldDesc = FormDesc::FromString(tmp);

  ReadEx(element, spawnDelay, &res.spawnDelay);

  if (element.at_pointer(effects.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, effects, &jTmp);
    res.activeMagicEffects = ActiveMagicEffectsMap::FromJson(jTmp);
  }

  if (element.at_pointer(templateChain.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    std::vector<std::string> data;
    ReadVector(element, templateChain, &data);
    res.templateChain = ToFormDescsArray(data);
  }

  if (element.at_pointer(lastAnimation.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* tmp;
    ReadEx(element, lastAnimation, &tmp);
    res.lastAnimation = tmp;
  }

  if (element.at_pointer(setNodeTextureSet.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    simdjson::dom::element data;
    ReadEx(element, setNodeTextureSet, &data);

    if (res.setNodeTextureSet == std::nullopt) {
      res.setNodeTextureSet = std::map<std::string, std::string>();
    }

    for (auto [key, value] : data.get_object()) {
      std::string keyStr = key.data();
      std::string valueStr = value.get_string().value().data();
      res.setNodeTextureSet->emplace(keyStr, valueStr);
    }
  }

  if (element.at_pointer(setNodeScale.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    simdjson::dom::element data;
    ReadEx(element, setNodeScale, &data);

    if (res.setNodeScale == std::nullopt) {
      res.setNodeScale = std::map<std::string, float>();
    }

    for (auto [key, value] : data.get_object()) {
      std::string keyStr = key.data();
      float valueFloat = static_cast<float>(value.get_double().value());
      res.setNodeScale->emplace(keyStr, valueFloat);
    }
  }

  if (element.at_pointer(displayName.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* tmp;
    ReadEx(element, displayName, &tmp);
    res.displayName = tmp;
  }

  if (element.at_pointer(factions.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, factions, &jTmp);
    static const JsonPointer entries("entries");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<Faction> factions = std::vector<Faction>(parsedEntries.size());

    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      auto& jEntry = parsedEntries[i];

      static JsonPointer rank("rank");
      Faction fact = Faction();
      const char* tmp;
      ReadEx(jEntry, formDesc, &tmp);
      fact.formDesc = FormDesc::FromString(tmp);

      uint32_t rankTemp = 0;
      ReadEx(jEntry, rank, &rankTemp);
      fact.rank = rankTemp;

      factions[i] = fact;
    }

    res.factions = factions;
  }

  return res;
}

void LearnedSpells::LearnSpell(const Data::key_type spellId)
{
  _learnedSpellIds.emplace(spellId);
}

void LearnedSpells::ForgetSpell(const Data::key_type spellId)
{
  _learnedSpellIds.erase(spellId);
}

size_t LearnedSpells::Count() const noexcept
{
  return _learnedSpellIds.size();
}

bool LearnedSpells::IsSpellLearned(const Data::key_type spellId) const
{
  return _learnedSpellIds.count(spellId) != 0;
}

std::vector<LearnedSpells::Data::key_type> LearnedSpells::GetLearnedSpells()
  const
{
  return std::vector(_learnedSpellIds.begin(), _learnedSpellIds.end());
}
