#include "MpObjectReference.h"
#include "ChangeFormGuard.h"
#include "EvaluateTemplate.h"
#include "FormCallbacks.h"
#include "GetWeightFromRecord.h"
#include "Inventory.h"
#include "LeveledListUtils.h"
#include "MathUtils.h"
#include "MessageBase.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "Primitive.h"
#include "ScopedTask.h"
#include "ScriptVariablesHolder.h"
#include "TimeUtils.h"
#include "UpdatePropertyMessage.h"
#include "WorldState.h"
#include "gamemode_events/ActivateEvent.h"
#include "gamemode_events/PutItemEvent.h"
#include "gamemode_events/TakeItemEvent.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/Convert.h"
#include "libespm/GroupUtils.h"
#include "libespm/Utils.h"
#include "papyrus-vm/Reader.h"
#include "papyrus-vm/Utils.h" // stricmp
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/EspmGameObject.h"
#include "script_storages/IScriptStorage.h"
#include <antigo/Context.h>
#include <antigo/ResolvedContext.h>
#include <map>
#include <numeric>
#include <optional>

#include "OpenContainerMessage.h"
#include "SetInventoryMessage.h"
#include "TeleportMessage.h"

#include "script_classes/PapyrusObjectReference.h" // kOriginalNameExpression

constexpr uint32_t kPlayerCharacterLevel = 1;

UpdatePropertyMessage MpObjectReference::CreatePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  return PreparePropertyMessage_(self, name, valueDump);
}

UpdatePropertyMessage MpObjectReference::PreparePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  UpdatePropertyMessage res;

  std::string baseRecordType;

  auto& loader = self->GetParent()->GetEspm();
  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (base.rec) {
    baseRecordType = base.rec->GetType().ToString();
  }

  res.idx = self->GetIdx();
  res.propName = name;
  res.refrId = self->GetFormId();
  res.dataDump = valueDump;

  // See 'perf: improve game framerate #1186'
  // Client needs to know if it is DOOR or not
  if (baseRecordType == "DOOR") {
    res.baseRecordType = baseRecordType;
  }

  return res;
}

// TODO: de-duplicate code of
// OccupantDisableEventSink/OccupantDisableEventSink, the only difference is
// base classes

class OccupantDestroyEventSink : public MpActor::DestroyEventSink
{
public:
  OccupantDestroyEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDestroy(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

class OccupantDisableEventSink : public MpActor::DisableEventSink
{
public:
  OccupantDisableEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDisable(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

namespace {
std::pair<int16_t, int16_t> GetGridPos(const NiPoint3& pos) noexcept
{
  return { int16_t(pos.x / 4096), int16_t(pos.y / 4096) };
}
}

struct AnimGraphHolder
{
  AnimGraphHolder() { boolVariables.fill(false); }

  std::array<bool, static_cast<size_t>(AnimationVariableBool::kNumVariables)>
    boolVariables;
};

struct ScriptState
{
  std::map<std::string, std::shared_ptr<ScriptVariablesHolder>> varHolders;
};

struct PrimitiveData
{
  NiPoint3 boundsDiv2;
  GeoProc::GeoPolygonProc polygonProc;
};

struct MpObjectReference::Impl
{
public:
  bool onInitEventSent = false;
  bool scriptsInited = false;
  std::unique_ptr<ScriptState> scriptState;
  AnimGraphHolder animGraphHolder;
  std::optional<PrimitiveData> primitive;
  bool teleportFlag = false;
  bool setPropertyCalled = false;
};

namespace {
ChangeFormGuard::Mode MakeMode(bool isLocationSaveNeeded,
                               SetPosMode setPosMode)
{
  if (isLocationSaveNeeded) {
    return ChangeFormGuard::Mode::RequestSave;
  }

  switch (setPosMode) {
    case SetPosMode::CalledByUpdateMovement:
      return ChangeFormGuard::Mode::NoRequestSave;
    case SetPosMode::Other:
      return ChangeFormGuard::Mode::RequestSave;
    default:
      spdlog::critical("Invalid SetPosMode");
      std::terminate();
      return ChangeFormGuard::Mode::RequestSave;
  }
}

MpChangeForm MakeChangeForm(const LocationalData& locationalData)
{
  MpChangeForm changeForm;
  changeForm.position = locationalData.pos;
  changeForm.angle = locationalData.rot;
  changeForm.worldOrCellDesc = locationalData.cellOrWorldDesc;
  return changeForm;
}
}

MpObjectReference::MpObjectReference(
  const LocationalData& locationalData_, const FormCallbacks& callbacks_,
  uint32_t baseId_, std::string baseType_,
  std::optional<NiPoint3> primitiveBoundsDiv2)
  : callbacks(new FormCallbacks(callbacks_))
  , baseId(baseId_)
  , baseType(baseType_)
  , ChangeFormGuard(MakeChangeForm(locationalData_), this)
{
  pImpl.reset(new Impl);
  asObjectReference = this;

  if (primitiveBoundsDiv2)
    SetPrimitive(*primitiveBoundsDiv2);
}

const NiPoint3& MpObjectReference::GetPos() const
{
  return ChangeForm().position;
}

const NiPoint3& MpObjectReference::GetAngle() const
{
  return ChangeForm().angle;
}

const FormDesc& MpObjectReference::GetCellOrWorld() const
{
  return ChangeForm().worldOrCellDesc;
}

const uint32_t& MpObjectReference::GetBaseId() const
{
  return baseId;
}

const std::string& MpObjectReference::GetBaseType() const
{
  return baseType;
}

const Inventory& MpObjectReference::GetInventory() const
{
  return ChangeForm().inv;
}

const bool& MpObjectReference::IsHarvested() const
{
  return ChangeForm().isHarvested;
}

const bool& MpObjectReference::IsOpen() const
{
  return ChangeForm().isOpen;
}

const bool& MpObjectReference::IsDisabled() const
{
  return ChangeForm().isDisabled;
}

const bool& MpObjectReference::IsDeleted() const
{
  return ChangeForm().isDeleted;
}

const uint32_t& MpObjectReference::GetCount() const
{
  return ChangeForm().count;
}

std::chrono::system_clock::duration MpObjectReference::GetRelootTime() const
{
  if (relootTimeOverride) {
    return *relootTimeOverride;
  }

  if (auto time = GetParent()->GetRelootTime(baseType)) {
    return *time;
  }

  if (!std::strcmp(baseType.data(), "FLOR") ||
      !std::strcmp(baseType.data(), "TREE")) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "DOOR")) {
    return std::chrono::seconds(3);
  } else if (espm::utils::IsItem(espm::Type{ baseType.data() })) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "CONT")) {
    return std::chrono::hours(1);
  }

  return std::chrono::hours(0);
}

bool MpObjectReference::GetAnimationVariableBool(const char* name) const
{
  AnimationVariableBool variable = AnimationVariableBool::kInvalidVariable;

  if (!Utils::stricmp(name, "bInJumpState")) {
    variable = AnimationVariableBool::kVariable_bInJumpState;
  } else if (!Utils::stricmp(name, "_skymp_isWeapDrawn")) {
    variable = AnimationVariableBool::kVariable__skymp_isWeapDrawn;
  } else if (!Utils::stricmp(name, "IsBlocking")) {
    variable = AnimationVariableBool::kVariable_IsBlocking;
  }

  if (variable == AnimationVariableBool::kInvalidVariable) {
    spdlog::warn("MpObjectReference::GetAnimationVariableBool - unknown "
                 "variable name: {}",
                 name);
    return false;
  }

  return pImpl->animGraphHolder.boolVariables[static_cast<size_t>(variable)];
}

