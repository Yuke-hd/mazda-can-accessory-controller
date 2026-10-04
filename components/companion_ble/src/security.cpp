#include "companion_ble/security.hpp"

#include "companion_protocol/link_security.hpp"
#include "companion_protocol/pairing_window.hpp"
#include "companion_protocol/user_key_debouncer.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include <algorithm>
#include <array>
#include <cstdint>

// NimBLE's config store has no public header on the ESP-IDF include path; the
// ESP-IDF examples declare it the same way.
extern "C" void ble_store_config_init(void);

namespace companion_ble::internal {
namespace {

using companion_protocol::BondStorage;
using companion_protocol::EncryptedLink;
using companion_protocol::EncryptionVerdict;
using companion_protocol::LinkEncryption;
using companion_protocol::LinkSecurity;
using companion_protocol::PairingClock;
using companion_protocol::PairingWindow;
using companion_protocol::PeerBond;
using companion_protocol::ProtectedAccess;
using companion_protocol::RepeatPairingAction;
using companion_protocol::UserKeyDebouncer;
using companion_protocol::UserKeyLevel;

constexpr const char *kTag = "companion_ble";
constexpr std::uint32_t kUserKeySamplePeriodMs = 20U;
// With security level 1, NimBLE answers every Pairing Request with SMP
// `Command Not Supported` before any key exchange or bond store access; level
// 0 lets a pairing proceed. Re-encryption with a stored key ignores the level.
constexpr std::uint8_t kRejectPairing = 1U;
constexpr std::uint8_t kAcceptPairing = 0U;

// All policy state. Touched only on the NimBLE host task, except by
// configure_security() before that task exists.
struct SecurityState {
  PairingWindow window{BondStorage::Unavailable};
  UserKeyDebouncer debouncer{};
  UserKeySampler user_key_pressed{nullptr};
  LinkSecurity link{};
  std::uint16_t connection{BLE_HS_CONN_HANDLE_NONE};
  std::uint16_t disconnect_connection{BLE_HS_CONN_HANDLE_NONE};
  ble_store_write_fn *store_write{nullptr};
  ble_npl_callout user_key_timer{};
  ble_npl_callout window_timer{};
  ble_npl_callout drop_timer{};
  ble_npl_callout disconnect_timer{};
};

SecurityState state{};

PairingClock now() noexcept { return PairingClock{esp_timer_get_time() / 1000}; }

void arm(ble_npl_callout &timer, const PairingClock delay) noexcept {
  const auto milliseconds =
      static_cast<std::uint32_t>(std::max<PairingClock::rep>(delay.count(), 0));
  // One extra tick so a timer never fires before its deadline.
  (void)ble_npl_callout_reset(&timer, ble_npl_time_ms_to_ticks32(milliseconds) + 1U);
}

void arm_at(ble_npl_callout &timer, const PairingClock deadline) noexcept {
  arm(timer, deadline - now());
}

bool find_connection(const std::uint16_t connection, ble_gap_conn_desc &description) noexcept {
  return ble_gap_conn_find(connection, &description) == 0;
}

PeerBond stored_bond(const ble_addr_t &peer_identity) noexcept {
  ble_store_key_sec key{};
  key.peer_addr = peer_identity;
  ble_store_value_sec value{};
  return ble_store_read_peer_sec(&key, &value) == 0 && value.ltk_present != 0U ? PeerBond::Stored
                                                                               : PeerBond::None;
}

void delete_peer_keys(const ble_addr_t &peer_identity) noexcept {
  const int rc = ble_store_util_delete_peer(&peer_identity);
  if (rc != 0)
    ESP_LOGW(kTag, "could not delete a rejected pairing's keys (rc=%d)", rc);
}

void schedule_disconnect(const std::uint16_t connection) noexcept {
  if (connection == BLE_HS_CONN_HANDLE_NONE)
    return;
  state.disconnect_connection = connection;
  arm(state.disconnect_timer, PairingClock{0});
}

// Restarts or stops the unbonded-link drop timer from the link's deadline.
void rearm_drop_timer() noexcept {
  const auto deadline = state.link.drop_deadline();
  if (state.connection == BLE_HS_CONN_HANDLE_NONE || !deadline.has_value()) {
    ble_npl_callout_stop(&state.drop_timer);
    return;
  }
  arm_at(state.drop_timer, *deadline);
}

// Applies the window state to the security manager and the expiry timer.
void apply_window_state() noexcept {
  const PairingClock current = now();
  if (state.window.is_open(current)) {
    ble_hs_cfg.sm_sec_lvl = kAcceptPairing;
    arm_at(state.window_timer, *state.window.closes_at());
    return;
  }
  ble_hs_cfg.sm_sec_lvl = kRejectPairing;
  state.window.close();
  ble_npl_callout_stop(&state.window_timer);
}

void on_user_key_timer(ble_npl_event * /*event*/) {
  const UserKeyLevel level =
      state.user_key_pressed() ? UserKeyLevel::Pressed : UserKeyLevel::Released;
  if (state.debouncer.sample(level) && state.window.open(now())) {
    apply_window_state();
    ESP_LOGI(kTag, "pairing window open for %u s",
             static_cast<unsigned>(companion_protocol::kPairingWindowPeriod.count() / 1000));
  }
  arm(state.user_key_timer, PairingClock{kUserKeySamplePeriodMs});
}

void on_window_timer(ble_npl_event * /*event*/) {
  apply_window_state();
  if (!state.window.closes_at().has_value())
    ESP_LOGI(kTag, "pairing window closed");
}

void on_drop_timer(ble_npl_event * /*event*/) {
  if (state.connection == BLE_HS_CONN_HANDLE_NONE)
    return;
  if (state.link.must_drop(now())) {
    ESP_LOGI(kTag, "dropping a link without an accepted bond");
    schedule_disconnect(state.connection);
    return;
  }
  rearm_drop_timer();
}

void on_disconnect_timer(ble_npl_event * /*event*/) {
  const std::uint16_t connection = state.disconnect_connection;
  state.disconnect_connection = BLE_HS_CONN_HANDLE_NONE;
  if (connection != BLE_HS_CONN_HANDLE_NONE)
    (void)ble_gap_terminate(connection, BLE_ERR_REM_USER_CONN_TERM);
}

bool is_bond_record(const int object_type) noexcept {
  return object_type == BLE_STORE_OBJ_TYPE_OUR_SEC || object_type == BLE_STORE_OBJ_TYPE_PEER_SEC;
}

// Wraps the config store's write: pairing keys are stored only while the
// window is open, so no pairing outside it writes a bond to flash. Runs with
// the host lock held, so it only updates state.
int guarded_store_write(const int object_type, const ble_store_value *const value) {
  if (!is_bond_record(object_type))
    return state.store_write(object_type, value);
  if (!state.window.is_open(now()))
    return BLE_HS_EREJECT;
  const int rc = state.store_write(object_type, value);
  if (rc == 0)
    state.link.bond_keys_written();
  return rc;
}

// Bond capacity events. The stock handler deletes the oldest bond; that is
// allowed only inside the window, so outside it the event fails and the host
// rejects the pairing with every stored bond untouched.
int on_store_status(ble_store_status_event *const event, void *const argument) {
  if (!state.window.is_open(now()))
    return BLE_HS_EREJECT;
  return ble_store_util_status_rr(event, argument);
}

void handle_connect(const ble_gap_event &event) noexcept {
  if (event.connect.status != 0)
    return;
  state.connection = event.connect.conn_handle;
  state.disconnect_connection = BLE_HS_CONN_HANDLE_NONE;
  state.link.connected(now());
  rearm_drop_timer();
  ble_gap_conn_desc description{};
  if (find_connection(state.connection, description) &&
      stored_bond(description.peer_id_addr) == PeerBond::Stored) {
    // A bonded central re-encrypts promptly instead of on its first rejection.
    const int rc = ble_gap_security_initiate(state.connection);
    if (rc != 0)
      ESP_LOGW(kTag, "security request failed (rc=%d)", rc);
  }
}

void handle_disconnect(const ble_gap_event &event) noexcept {
  if (event.disconnect.conn.conn_handle != state.connection)
    return;
  state.connection = BLE_HS_CONN_HANDLE_NONE;
  state.disconnect_connection = BLE_HS_CONN_HANDLE_NONE;
  ble_npl_callout_stop(&state.drop_timer);
  ble_npl_callout_stop(&state.disconnect_timer);
}

void handle_encryption_change(const ble_gap_event &event) noexcept {
  const std::uint16_t connection = event.enc_change.conn_handle;
  if (connection != state.connection)
    return;
  ble_gap_conn_desc description{};
  const bool found = find_connection(connection, description);
  EncryptedLink link{};
  if (event.enc_change.status == 0 && found) {
    link.bonded = description.sec_state.bonded != 0U;
    link.key_size = description.sec_state.key_size;
    link.peer_bond = stored_bond(description.peer_id_addr);
  }
  switch (state.link.encrypted(link)) {
  case EncryptionVerdict::NewBondAccepted:
    state.window.close();
    apply_window_state();
    rearm_drop_timer();
    ESP_LOGI(kTag, "new bond stored; pairing window closed");
    return;
  case EncryptionVerdict::BondRestored:
    rearm_drop_timer();
    return;
  case EncryptionVerdict::RejectedDeleteKeys:
    if (found)
      delete_peer_keys(description.peer_id_addr);
    schedule_disconnect(connection);
    return;
  case EncryptionVerdict::Rejected:
    schedule_disconnect(connection);
    return;
  }
}

int handle_repeat_pairing(const ble_gap_event &event) noexcept {
  const std::uint16_t connection = event.repeat_pairing.conn_handle;
  ble_gap_conn_desc description{};
  if (connection == state.connection && find_connection(connection, description) &&
      state.window.repeat_pairing_action(now()) == RepeatPairingAction::DeleteOldBondAndRetry) {
    // Inside the window the old bond goes before key exchange.
    delete_peer_keys(description.peer_id_addr);
    state.link.accepted_pairing_pdu(now());
    rearm_drop_timer();
    return BLE_GAP_REPEAT_PAIRING_RETRY;
  }
  schedule_disconnect(connection);
  return BLE_GAP_REPEAT_PAIRING_IGNORE;
}

// Reached only for CCCD writes on an encrypted link (the stock check rejects
// unencrypted ones first).
void handle_authorize(ble_gap_event &event) noexcept {
  if (link_has_accepted_bond(event.authorize.conn_handle)) {
    event.authorize.out_response = BLE_GAP_AUTHORIZE_ACCEPT;
    return;
  }
  event.authorize.out_response = BLE_GAP_AUTHORIZE_REJECT;
  schedule_disconnect(event.authorize.conn_handle);
}

} // namespace

void configure_security(const PairingInputs &inputs) noexcept {
  state.window = PairingWindow{inputs.bond_storage};
  state.user_key_pressed = inputs.user_key_pressed;

  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1U;
  ble_hs_cfg.sm_mitm = 0U;
  ble_hs_cfg.sm_sc = 1U;
  ble_hs_cfg.sm_sc_only = 0U;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_sec_lvl = kRejectPairing;

  ble_store_config_init();
  state.store_write = ble_hs_cfg.store_write_cb;
  ble_hs_cfg.store_write_cb = guarded_store_write;
  ble_hs_cfg.store_status_cb = on_store_status;

  ble_npl_eventq *const queue = nimble_port_get_dflt_eventq();
  ble_npl_callout_init(&state.user_key_timer, queue, on_user_key_timer, nullptr);
  ble_npl_callout_init(&state.window_timer, queue, on_window_timer, nullptr);
  ble_npl_callout_init(&state.drop_timer, queue, on_drop_timer, nullptr);
  ble_npl_callout_init(&state.disconnect_timer, queue, on_disconnect_timer, nullptr);

  if (inputs.bond_storage == BondStorage::Unavailable) {
    ESP_LOGW(kTag, "no bond storage this boot; pairing is disabled");
    return;
  }
  if (state.user_key_pressed != nullptr)
    arm(state.user_key_timer, PairingClock{kUserKeySamplePeriodMs});
}

int handle_security_event(ble_gap_event &event) noexcept {
  switch (event.type) {
  case BLE_GAP_EVENT_CONNECT:
    handle_connect(event);
    return 0;
  case BLE_GAP_EVENT_DISCONNECT:
    handle_disconnect(event);
    return 0;
  case BLE_GAP_EVENT_ENC_CHANGE:
    handle_encryption_change(event);
    return 0;
  case BLE_GAP_EVENT_PARING_COMPLETE:
    // The central's last pairing PDU of an accepted pairing arrived.
    if (event.pairing_complete.conn_handle == state.connection &&
        event.pairing_complete.status == 0) {
      state.link.accepted_pairing_pdu(now());
      rearm_drop_timer();
    }
    return 0;
  case BLE_GAP_EVENT_REPEAT_PAIRING:
    return handle_repeat_pairing(event);
  case BLE_GAP_EVENT_AUTHORIZE:
    handle_authorize(event);
    return 0;
  default:
    return 0;
  }
}

bool pairing_window_open() noexcept { return state.window.is_open(now()); }

int check_protected_access(const std::uint16_t connection) noexcept {
  if (connection == BLE_HS_CONN_HANDLE_NONE)
    return 0;
  ble_gap_conn_desc description{};
  if (connection != state.connection || !find_connection(connection, description))
    return companion_protocol::att_error_code(ProtectedAccess::InsufficientAuthentication);
  const LinkEncryption encryption = description.sec_state.encrypted != 0U
                                        ? LinkEncryption::Encrypted
                                        : LinkEncryption::Unencrypted;
  const ProtectedAccess access =
      state.link.check_protected_access(now(), encryption, stored_bond(description.peer_id_addr));
  if (access == ProtectedAccess::InsufficientAuthenticationAndDisconnect)
    schedule_disconnect(connection);
  else
    rearm_drop_timer();
  return companion_protocol::att_error_code(access);
}

bool link_has_accepted_bond(const std::uint16_t connection) noexcept {
  return connection != BLE_HS_CONN_HANDLE_NONE && connection == state.connection &&
         state.link.has_accepted_bond();
}

bool clear_bonds() noexcept {
  std::array<ble_addr_t, MYNEWT_VAL(BLE_STORE_MAX_BONDS)> peers{};
  int count = 0;
  bool cleared =
      ble_store_util_bonded_peers(peers.data(), &count, static_cast<int>(peers.size())) == 0;
  // Each peer's deletion also removes its CCCDs. NimBLE's own identity keys
  // stay, so the controller keeps its address and IRK.
  for (int index = 0; index < count; ++index)
    cleared = ble_store_util_delete_peer(&peers[static_cast<std::size_t>(index)]) == 0 && cleared;
  state.link.revoke_bond();
  rearm_drop_timer();
  if (!cleared)
    ESP_LOGW(kTag, "clearing bonds failed; some bonds may remain");
  return cleared;
}

void disconnect_after_response(const std::uint16_t connection) noexcept {
  schedule_disconnect(connection);
}

} // namespace companion_ble::internal
