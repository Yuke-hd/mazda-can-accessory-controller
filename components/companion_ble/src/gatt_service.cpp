#include "companion_ble/gatt_service.hpp"

#include "companion_ble/nimble_uuid.hpp"
#include "companion_ble/security.hpp"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <array>

namespace companion_ble::internal {
namespace {

// NimBLE keeps pointers into these tables for the lifetime of the host, so
// they have static storage. Each table ends with a zeroed terminator entry.
std::array<ble_uuid128_t, kMaxCompanionCharacteristics> characteristic_uuids{};
std::array<ble_gatt_chr_def, kMaxCompanionCharacteristics + 1U> characteristic_table{};
const ble_uuid128_t service_uuid = nimble_uuid(CompanionAttribute::Service);
std::array<ble_gatt_svc_def, 2U> service_table{};
// The registered definitions, handed to guarded_access as its argument.
std::array<CharacteristicDefinition, kMaxCompanionCharacteristics> registered{
    CharacteristicDefinition{CompanionAttribute::Service},
    CharacteristicDefinition{CompanionAttribute::Service},
    CharacteristicDefinition{CompanionAttribute::Service},
    CharacteristicDefinition{CompanionAttribute::Service},
    CharacteristicDefinition{CompanionAttribute::Service}};

// The access trampoline of every protected characteristic. It applies the
// pairing policy itself instead of the stock encryption flags, so a link that
// is encrypted without an accepted bond is refused and every rejection reaches
// the unbonded-link timer.
int guarded_access(const std::uint16_t connection, const std::uint16_t attribute,
                   ble_gatt_access_ctxt *const context, void *const argument) {
  const auto &definition = *static_cast<const CharacteristicDefinition *>(argument);
  const int rejection = check_protected_access(connection);
  if (rejection != 0)
    return rejection;
  return definition.access(connection, attribute, context, definition.context);
}

ble_gatt_chr_def nimble_characteristic(const CharacteristicDefinition &definition,
                                       const ble_uuid128_t &uuid) noexcept {
  ble_gatt_chr_def characteristic{};
  characteristic.uuid = &uuid.u;
  characteristic.access_cb = definition.access;
  characteristic.arg = definition.context;
  characteristic.flags = definition.flags;
  characteristic.val_handle = definition.value_handle;
  if (definition.security == AttributeSecurity::EncryptedBonded) {
    characteristic.access_cb = guarded_access;
    characteristic.arg = const_cast<CharacteristicDefinition *>(&definition);
    // NimBLE handles CCCD writes without an access callback: the stock check
    // requires encryption (0x05 or 0x0F like the guard), and authorization
    // asks the pairing policy for an accepted bond.
    if ((definition.flags & (BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE)) != 0U)
      characteristic.flags |=
          BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
  }
  return characteristic;
}

// True when every definition names a distinct characteristic attribute.
bool attributes_are_distinct(
    const std::initializer_list<CharacteristicDefinition> characteristics) noexcept {
  for (auto first = characteristics.begin(); first != characteristics.end(); ++first) {
    if (first->attribute == CompanionAttribute::Service)
      return false;
    for (auto second = first + 1; second != characteristics.end(); ++second) {
      if (first->attribute == second->attribute)
        return false;
    }
  }
  return true;
}

} // namespace

int register_companion_service(
    const std::initializer_list<CharacteristicDefinition> characteristics) noexcept {
  if (characteristics.size() == 0U || characteristics.size() > kMaxCompanionCharacteristics ||
      !attributes_are_distinct(characteristics))
    return BLE_HS_EINVAL;

  std::size_t index = 0U;
  for (const auto &definition : characteristics) {
    registered[index] = definition;
    characteristic_uuids[index] = nimble_uuid(definition.attribute);
    characteristic_table[index] =
        nimble_characteristic(registered[index], characteristic_uuids[index]);
    ++index;
  }
  characteristic_table[index] = ble_gatt_chr_def{};

  service_table[0] = ble_gatt_svc_def{};
  service_table[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  service_table[0].uuid = &service_uuid.u;
  service_table[0].characteristics = characteristic_table.data();
  service_table[1] = ble_gatt_svc_def{};

  // GAP carries the device name; GATT carries Service Changed, which the
  // protocol requires for bonded clients that cache the attribute table.
  ble_svc_gap_init();
  ble_svc_gatt_init();
  int rc = ble_gatts_count_cfg(service_table.data());
  if (rc != 0)
    return rc;
  rc = ble_gatts_add_svcs(service_table.data());
  return rc;
}

} // namespace companion_ble::internal