bool MpObjectReference::IsPointInsidePrimitive(const NiPoint3& point) const
{
  if (pImpl->primitive) {
    return Primitive::IsInside(point, pImpl->primitive->polygonProc);
  }
  return false;
}

bool MpObjectReference::HasPrimitive() const
{
  return pImpl->primitive.has_value();
}

FormCallbacks MpObjectReference::GetCallbacks() const
{
  if (!callbacks)
    return FormCallbacks::DoNothing();
  return *callbacks;
}

bool MpObjectReference::HasScript(const char* name) const
{
  return ToGameObject()->HasScript(name);
}

bool MpObjectReference::IsActivationBlocked() const
{
  return activationBlocked;
}

bool MpObjectReference::GetTeleportFlag() const
{
  return pImpl->teleportFlag;
}

void MpObjectReference::VisitProperties(CreateActorMessage& message,
                                        VisitPropertiesMode mode)
{
  if (IsHarvested()) {
    message.props.isHarvested = true;
  }

  if (IsOpen()) {
    message.props.isOpen = true;
  }

  if (auto actor = AsActor(); actor && actor->IsDead()) {
    message.props.isDead = true;
  }

  if (mode == VisitPropertiesMode::All && !GetInventory().IsEmpty()) {
    message.props.inventory = GetInventory();
  }

  if (IsEspmForm() && IsDisabled()) {
    message.props.isDisabled = true;
  }

  if (ChangeForm().lastAnimation.has_value()) {
    message.props.lastAnimation = *ChangeForm().lastAnimation;
  }

  if (ChangeForm().setNodeScale.has_value()) {
    std::vector<SetNodeScaleEntry> setNodeScale;
    for (auto& [nodeName, scale] : *ChangeForm().setNodeScale) {
      SetNodeScaleEntry setNodeScaleEntry;
      setNodeScaleEntry.nodeName = nodeName;
      setNodeScaleEntry.scale = scale;
      setNodeScale.push_back(setNodeScaleEntry);
    }
    message.props.setNodeScale = std::move(setNodeScale);
  }

  if (ChangeForm().setNodeTextureSet.has_value()) {
    std::vector<SetNodeTextureSetEntry> setNodeTextureSet;
    for (auto& [nodeName, textureSetId] : *ChangeForm().setNodeTextureSet) {
      SetNodeTextureSetEntry setNodeTextureSetEntry;
      setNodeTextureSetEntry.nodeName = nodeName;
      setNodeTextureSetEntry.textureSetId =
        FormDesc::FromString(textureSetId).ToFormId(GetParent()->espmFiles);
      setNodeTextureSet.push_back(setNodeTextureSetEntry);
    }
    message.props.setNodeTextureSet = std::move(setNodeTextureSet);
  }

  if (ChangeForm().displayName.has_value()) {
    const std::string& raw = *ChangeForm().displayName;
    if (raw != PapyrusObjectReference::kOriginalNameExpression) {
      message.props.displayName = raw;
    }
  }

  // Property flags (isVisibleByOwner, isVisibleByNeighbor) are expected to be
  // checked by a caller (PartOne.cpp in this case)
  ChangeForm().dynamicFields.ForEachValueDump(
    [&](const std::string& propName, const std::string& valueDump) {
      CustomPropsEntry customPropsEntry;
      customPropsEntry.propName = propName;
      customPropsEntry.propValueJsonDump = valueDump;
      message.customPropsJsonDumps.push_back(customPropsEntry);
    });
}

void MpObjectReference::Activate(MpObjectReference& activationSource,
                                 bool defaultProcessingOnly,
                                 bool isSecondActivation)
{
  if (spdlog::should_log(spdlog::level::trace)) {
    for (auto& script : ListActivePexInstances()) {
      spdlog::trace("MpObjectReference::Activate {:x} - found script {}",
                    GetFormId(), script->GetSourcePexName());
    }
  }

  if (auto worldState = activationSource.GetParent(); worldState->HasEspm()) {
    CheckInteractionAbility(activationSource);

    // Pillars puzzle Bleak Falls Barrow
    bool workaroundBypassParentsCheck = &activationSource == this;

    // Block if only activation parents can activate this
    auto refrId = GetFormId();
    if (!workaroundBypassParentsCheck && IsEspmForm() && !AsActor()) {
      auto lookupRes = worldState->GetEspm().GetBrowser().LookupById(refrId);
      auto data = espm::GetData<espm::REFR>(refrId, worldState);
      auto it = std::find_if(
        data.activationParents.begin(), data.activationParents.end(),
        [&](const espm::REFR::ActivationParentInfo& info) {
          return lookupRes.ToGlobalId(info.refrId) ==
            activationSource.GetFormId();
        });
      if (it == data.activationParents.end()) {
        if (data.isParentActivationOnly) {
          throw std::runtime_error(
            "Only activation parents can activate this object");
        }
      }
    }
  }

  if (isSecondActivation) {
    bool processed = ProcessActivateSecond(activationSource);
    if (processed) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("SkympOnActivateClose", &arg, 1);
    }
  } else {
    ActivateEvent activateEvent(GetFormId(), activationSource.GetFormId());

    bool activationBlockedByMpApi = !activateEvent.Fire(GetParent());

    if (!activationBlockedByMpApi &&
        (!activationBlocked || defaultProcessingOnly)) {
      ProcessActivateNormal(activationSource);
      ActivateChilds();
    } else {
      spdlog::trace(
        "Activation of form {:#x} has been blocked. Reasons: "
        "blocked by MpApi={}, form is blocked={}, defaultProcessingOnly={}",
        GetFormId(), activationBlockedByMpApi, activationBlocked,
        defaultProcessingOnly);
    }

    if (!defaultProcessingOnly) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("OnActivate", &arg, 1);
    }
  }
}

void MpObjectReference::Disable()
{
  if (ChangeForm().isDisabled) {
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = true; });

  if (!IsEspmForm() || AsActor()) {
    RemoveFromGridAndUnsubscribeAll();
  }
}

void MpObjectReference::Enable()
{
  if (!ChangeForm().isDisabled) {
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = false; });

  if (!IsEspmForm() || AsActor()) {
    ForceSubscriptionsUpdate();
  }
}

