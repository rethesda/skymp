#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

TEST_CASE(
  "Actor appearance, equipment and isRaceMenuOpen properties should present "
  "in changeForm",
  "[Actor]")
{
  MpActor actor(LocationalData(), FormCallbacks::DoNothing());
  Appearance appearance;
  appearance.raceId = 0x123;
  actor.SetAppearance(&appearance);
  actor.SetEquipment(Equipment());
  actor.SetRaceMenuOpen(true);

  REQUIRE(actor.GetChangeForm().appearanceDump == appearance.ToJson());
  REQUIRE(actor.GetChangeForm().equipment == Equipment());
  REQUIRE(actor.GetChangeForm().isRaceMenuOpen == true);
}

TEST_CASE("Actor should load be able to load appearance, equipment, "
          "isRaceMenuOpen and other properties from changeform",
          "[Actor]")
{
  const auto kExtraWornTrue = [] {
    Inventory::ExtraData extra;
    extra.worn_ = true;
    return extra;
  }();

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(0x12eb7, 1, kExtraWornTrue));

  MpChangeForm changeForm;
  changeForm.isRaceMenuOpen = true;
  changeForm.equipment = eq;
  changeForm.appearanceDump = Appearance().ToJson();
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.actorValues.healthPercentage = 1.0f;
  changeForm.actorValues.magickaPercentage = 0.9f;
  changeForm.actorValues.staminaPercentage = 0.0f;
  changeForm.isDead = true;
  changeForm.spawnPoint.cellOrWorldDesc.file = "yay";
  changeForm.spawnPoint.cellOrWorldDesc.shortFormId = 0xDEAD;
  changeForm.spawnPoint.pos = { 1, 2, 3 };
  changeForm.spawnPoint.rot = { 1, 2, 4 };
  changeForm.spawnDelay = 8.0f;
  changeForm.consoleCommandsAllowed = true;

  MpActor actor(LocationalData(), FormCallbacks::DoNothing(), 0xff000000);
  actor.ApplyChangeForm(changeForm);

  REQUIRE(actor.GetChangeForm().isRaceMenuOpen == true);
  REQUIRE(actor.GetChangeForm().equipment == eq);
  REQUIRE(actor.GetChangeForm().appearanceDump == Appearance().ToJson());
  REQUIRE(actor.GetChangeForm().actorValues.healthPercentage == 1.0f);
  REQUIRE(actor.GetChangeForm().actorValues.magickaPercentage == 0.9f);
  REQUIRE(actor.GetChangeForm().actorValues.staminaPercentage == 0.0f);
  REQUIRE(actor.GetChangeForm().isDead == true);
  REQUIRE(actor.GetChangeForm().spawnPoint.cellOrWorldDesc.file == "yay");
  REQUIRE(actor.GetChangeForm().spawnPoint.cellOrWorldDesc.shortFormId ==
          0xDEAD);
  REQUIRE(actor.GetChangeForm().spawnPoint.pos == NiPoint3{ 1, 2, 3 });
  REQUIRE(actor.GetChangeForm().spawnPoint.rot == NiPoint3{ 1, 2, 4 });
  REQUIRE(actor.GetChangeForm().spawnDelay == 8.0f);
  REQUIRE(actor.GetChangeForm().consoleCommandsAllowed == true);
}

TEST_CASE("Actor factions in changeForm", "[Actor]")
{
  PartOne p;
  p.worldState.espmFiles = { "Skyrim.esm" };

  MpActor actor(LocationalData(), FormCallbacks::DoNothing());

  Faction faction = Faction();
  faction.formDesc = FormDesc::FromFormId(0x000123, p.worldState.espmFiles);
  faction.rank = 0;

  actor.AddToFaction(faction, false);
  // Second time should be ignored
  actor.AddToFaction(faction, false);

  REQUIRE(actor.GetChangeForm().factions.has_value());
  REQUIRE(actor.GetChangeForm().factions.value().size() == 1);
  REQUIRE(actor.GetChangeForm().factions.value()[0].formDesc.shortFormId ==
          0x000123);

  REQUIRE(
    actor.IsInFaction(FormDesc::FromFormId(0x000223, p.worldState.espmFiles),
                      false) == false);
  REQUIRE(actor.IsInFaction(
    FormDesc::FromFormId(0x000123, p.worldState.espmFiles), false));

  actor.RemoveFromFaction(
    FormDesc::FromFormId(0x000003, p.worldState.espmFiles), false);
  REQUIRE(actor.GetChangeForm().factions.value().size() == 1);

  actor.RemoveFromFaction(
    FormDesc::FromFormId(0x000123, p.worldState.espmFiles), false);
  REQUIRE(actor.GetChangeForm().factions.value().size() == 0);
}
