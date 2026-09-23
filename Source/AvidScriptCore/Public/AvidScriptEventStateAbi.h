#pragma once

// Concrete managed heap identities, never native addresses or Guest root maps.
namespace AvidScript::EventState::Abi
{
inline constexpr char SubscribeImport[] = "avid_event_state_subscribe_v1"; // (iiiiI)I: slot, generation, ordinal, type, object -> subscription
inline constexpr char ReadImport[] = "avid_event_state_read_v1"; // (i)I: concrete type -> current callback state
inline constexpr char CallbackExportSuffix[] = "_state_v1";
inline constexpr unsigned StateBytes = 12; // native storage: LE u32 type, u64 object
}