void MpObjectReference::SetPos(const NiPoint3& newPos, SetPosMode setPosMode)
{
  auto oldGridPos = GetGridPos(ChangeForm().position);
  auto newGridPos = GetGridPos(newPos);

  EditChangeForm(
    [&newPos](MpChangeFormREFR& changeForm) { changeForm.position = newPos; },
    MakeMode(IsLocationSavingNeeded(), setPosMode));

  if (oldGridPos != newGridPos || !everSubscribedOrListened)
    ForceSubscriptionsUpdate();

  if (!IsDisabled()) {
    if (emittersWithPrimitives) {
      if (!primitivesWeAreInside) {
        primitivesWeAreInside.reset(new std::set<MpObjectReference*>);
      }

      for (auto& [emitterRefr, wasInside] : *emittersWithPrimitives) {
        bool inside = emitterRefr->IsPointInsidePrimitive(newPos);
        if (wasInside != inside) {
          wasInside = inside;
          auto me = ToVarValue();

          auto wst = GetParent();
          auto id = emitterRefr->GetFormId();
          auto myId = GetFormId();
          wst->SetTimer(std::chrono::seconds(0))
            .Then([wst, id, inside, me, myId, this](Viet::Void) {
              if (wst->LookupFormById(myId).get() != this) {
                wst->logger->error("Refr pointer expired", id);
                return;
              }
              auto& emitter = wst->LookupFormById(id);
              MpObjectReference* emitterRefr =
                emitter ? emitter->AsObjectReference() : nullptr;
              if (!emitterRefr) {
                wst->logger->error("Emitter not found in timer ({0:x})", id);
                return;
              }
              emitterRefr->SendPapyrusEvent(
                inside ? "OnTriggerEnter" : "OnTriggerLeave", &me, 1);
            });

          if (inside) {
            primitivesWeAreInside->insert(emitterRefr);
          } else {
            primitivesWeAreInside->erase(emitterRefr);
          }
        }
      }
    }

    if (primitivesWeAreInside) {
      auto me = ToVarValue();

      // May be modified inside loop, so copying
      const auto set = *primitivesWeAreInside;

      for (MpObjectReference* emitterRefr : set) {
        emitterRefr->SendPapyrusEvent("OnTrigger", &me, 1);
      }
    }
  }
}

void MpObjectReference::SetAngle(const NiPoint3& newAngle,
                                 SetAngleMode setAngleMode)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.angle = newAngle; },
    MakeMode(IsLocationSavingNeeded(), setAngleMode));
}

void MpObjectReference::SetHarvested(bool harvested)
{
  if (harvested != ChangeForm().isHarvested) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.isHarvested = harvested;
    });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isHarvested",
                             harvested ? "true" : "false"),
      true);
  }
}

void MpObjectReference::SetOpen(bool open)
{
  if (open != ChangeForm().isOpen) {
    EditChangeForm(
      [&](MpChangeFormREFR& changeForm) { changeForm.isOpen = open; });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isOpen", open ? "true" : "false"), true);
  }
}

void MpObjectReference::PutItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (this->occupant != &ac) {
    std::stringstream err;
    err << std::hex << "Actor 0x" << ac.GetFormId() << " doesn't occupy ref 0x"
        << GetFormId();
    throw std::runtime_error(err.str());
  }

  PutItemEvent putItemEvent(&ac, this, e);
  putItemEvent.Fire(GetParent());
}

void MpObjectReference::TakeItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (this->occupant != &ac) {
    std::stringstream err;
    err << std::hex << "Actor 0x" << ac.GetFormId() << " doesn't occupy ref 0x"
        << GetFormId();
    throw std::runtime_error(err.str());
  }

  TakeItemEvent takeItemEvent(&ac, this, e);
  takeItemEvent.Fire(GetParent());
}

void MpObjectReference::SetRelootTime(
  std::chrono::system_clock::duration newRelootTime)
{
  relootTimeOverride = newRelootTime;
}

void MpObjectReference::SetChanceNoneOverride(uint8_t newChanceNone)
{
  chanceNoneOverride.reset(new uint8_t(newChanceNone));
}

void MpObjectReference::SetCellOrWorld(const FormDesc& newWorldOrCell)
{
  SetCellOrWorldObsolete(newWorldOrCell);
  ForceSubscriptionsUpdate();
}

void MpObjectReference::SetActivationBlocked(bool blocked)
{
  // TODO: Save
  activationBlocked = blocked;
}

void MpObjectReference::ForceSubscriptionsUpdate()
{
  auto worldState = GetParent();
  if (!worldState || IsDisabled()) {
    return;
  }
  InitListenersAndEmitters();

  auto worldOrCell = GetCellOrWorld().ToFormId(worldState->espmFiles);

  auto& gridInfo = worldState->grids[worldOrCell];
  MoveOnGrid(*gridInfo.grid);

  auto& was = *this->listeners;
  auto pos = GetGridPos(GetPos());
  auto& now =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);

  std::vector<MpObjectReference*> toRemove;
  std::set_difference(was.begin(), was.end(), now.begin(), now.end(),
                      std::inserter(toRemove, toRemove.begin()));
  for (auto listener : toRemove) {
    Unsubscribe(this, listener);
    // Unsubscribe from self is NEEDED. See comment below
    if (this != listener)
      Unsubscribe(listener, this);
  }

  std::vector<MpObjectReference*> toAdd;
  std::set_difference(now.begin(), now.end(), was.begin(), was.end(),
                      std::inserter(toAdd, toAdd.begin()));
  for (auto listener : toAdd) {
    Subscribe(this, listener);
    // Note: Self-subscription is OK this check is performed as we don't want
    // to self-subscribe twice! We have already been subscribed to self in
    // the last line of code
    if (this != listener)
      Subscribe(listener, this);
  }

  everSubscribedOrListened = true;
}

void MpObjectReference::SetPrimitive(const NiPoint3& boundsDiv2)
{
  auto vertices = Primitive::GetVertices(GetPos(), GetAngle(), boundsDiv2);
  pImpl->primitive =
    PrimitiveData{ boundsDiv2, Primitive::CreateGeoPolygonProc(vertices) };
}

void MpObjectReference::UpdateHoster(uint32_t newHosterId)
{
  auto hostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "true");
  auto notHostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "false");
  for (auto listener : this->GetActorListeners()) {
    if (newHosterId != 0 && newHosterId != listener->GetFormId()) {
      listener->SendToUser(hostedMsg, true);
    } else {
      listener->SendToUser(notHostedMsg, true);
    }
  }
}

void MpObjectReference::SetPropertyValueDump(const std::string& propertyName,
                                             const std::string& valueDump,
                                             bool isVisibleByOwner,
                                             bool isVisibleByNeighbor)
{
  auto msg = CreatePropertyMessage_(this, propertyName.c_str(), valueDump);
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName, valueDump);
  });
  if (isVisibleByNeighbor) {
    SendMessageToActorListeners(msg, true);
  } else if (isVisibleByOwner) {
    if (auto ac = AsActor()) {
      ac->SendToUser(msg, true);
    }
  }
  pImpl->setPropertyCalled = true;
}

void MpObjectReference::SetTeleportFlag(bool value)
{
  pImpl->teleportFlag = value;
}

void MpObjectReference::SetPosAndAngleSilent(const NiPoint3& pos,
                                             const NiPoint3& rot)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.position = pos;
      changeForm.angle = rot;
    },
    Mode::NoRequestSave);
}

void MpObjectReference::Delete()
{
  if (IsEspmForm()) {
    spdlog::warn("MpObjectReference::Delete {:x} - Attempt to delete non-FF "
                 "object, ignoring",
                 GetFormId());
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDeleted = true; });
  RemoveFromGridAndUnsubscribeAll();
}

void MpObjectReference::SetCount(uint32_t newCount)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.count = newCount; });
}

void MpObjectReference::SetAnimationVariableBool(
  AnimationVariableBool animationVariableBool, bool value)
{
  auto i = static_cast<size_t>(animationVariableBool);
  pImpl->animGraphHolder.boolVariables[i] = value;
}

void MpObjectReference::SetInventory(const Inventory& inv)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv = inv;
  });
  SendInventoryUpdate();
}

void MpObjectReference::AddItem(uint32_t baseId, uint32_t count)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv.AddItem(baseId, count);
  });
  SendInventoryUpdate();

  if (auto worldState = GetParent(); worldState->HasEspm()) {
    espm::LookupResult lookupRes =
      worldState->GetEspm().GetBrowser().LookupById(baseId);

    if (lookupRes.rec) {
      auto baseItem = VarValue(std::make_shared<EspmGameObject>(lookupRes));
      auto itemCount = VarValue(static_cast<int32_t>(count));
      auto itemReference =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      auto sourceContainer =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      VarValue args[4] = { baseItem, itemCount, itemReference,
                           sourceContainer };
      SendPapyrusEvent("OnItemAdded", args, 4);
    } else {
      spdlog::warn("MpObjectReference::AddItem - failed to lookup item {:x}",
                   baseId);
    }
  }
}

void MpObjectReference::AddItems(const std::vector<Inventory::Entry>& entries)
{
  if (entries.size() > 0) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
      changeForm.inv.AddItems(entries);
    });
    SendInventoryUpdate();
  }

  // for (const auto& entri : entries) {
  //   auto baseItem = VarValue(static_cast<int32_t>(entri.baseId));
  //   auto itemCount = VarValue(static_cast<int32_t>(entri.count));
  //   auto itemReference = VarValue((IGameObject*)nullptr);
  //   auto sourceContainer = VarValue((IGameObject*)nullptr);
  //   VarValue args[4] = { baseItem, itemCount, itemReference,
  //   sourceContainer
  //   }; SendPapyrusEvent("OnItemAdded", args, 4);
  // }
}

void MpObjectReference::RemoveItem(uint32_t baseId, uint32_t count,
                                   MpObjectReference* target)
{
  RemoveItems({ { baseId, count } }, target);
}

void MpObjectReference::RemoveItems(
  const std::vector<Inventory::Entry>& entries, MpObjectReference* target)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.inv.RemoveItems(entries);
  });

  if (target)
    target->AddItems(entries);

  SendInventoryUpdate();

  if (GetBaseType() == "CONT") {
    if (GetInventory().IsEmpty()) {
      spdlog::info("MpObjectReference::RemoveItems - {:x} requesting reloot",
                   this->GetFormId());
      RequestReloot();
    }
  }
}

void MpObjectReference::RemoveAllItems(MpObjectReference* target)
{
  auto prevInv = GetInventory();
  RemoveItems(prevInv.entries, target);
}

void MpObjectReference::RelootContainer()
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = false;
    },
    Mode::NoRequestSave);
  EnsureBaseContainerAdded(*GetParent()->espm);
}

void MpObjectReference::RegisterProfileId(int32_t profileId)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  if (profileId < 0) {
    throw std::runtime_error(
      "Negative profileId passed to RegisterProfileId, should be >= 0");
  }

  auto currentProfileId = ChangeForm().profileId;
  auto formId = GetFormId();

  if (currentProfileId > 0) {
    worldState->actorIdByProfileId[currentProfileId].erase(formId);
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.profileId = profileId; });

  if (profileId > 0) {
    worldState->actorIdByProfileId[profileId].insert(formId);
  }
}

void MpObjectReference::RegisterPrivateIndexedProperty(
  const std::string& propertyName, const std::string& propertyValueStringified)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  bool (*isNull)(const std::string&) = [](const std::string& s) {
    return s == "null" || s == "undefined" || s == "" || s == "''" ||
      s == "\"\"";
  };

  auto currentValueStringified =
    ChangeForm().dynamicFields.GetValueDump(propertyName);
  auto formId = GetFormId();
  if (!isNull(currentValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, currentValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].erase(formId);
    spdlog::trace("MpObjectReference::RegisterPrivateIndexedProperty {:x} - "
                  "unregister {}",
                  formId, key);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName,
                                          propertyValueStringified);
  });

  if (!isNull(propertyValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, propertyValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].insert(formId);
    spdlog::trace(
      "MpObjectReference::RegisterPrivateIndexedProperty {:x} - register {}",
      formId, key);
  }
}

void MpObjectReference::Subscribe(MpObjectReference* emitter,
                                  MpObjectReference* listener)
{
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  if (!actorEmitter && !actorListener) {
    return;
  }

  if (!emitter->pImpl->onInitEventSent &&
      listener->GetChangeForm().profileId != -1) {
    emitter->pImpl->onInitEventSent = true;
    emitter->SendPapyrusEvent("OnInit");
    emitter->SendPapyrusEvent("OnCellLoad");
    emitter->SendPapyrusEvent("OnLoad");
  }

  const bool hasPrimitive = emitter->HasPrimitive();

  emitter->InitListenersAndEmitters();
  listener->InitListenersAndEmitters();

  auto [it, inserted] = emitter->listeners->insert(listener);

  if (actorListener && inserted) {
    emitter->actorListenerArray.push_back(actorListener);
  }

  listener->emitters->insert(emitter);

  if (!hasPrimitive) {
    emitter->callbacks->subscribe(emitter, listener);
  }

  if (hasPrimitive) {
    if (!listener->emittersWithPrimitives) {
      listener->emittersWithPrimitives.reset(
        new std::map<MpObjectReference*, bool>);
    }
    listener->emittersWithPrimitives->insert({ emitter, false });
  }
}

void MpObjectReference::Unsubscribe(MpObjectReference* emitter,
                                    MpObjectReference* listener)
{
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  bool bothNonActors = !actorEmitter && !actorListener;
  if (bothNonActors) {
    return;
  }

  const bool hasPrimitive = emitter->HasPrimitive();

  if (!hasPrimitive) {
    emitter->callbacks->unsubscribe(emitter, listener);
  }

  size_t numElementsErased = emitter->listeners->erase(listener);

  if (actorListener && numElementsErased > 0) {
    emitter->actorListenerArray.erase(
      std::remove(emitter->actorListenerArray.begin(),
                  emitter->actorListenerArray.end(), actorListener),
      emitter->actorListenerArray.end());
  }

  listener->emitters->erase(emitter);

  if (listener->emittersWithPrimitives && hasPrimitive) {
    listener->emittersWithPrimitives->erase(emitter);
  }
}

void MpObjectReference::SetLastAnimation(const std::string& lastAnimation)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.lastAnimation = lastAnimation;
  });
}

void MpObjectReference::SetNodeTextureSet(const std::string& node,
                                          const espm::LookupResult& textureSet,
                                          bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeTextureSet == std::nullopt) {
      changeForm.setNodeTextureSet = std::map<std::string, std::string>();
    }

    uint32_t textureSetId = textureSet.ToGlobalId(textureSet.rec->GetId());

    FormDesc textureSetFormDesc =
      FormDesc::FromFormId(textureSetId, GetParent()->espmFiles);

    changeForm.setNodeTextureSet->insert_or_assign(
      node, textureSetFormDesc.ToString());
  });
}

void MpObjectReference::SetNodeScale(const std::string& node, float scale,
                                     bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeScale == std::nullopt) {
      changeForm.setNodeScale = std::map<std::string, float>();
    }
    changeForm.setNodeScale->insert_or_assign(node, scale);
  });
}

void MpObjectReference::SetDisplayName(
  const std::optional<std::string>& newName)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.displayName = newName; });
}

const std::set<MpObjectReference*>& MpObjectReference::GetListeners() const
{
  static const std::set<MpObjectReference*> kEmptyListeners;
  return listeners ? *listeners : kEmptyListeners;
}

const std::vector<MpActor*>& MpObjectReference::GetActorListeners()
  const noexcept
{
  return actorListenerArray;
}

const std::set<MpObjectReference*>& MpObjectReference::GetEmitters() const
{
  static const std::set<MpObjectReference*> kEmptyEmitters;
  return emitters ? *emitters : kEmptyEmitters;
}

void MpObjectReference::RequestReloot(
  std::optional<std::chrono::system_clock::duration> time)
{
  if (!IsEspmForm()) {
    return;
  }

  if (GetParent()->IsRelootForbidden(baseType)) {
    return;
  }

  if (!time)
    time = GetRelootTime();

  if (!ChangeForm().nextRelootDatetime) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() + GetRelootTime());
    });

    GetParent()->RequestReloot(*this, *time);
  }
}

void MpObjectReference::DoReloot()
{
  if (ChangeForm().nextRelootDatetime) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = 0;
    });
    SetOpen(false);
    SetHarvested(false);
    RelootContainer();
  }
}

std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>>
MpObjectReference::GetNextRelootMoment() const
{
  std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>> res;
  if (ChangeForm().nextRelootDatetime) {
    res.reset(new std::chrono::time_point<std::chrono::system_clock>(
      std::chrono::system_clock::from_time_t(
        ChangeForm().nextRelootDatetime)));
  }
  return res;
}

MpChangeForm MpObjectReference::GetChangeForm() const
{
  MpChangeForm res = ChangeForm();

  if (GetParent() && !GetParent()->espmFiles.empty()) {
    res.formDesc = FormDesc::FromFormId(GetFormId(), GetParent()->espmFiles);
    res.baseDesc = FormDesc::FromFormId(GetBaseId(), GetParent()->espmFiles);
  } else {
    res.formDesc = res.baseDesc = FormDesc(GetFormId(), "");
  }

  return res;
}

void MpObjectReference::ApplyChangeForm(const MpChangeForm& changeForm)
{
  ANTIGO_CONTEXT_INIT(ctx);

  if (pImpl->setPropertyCalled) {
    GetParent()->logger->critical(
      "ApplyChangeForm called after SetProperty\n{}",
      ctx.Resolve().ToString());
    std::terminate();
  }

  blockSaving = true;
  Viet::ScopedTask<MpObjectReference> unblockTask(
    [](MpObjectReference& self) { self.blockSaving = false; }, *this);

  const auto currentBaseId = GetBaseId();
  const auto newBaseId = changeForm.baseDesc.ToFormId(GetParent()->espmFiles);
  if (currentBaseId != newBaseId) {
    spdlog::error("Anomaly, baseId should never change ({:x} => {:x})",
                  currentBaseId, newBaseId);
  }

  if (ChangeForm().formDesc != changeForm.formDesc) {
    throw std::runtime_error("Expected formDesc to be " +
                             ChangeForm().formDesc.ToString() +
                             ", but found " + changeForm.formDesc.ToString());
  }

  if (changeForm.profileId >= 0) {
    RegisterProfileId(changeForm.profileId);
  }

  changeForm.dynamicFields.ForEachValueDump(
    [&](const std::string& propertyName, const std::string& valueDump) {
      static const std::string kPrefix = GetPropertyPrefixPrivateIndexed();
      bool startsWith = propertyName.compare(0, kPrefix.size(), kPrefix) == 0;
      if (startsWith) {
        RegisterPrivateIndexedProperty(propertyName, valueDump);
      }
    });

  // See https://github.com/skyrim-multiplayer/issue-tracker/issues/42
  EditChangeForm(
    [&](MpChangeFormREFR& f) {
      f = changeForm;

      // Fix: RequestReloot doesn't work with non-zero 'nextRelootDatetime'
      f.nextRelootDatetime = 0;
    },
    Mode::NoRequestSave);
  if (changeForm.nextRelootDatetime) {
    auto tp =
      std::chrono::system_clock::from_time_t(changeForm.nextRelootDatetime);

    // Fix: Handle situations when the server is stopped at the 'tp' moment
    if (tp < std::chrono::system_clock::now()) {
      tp = std::chrono::system_clock::now() + std::chrono::milliseconds(1);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      tp - std::chrono::system_clock::now());
    RequestReloot(ms);
  }

  // Perform all required grid operations
  // Mirrors MpActor impl
  if (!AsActor()) {
    changeForm.isDisabled ? Disable() : Enable();
    SetCellOrWorldObsolete(changeForm.worldOrCellDesc);
    SetPos(changeForm.position);
  }
}

const DynamicFields& MpObjectReference::GetDynamicFields() const
{
  return ChangeForm().dynamicFields;
}

void MpObjectReference::SetCellOrWorldObsolete(const FormDesc& newWorldOrCell)
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  everSubscribedOrListened = false;
  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator != worldState->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.worldOrCellDesc = newWorldOrCell;
  });
}

void MpObjectReference::VisitNeighbours(const Visitor& visitor)
{
  if (IsDisabled()) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator == worldState->grids.end()) {
    return;
  }

  auto& grid = gridIterator->second;
  auto pos = GetGridPos(GetPos());
  auto& neighbours =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);
  for (auto neighbour : neighbours) {
    visitor(neighbour);
  }
}

void MpObjectReference::SendPapyrusEvent(const char* eventName,
                                         const VarValue* arguments,
                                         size_t argumentsCount)
{
  if (!pImpl->scriptsInited) {
    InitScripts();
    pImpl->scriptsInited = true;
  }
  return MpForm::SendPapyrusEvent(eventName, arguments, argumentsCount);
}

void MpObjectReference::Init(WorldState* parent, uint32_t formId,
                             bool hasChangeForm)
{
  MpForm::Init(parent, formId, hasChangeForm);

  // We should queue created form for saving as soon as it is initialized
  const auto mode = (!hasChangeForm && !IsEspmForm()) ? Mode::RequestSave
                                                      : Mode::NoRequestSave;

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.formDesc =
        FormDesc::FromFormId(formId, GetParent()->espmFiles);
    },
    mode);

  auto refrId = GetFormId();
  if (parent->HasEspm() && IsEspmForm() && !AsActor()) {
    auto lookupRes = parent->GetEspm().GetBrowser().LookupById(refrId);
    auto data = espm::GetData<espm::REFR>(refrId, parent);
    for (auto& info : data.activationParents) {
      auto activationParent = lookupRes.ToGlobalId(info.refrId);

      // Using WorldState for that, because we don't want search (potentially
      // load) other references during OnInit
      parent->activationChildsByActivationParent[activationParent].insert(
        { refrId, info.delay });
    }
  }
}

bool MpObjectReference::IsLocationSavingNeeded() const
{
  auto last = GetLastSaveRequestMoment();
  return !last ||
    std::chrono::system_clock::now() - *last > std::chrono::seconds(30);
}

void MpObjectReference::ProcessActivateNormal(
  MpObjectReference& activationSource)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    return spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                         "have base form, activationSource is {:x}",
                         GetFormId(), activationSource.GetFormId());
  }

  // Not sure if this is needed
  if (IsDeleted()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - deleted "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  // Not sure if this is needed
  if (IsDisabled()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - disabled "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  auto t = base.rec->GetType();

  bool pickable = espm::utils::Is<espm::TREE>(t) ||
    espm::utils::Is<espm::FLOR>(t) || espm::utils::IsItem(t);
  if (pickable && !IsHarvested()) {
    auto mapping = loader.GetBrowser().GetCombMapping(base.fileIdx);
    uint32_t resultItem = 0;
    if (espm::utils::Is<espm::TREE>(t)) {
      auto data =
        espm::Convert<espm::TREE>(base.rec)->GetData(compressedFieldsCache);
      resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
    }

    if (espm::utils::Is<espm::FLOR>(t)) {
      auto data =
        espm::Convert<espm::FLOR>(base.rec)->GetData(compressedFieldsCache);
      resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
    }

    if (espm::utils::Is<espm::LIGH>(t)) {
      auto res =
        espm::Convert<espm::LIGH>(base.rec)->GetData(compressedFieldsCache);
      bool isTorch = res.data.flags & espm::LIGH::Flags::CanBeCarried;
      if (!isTorch) {
        return;
      }
      resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
    }

    if (resultItem == 0) {
      resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
    }

    auto resultItemLookupRes = loader.GetBrowser().LookupById(resultItem);
    auto leveledItem = espm::Convert<espm::LVLI>(resultItemLookupRes.rec);
    if (leveledItem) {
      const auto kCountMult = 1;
      auto map = LeveledListUtils::EvaluateListRecurse(
        loader.GetBrowser(), resultItemLookupRes, kCountMult,
        kPlayerCharacterLevel, chanceNoneOverride.get());
      for (auto& p : map) {
        activationSource.AddItem(p.first, p.second);
      }
    } else {
      auto refrRecord = espm::Convert<espm::REFR>(
        loader.GetBrowser().LookupById(GetFormId()).rec);

      uint32_t countRecord =
        refrRecord ? refrRecord->GetData(compressedFieldsCache).count : 1;

      uint32_t countChangeForm = ChangeForm().count;

      constexpr uint32_t kCountDefault = 1;

      uint32_t resultingCount =
        std::max(kCountDefault, std::max(countRecord, countChangeForm));

      activationSource.AddItem(resultItem, resultingCount);
    }
    SetHarvested(true);
    RequestReloot();

    if (espm::utils::IsItem(t) && !IsEspmForm()) {
      spdlog::info("MpObjectReference::ProcessActivate - Deleting 0xff item");
      Delete();
    }
  } else if (t == espm::DOOR::kType) {
    auto lookupRes = loader.GetBrowser().LookupById(GetFormId());
    auto refrRecord = espm::Convert<espm::REFR>(lookupRes.rec);
    auto teleport = refrRecord->GetData(compressedFieldsCache).teleport;
    if (teleport) {
      if (!IsOpen()) {
        SetOpen(true);
        RequestReloot();
      }

      auto destinationId = lookupRes.ToGlobalId(teleport->destinationDoor);
      auto destination = loader.GetBrowser().LookupById(destinationId);
      auto destinationRecord = espm::Convert<espm::REFR>(destination.rec);
      if (!destinationRecord) {
        throw std::runtime_error(
          "No destination found for this teleport door");
      }

      auto teleportWorldOrCell = destination.ToGlobalId(
        GetWorldOrCell(loader.GetBrowser(), destinationRecord));

      static const auto kPi = std::acos(-1.f);
      const auto& pos = teleport->pos;
      const float rot[] = { teleport->rotRadians[0] / kPi * 180,
                            teleport->rotRadians[1] / kPi * 180,
                            teleport->rotRadians[2] / kPi * 180 };

      TeleportMessage msg;
      msg.idx = activationSource.GetIdx();
      std::copy(std::begin(pos), std::end(pos), msg.pos.begin());
      std::copy(std::begin(rot), std::end(rot), msg.rot.begin());
      msg.worldOrCell = teleportWorldOrCell;

      if (actorActivator) {
        actorActivator->SendToUser(msg, true);
      }

      activationSource.SetCellOrWorldObsolete(
        FormDesc::FromFormId(teleportWorldOrCell, worldState->espmFiles));
      activationSource.SetPos({ pos[0], pos[1], pos[2] });
      activationSource.SetAngle({ rot[0], rot[1], rot[2] });

    } else {
      SetOpen(!IsOpen());
    }
  } else if (t == espm::CONT::kType && actorActivator) {
    EnsureBaseContainerAdded(loader);

    constexpr float kOccupationReach = 512.f;

    if (CheckIfObjectCanStartOccupyThis(activationSource, kOccupationReach)) {
      if (this->occupant) {
        this->occupant->RemoveEventSink(this->occupantDestroySink);
        this->occupant->RemoveEventSink(this->occupantDisableSink);
      }
      SetOpen(true);
      actorActivator->SendToUser(
        CreatePropertyMessage_(this, "inventory",
                               GetInventory().ToJson().dump()),
        true);
      activationSource.SendOpenContainer(GetFormId());

      this->occupant = actorActivator;

      this->occupantDestroySink.reset(
        new OccupantDestroyEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDestroySink);

      this->occupantDisableSink.reset(
        new OccupantDisableEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDisableSink);
    }
  } else if (t == espm::ACTI::kType && actorActivator) {
    // SendOpenContainer being used to activate the object
    // TODO: rename SendOpenContainer to SendActivate
    activationSource.SendOpenContainer(GetFormId());
  } else if (t == "FURN" && actorActivator) {

    constexpr float kOccupationReach = 256.f;

    if (CheckIfObjectCanStartOccupyThis(activationSource, kOccupationReach)) {
      if (this->occupant) {
        this->occupant->RemoveEventSink(this->occupantDestroySink);
        this->occupant->RemoveEventSink(this->occupantDisableSink);
      }

      // SendOpenContainer being used to activate the object
      // TODO: rename SendOpenContainer to SendActivate
      activationSource.SendOpenContainer(GetFormId());

      this->occupant = actorActivator;

      this->occupantDestroySink.reset(
        new OccupantDestroyEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDestroySink);

      this->occupantDisableSink.reset(
        new OccupantDisableEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDisableSink);
    }
  }
}

bool MpObjectReference::ProcessActivateSecond(
  MpObjectReference& activationSource)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                  "have base form, activationSource is {:x}",
                  GetFormId(), activationSource.GetFormId());
    return false;
  }

  auto t = base.rec->GetType();

  if (t == espm::CONT::kType && actorActivator) {
    if (this->occupant == &activationSource) {
      SetOpen(false);
      this->occupant->RemoveEventSink(this->occupantDestroySink);
      this->occupant = nullptr;
      return true;
    }
  } else if (t == "FURN" && actorActivator) {
    if (this->occupant == &activationSource) {
      this->occupant->RemoveEventSink(this->occupantDestroySink);
      this->occupant = nullptr;
      return true;
    }
  }

  return false;
}

void MpObjectReference::ActivateChilds()
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto myFormId = GetFormId();

  for (auto& pair : worldState->activationChildsByActivationParent[myFormId]) {
    auto childRefrId = pair.first;
    auto delay = pair.second;

    auto delayMs = Viet::TimeUtils::To<std::chrono::milliseconds>(delay);
    worldState->SetTimer(delayMs).Then([worldState, childRefrId,
                                        myFormId](Viet::Void) {
      auto& childForm = worldState->LookupFormById(childRefrId);
      MpObjectReference* childRefr =
        childForm ? childForm->AsObjectReference() : nullptr;
      if (!childRefr) {
        spdlog::warn("MpObjectReference::ActivateChilds {:x} - Bad/missing "
                     "activation child {:x}",
                     myFormId, childRefrId);
        return;
      }

      // Not sure about activationSource and defaultProcessingOnly in this
      // case I'll try to keep vanilla scripts working
      childRefr->Activate(worldState->GetFormAt<MpObjectReference>(myFormId));
    });
  }
}

bool MpObjectReference::CheckIfObjectCanStartOccupyThis(
  MpObjectReference& activationSource, float occupationReach)
{
  if (!this->occupant) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - no occupant "
                 "(activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant->IsDisabled()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                 "disabled (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantPos = this->occupant->GetPos();
  auto distanceToOccupantSqr = (occupantPos - GetPos()).SqrLength();
  if (distanceToOccupantSqr > occupationReach * occupationReach) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is too "
                 "far away (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantCellOrWorld = this->occupant->GetCellOrWorld();
  if (occupantCellOrWorld != GetCellOrWorld()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is in "
                 "another cell/world (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant == &activationSource) {
    auto& loader = GetParent()->GetEspm();
    auto base = loader.GetBrowser().LookupById(GetBaseId());
    auto t = base.rec->GetType();
    auto actorActivator = activationSource.AsActor();
    if (t == "FURN" && actorActivator) {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x}). Blocking "
                   "because it's FURN",
                   GetFormId(), activationSource.GetFormId());
      return false;
    } else {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x})",
                   GetFormId(), activationSource.GetFormId());
      return true;
    }
  }

  spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
               "another object and is nearby (activationSource = {:x})",
               GetFormId(), activationSource.GetFormId());
  return false;
}

void MpObjectReference::RemoveFromGridAndUnsubscribeAll()
{
  auto worldOrCell = GetCellOrWorld().ToFormId(GetParent()->espmFiles);
  auto gridIterator = GetParent()->grids.find(worldOrCell);
  if (gridIterator != GetParent()->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  auto listenersCopy = GetListeners();
  for (auto listener : listenersCopy) {
    Unsubscribe(this, listener);
  }

  everSubscribedOrListened = false;
}

void MpObjectReference::UnsubscribeFromAll()
{
  auto emittersCopy = GetEmitters();
  for (auto emitter : emittersCopy)
    Unsubscribe(emitter, this);
}

void MpObjectReference::InitScripts()
{
  auto baseId = GetBaseId();
  if (!baseId || !GetParent()->espm) {
    return;
  }

  auto scriptStorage = GetParent()->GetScriptStorage();
  if (!scriptStorage) {
    return;
  }

  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  std::vector<std::string> scriptNames;

  auto& br = GetParent()->espm->GetBrowser();
  auto base = br.LookupById(baseId);
  auto refr = br.LookupById(GetFormId());
  for (auto record : { base.rec, refr.rec }) {
    if (!record) {
      continue;
    }

    std::optional<espm::ScriptData> scriptData;

    if (record == base.rec && record->GetType() == "NPC_") {
      auto baseId = base.ToGlobalId(base.rec->GetId());
      if (auto actor = AsActor()) {
        auto& templateChain = actor->GetTemplateChain();
        scriptData = EvaluateTemplate<espm::NPC_::UseScript>(
          GetParent(), baseId, templateChain,
          [&compressedFieldsCache](const auto& npcLookupRes,
                                   const auto& npcData) {
            espm::ScriptData scriptData;
            npcLookupRes.rec->GetScriptData(&scriptData,
                                            compressedFieldsCache);
            return scriptData;
          });
      }
    }

    if (!scriptData) {
      scriptData = espm::ScriptData();
      record->GetScriptData(&*scriptData, compressedFieldsCache);
    }

    auto& scriptsInStorage =
      GetParent()->GetScriptStorage()->ListScripts(false);
    for (auto& script : scriptData->scripts) {
      if (scriptsInStorage.count(
            { script.scriptName.begin(), script.scriptName.end() })) {

        if (std::count(scriptNames.begin(), scriptNames.end(),
                       script.scriptName) == 0) {
          scriptNames.push_back(script.scriptName);
        }
      } else if (auto wst = GetParent())
        wst->logger->warn("Script '{}' not found in the script storage",
                          script.scriptName);
    }
  }

  // A hardcoded hack to remove all scripts except SweetPie scripts from
  // exterior objects
  // TODO: make is a server setting with proper conditions or an API
  if (GetParent() && GetParent()->disableVanillaScriptsInExterior &&
      GetFormId() < 0x05000000) {
    auto cellOrWorld = GetCellOrWorld().ToFormId(GetParent()->espmFiles);
    auto lookupRes =
      GetParent()->GetEspm().GetBrowser().LookupById(cellOrWorld);
    if (lookupRes.rec && lookupRes.rec->GetType() == "WRLD") {
      spdlog::info("Skipping non-Sweet scripts for exterior form {:x}",
                   cellOrWorld);
      scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                       [](const std::string& val) {
                                         auto kPrefix = "Sweet";
                                         bool startsWith = val.size() >= 5 &&
                                           !memcmp(kPrefix, val.data(), 5);
                                         return !startsWith;
                                       }),
                        scriptNames.end());
    }
  }

  // A hardcoded hack to remove unsupported scripts
  // TODO: make is a server setting with proper conditions or an API
  auto isScriptEraseNeeded = [](const std::string& val) {
    // 1. GetStage in OnTrigger
    // 2. Unable to determine Actor for 'Game.GetPlayer' in 'OnLoad'
    const bool isRemoveNeeded =
      !Utils::stricmp(val.data(), "DA06PreRitualSceneTriggerScript") ||
      !Utils::stricmp(val.data(), "CritterSpawn");

    spdlog::info("Skipping script {}", val);

    return isRemoveNeeded;
  };
  scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                   isScriptEraseNeeded),
                    scriptNames.end());

  if (!scriptNames.empty()) {
    pImpl->scriptState = std::make_unique<ScriptState>();

    std::vector<VirtualMachine::ScriptInfo> scriptInfo;
    for (auto& scriptName : scriptNames) {
      auto scriptVariablesHolder = std::make_shared<ScriptVariablesHolder>(
        scriptName, base, refr, base.parent, &compressedFieldsCache,
        GetParent());
      scriptInfo.push_back({ scriptName, std::move(scriptVariablesHolder) });
    }

    GetParent()->GetPapyrusVm().AddObject(ToGameObject(), scriptInfo);
  }
}

void MpObjectReference::MoveOnGrid(GridImpl<MpObjectReference*>& grid)
{
  auto newGridPos = GetGridPos(GetPos());
  grid.Move(this, newGridPos.first, newGridPos.second);
}

void MpObjectReference::InitListenersAndEmitters()
{
  if (!listeners) {
    listeners.reset(new std::set<MpObjectReference*>);
    emitters.reset(new std::set<MpObjectReference*>);
    actorListenerArray.clear();
  }
}

void MpObjectReference::SendInventoryUpdate()
{
  constexpr int kChannelSetInventory = 0;
  auto actor = AsActor();
  if (actor) {
    SetInventoryMessage message;
    message.inventory = actor->GetInventory();
    actor->SendToUserDeferred(message, true, kChannelSetInventory, true);
  }
}

void MpObjectReference::SendOpenContainer(uint32_t targetId)
{
  auto actor = AsActor();
  if (actor) {
    OpenContainerMessage msg;
    msg.target = targetId;
    actor->SendToUser(msg, true);
  }
}

std::vector<espm::CONT::ContainerObject> GetOutfitObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  std::vector<espm::CONT::ContainerObject> res;

  if (auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec)) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    auto outfitId = EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto& npcLookupRes, const auto& npcData) {
        return npcLookupRes.ToGlobalId(npcData.defaultOutfitId);
      });

    auto outfitLookupRes =
      worldState->GetEspm().GetBrowser().LookupById(outfitId);
    auto outfit = espm::Convert<espm::OTFT>(outfitLookupRes.rec);
    auto outfitData =
      outfit ? outfit->GetData(compressedFieldsCache) : espm::OTFT::Data();

    for (uint32_t i = 0; i != outfitData.count; ++i) {
      auto outfitElementId = outfitLookupRes.ToGlobalId(outfitData.formIds[i]);
      res.push_back({ outfitElementId, 1 });
    }
  }
  return res;
}

std::vector<espm::CONT::ContainerObject> GetInventoryObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  auto baseContainer = espm::Convert<espm::CONT>(lookupRes.rec);
  if (baseContainer) {
    return baseContainer->GetData(compressedFieldsCache).objects;
  }

  auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec);
  if (baseNpc) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    return EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto&, const auto& npcData) { return npcData.objects; });
  }

  return {};
}

void MpObjectReference::AddContainerObject(
  const espm::CONT::ContainerObject& entry,
  std::map<uint32_t, uint32_t>* itemsToAdd)
{
  auto& espm = GetParent()->GetEspm();
  auto formLookupRes = espm.GetBrowser().LookupById(entry.formId);
  auto leveledItem = espm::Convert<espm::LVLI>(formLookupRes.rec);
  if (leveledItem) {
    constexpr uint32_t kCountMult = 1;
    auto map = LeveledListUtils::EvaluateListRecurse(
      espm.GetBrowser(), formLookupRes, kCountMult, kPlayerCharacterLevel,
      chanceNoneOverride.get());
    for (auto& p : map) {
      (*itemsToAdd)[p.first] += p.second;
    }
  } else {
    (*itemsToAdd)[entry.formId] += entry.count;
  }
}

void MpObjectReference::EnsureBaseContainerAdded(espm::Loader& espm)
{
  if (ChangeForm().baseContainerAdded) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto actor = AsActor();
  const std::vector<FormDesc> kEmptyTemplateChain;
  const std::vector<FormDesc>& templateChain =
    actor ? actor->GetTemplateChain() : kEmptyTemplateChain;

  auto lookupRes = espm.GetBrowser().LookupById(GetBaseId());

  std::map<uint32_t, uint32_t> itemsToAdd, itemsToEquip;

  auto inventoryObjects =
    GetInventoryObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : inventoryObjects) {
    AddContainerObject(entry, &itemsToAdd);
  }

  auto outfitObjects = GetOutfitObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : outfitObjects) {
    AddContainerObject(entry, &itemsToAdd);
    AddContainerObject(entry, &itemsToEquip);
  }
  if (actor) {
    Equipment eq;
    for (auto& p : itemsToEquip) {
      Inventory::Entry e;
      e.baseId = p.first;
      e.count = p.second;
      e.SetWorn(Inventory::Worn::Right);
      eq.inv.AddItems({ e });
    }
    actor->SetEquipment(eq);
  }

  std::vector<Inventory::Entry> entries;
  for (auto& p : itemsToAdd) {
    entries.push_back({ p.first, p.second });
  }
  AddItems(entries);

  if (!ChangeForm().baseContainerAdded) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
    });
  }
}

void MpObjectReference::CheckInteractionAbility(MpObjectReference& refr)
{
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto casterWorldId = refr.GetCellOrWorld().ToFormId(GetParent()->espmFiles);
  auto targetWorldId = GetCellOrWorld().ToFormId(GetParent()->espmFiles);

  auto casterWorld = loader.GetBrowser().LookupById(casterWorldId).rec;
  auto targetWorld = loader.GetBrowser().LookupById(targetWorldId).rec;

  if (targetWorld != casterWorld) {
    const char* casterWorldName =
      casterWorld ? casterWorld->GetEditorId(compressedFieldsCache) : "<null>";

    const char* targetWorldName =
      targetWorld ? targetWorld->GetEditorId(compressedFieldsCache) : "<null>";

    throw std::runtime_error(fmt::format(
      "WorldSpace doesn't match: caster is in {} ({:#x}), target is in "
      "{} ({:#x})",
      casterWorldName, casterWorldId, targetWorldName, targetWorldId));
  }
}

void MpObjectReference::SendMessageToActorListeners(const IMessageBase& msg,
                                                    bool reliable) const
{
  for (auto listener : GetActorListeners()) {
    listener->SendToUser(msg, true);
  }
}

void MpObjectReference::BeforeDestroy()
{
  if (this->occupant && this->occupantDestroySink) {
    this->occupant->RemoveEventSink(this->occupantDestroySink);
  }

  // Move far far away calling OnTriggerExit, unsubscribing, etc
  SetPos({ -1'000'000'000, 0, 0 });

  MpForm::BeforeDestroy();

  RemoveFromGridAndUnsubscribeAll();
}

float MpObjectReference::GetTotalItemWeight() const
{
  const auto& entries = GetInventory().entries;
  const auto calculateWeight = [this](float sum,
                                      const Inventory::Entry& entry) {
    const auto& espm = GetParent()->GetEspm();
    const auto* record = espm.GetBrowser().LookupById(entry.baseId).rec;
    if (!record) {
      spdlog::warn(
        "MpObjectReference::GetTotalItemWeight of ({:x}): Record of form "
        "({}) is nullptr",
        GetFormId(), entry.baseId);
      return 0.f;
    }
    float weight = GetWeightFromRecord(record, GetParent()->GetEspmCache());
    if (!espm::utils::IsItem(record->GetType())) {
      spdlog::warn("Unsupported espm type {} has been detected, when "
                   "calculating overall weight.",
                   record->GetType().ToString());
    } else {
      spdlog::trace("Weight: {} for record of type {}", weight,
                    record->GetType().ToString());
    }
    return sum + entry.count * weight;
  };

  return std::accumulate(entries.begin(), entries.end(), 0.f, calculateWeight);
}
