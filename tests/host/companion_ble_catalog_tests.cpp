#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "companion_ble/discovery_connection.hpp"
#include "companion_ble/output_catalog_characteristic.hpp"
#include "companion_ble/signal_catalog_characteristic.hpp"
#include "companion_protocol/device_info.hpp"
#include "companion_protocol/output_catalog_selector.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using namespace companion_ble::internal;
using namespace companion_protocol;

ByteView view(const std::string &text) {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}
std::string load(const char *path) {
  std::ifstream file(path, std::ios::binary);
  REQUIRE(file.good());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
struct Response {
  int error;
  std::vector<std::uint8_t> bytes;
};
Response read(const CharacteristicDefinition &definition, std::uint16_t connection = 7) {
  os_mbuf buffer{};
  ble_gatt_access_ctxt context{BLE_GATT_ACCESS_OP_READ_CHR, &buffer};
  const int result = definition.access(connection, 0, &context, definition.context);
  return {result, buffer.bytes};
}
int write(const CharacteristicDefinition &definition, std::uint32_t token, std::uint16_t offset,
          std::size_t size = 7, std::uint8_t format = 1) {
  os_mbuf buffer{{format, static_cast<std::uint8_t>(token), static_cast<std::uint8_t>(token >> 8U),
                  static_cast<std::uint8_t>(token >> 16U), static_cast<std::uint8_t>(token >> 24U),
                  static_cast<std::uint8_t>(offset), static_cast<std::uint8_t>(offset >> 8U)}};
  buffer.bytes.resize(size);
  ble_gatt_access_ctxt context{BLE_GATT_ACCESS_OP_WRITE_CHR, &buffer};
  return definition.access(7, 0, &context, definition.context);
}
std::uint32_t token_of(const Response &response) {
  REQUIRE(response.error == 0);
  REQUIRE(response.bytes.size() >= 11);
  return response.bytes[1] | (std::uint32_t{response.bytes[2]} << 8U) |
         (std::uint32_t{response.bytes[3]} << 16U) | (std::uint32_t{response.bytes[4]} << 24U);
}
} // namespace

TEST_CASE("output-only discovery serves arbitrary opaque bytes with a nonzero connection token") {
  const auto output = output_catalog_characteristic();
  CHECK(output.attribute == companion_ble::CompanionAttribute::OutputCatalog);
  CHECK(output.security == AttributeSecurity::EncryptedBonded);
  CHECK(output.flags == (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE));
  DiscoveryConnection connection;
  const std::string oversized_key(64, 'x');
  const vehicle_signals::SignalMetadata invalid_rows[]{
      {vehicle_signals::SignalId{1}, oversized_key}};
  const SignalCatalogExport no_signals{vehicle_signals::SignalCatalogView{invalid_rows}};
  REQUIRE_FALSE(no_signals.available());
  REQUIRE(bind_signal_catalog(no_signals) == 0);
  for (const std::string document : {std::string{"{\"outputs\":[]}"}, std::string{"not JSON"},
                                     std::string(1, '\0'), std::string(16384, 'x')}) {
    const OutputCatalogExport catalog{view(document)};
    REQUIRE(bind_output_catalog(catalog) == 0);
    const auto token = connection.connect(7);
    REQUIRE(token != 0);
    connect_output_catalog(7, token);
    connect_signal_catalog(7, token);
    CHECK(signal_catalog_token_for_connection(7) == 0);
    CHECK(read(signal_catalog_characteristic()).error == static_cast<int>(AttError::InvalidState));
    const auto info =
        encode_device_info({}, DeviceInfoExtensionStatus::Unusable, catalog.available());
    REQUIRE(info.has_value());
    CHECK(info->view()[1] == 2);
    CHECK(info->view()[info->size() - 2] == 1);
    CHECK(info->view()[info->size() - 1] == 2);
    const auto first = read(output);
    CHECK(token_of(first) == token);
    CHECK(first.bytes[7] == 0);
    CHECK(std::equal(first.bytes.begin() + 11, first.bytes.end(), document.begin(),
                     [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }));
    REQUIRE(connection.disconnect(7));
    disconnect_output_catalog(7);
    disconnect_signal_catalog(7);
    CHECK(read(output).error == static_cast<int>(AttError::InvalidState));
  }
}

TEST_CASE("real output callbacks serve generated reference bytes and stable Read Blob values") {
  const auto document = load(OUTPUT_CATALOG_PATH);
  const auto spec = load(OUTPUT_CATALOG_SPEC_PATH);
  const auto vectors = spec.find("## Synthetic reference vectors");
  REQUIRE(vectors != std::string::npos);
  const auto start = spec.find("```json\n", vectors) + 8;
  const auto end = spec.find("\n```", start);
  REQUIRE(end != std::string::npos);
  CHECK(document == spec.substr(start, end - start));
  const OutputCatalogExport catalog{view(document)};
  REQUIRE(bind_output_catalog(catalog) == 0);
  const auto endpoint = output_catalog_characteristic();
  DiscoveryConnection connection;
  const auto token = connection.connect(7);
  REQUIRE(token != 0);
  connect_output_catalog(7, token);
  std::vector<std::uint8_t> downloaded;
  for (std::size_t offset = 0; offset < document.size(); offset += 200) {
    REQUIRE(write(endpoint, token, static_cast<std::uint16_t>(offset)) == 0);
    const auto page = read(endpoint);
    REQUIRE(page.error == 0);
    CHECK(token_of(page) == token);
    CHECK(page.bytes[0] == 1);
    CHECK((page.bytes[5] | (page.bytes[6] << 8U)) == document.size());
    CHECK((page.bytes[7] | (page.bytes[8] << 8U)) == offset);
    // NimBLE invokes the callback again and applies each Read Blob offset.
    // Every invocation must supply exactly the same complete selected page.
    for (std::size_t blob = 0; blob < page.bytes.size(); blob += 22) {
      const auto reread = read(endpoint);
      CHECK(reread.error == 0);
      CHECK(reread.bytes == page.bytes);
    }
    downloaded.insert(downloaded.end(), page.bytes.begin() + 11, page.bytes.end());
  }
  CHECK(std::string(downloaded.begin(), downloaded.end()) == document);
  const auto selected = read(endpoint).bytes;
  CHECK(write(endpoint, token, 0, 6) == static_cast<int>(AttError::InvalidAttributeValueLength));
  CHECK(write(endpoint, 0, 65535, 7, 2) == static_cast<int>(AttError::UnsupportedOperation));
  CHECK(write(endpoint, 0, 65535) == static_cast<int>(AttError::InvalidState));
  CHECK(write(endpoint, token, static_cast<std::uint16_t>(document.size() + 1)) ==
        static_cast<int>(AttError::InvalidPdu));
  CHECK(read(endpoint).bytes == selected);
  CHECK(connection.connect(8) == 0);
  CHECK_FALSE(connection.disconnect(8));
  CHECK(read(endpoint, 8).error == static_cast<int>(AttError::InvalidState));
  CHECK(read(endpoint).bytes == selected);
  REQUIRE(connection.disconnect(7));
  disconnect_output_catalog(7);
  const auto next_token = connection.connect(7);
  CHECK(next_token != 0);
  CHECK(next_token != token);
  connect_output_catalog(7, next_token);
  CHECK(write(endpoint, token, 0) == static_cast<int>(AttError::InvalidState));
  CHECK(token_of(read(endpoint)) == next_token);
  CHECK(read(endpoint).bytes[7] == 0);
  CHECK(read(endpoint).bytes[8] == 0);
  CHECK(document == load(OUTPUT_CATALOG_PATH));
  REQUIRE(connection.disconnect(7));
  disconnect_output_catalog(7);
}

TEST_CASE("absent and oversized output documents stay unusable across connections") {
  const std::string oversized(16385, 'x');
  DiscoveryConnection connection;
  std::uint32_t previous = 0;
  for (const auto document : {ByteView{}, view(oversized)}) {
    const OutputCatalogExport catalog{document};
    REQUIRE(bind_output_catalog(catalog) == 0);
    const auto token = connection.connect(7);
    CHECK(token != 0);
    CHECK(token != previous);
    previous = token;
    connect_output_catalog(7, token);
    CHECK_FALSE(catalog.available());
    const auto endpoint = output_catalog_characteristic();
    CHECK(read(endpoint).error == static_cast<int>(AttError::InvalidState));
    CHECK(write(endpoint, token, 0) == static_cast<int>(AttError::InvalidState));
    REQUIRE(connection.disconnect(7));
    disconnect_output_catalog(7);
  }
}

TEST_CASE("both catalogs share the service token and reset their selectors independently") {
  const SignalCatalogExport signals{vehicle_signals::SignalCatalogView{}};
  const std::string document(300, 'x');
  const OutputCatalogExport outputs{view(document)};
  REQUIRE(bind_signal_catalog(signals) == 0);
  REQUIRE(bind_output_catalog(outputs) == 0);
  DiscoveryConnection connection;
  const auto first = connection.connect(7);
  connect_signal_catalog(7, first);
  connect_output_catalog(7, first);
  CHECK(token_of(read(signal_catalog_characteristic())) == first);
  CHECK(token_of(read(output_catalog_characteristic())) == first);
  CHECK(signal_catalog_token_for_connection(7) == first);
  CHECK(write(output_catalog_characteristic(), first, 200) == 0);
  CHECK(write(signal_catalog_characteristic(), first, signals.document_length()) == 0);
  CHECK(read(output_catalog_characteristic()).bytes[7] == 200);
  CHECK(read(signal_catalog_characteristic()).bytes[7] == signals.document_length());
  REQUIRE(connection.disconnect(7));
  disconnect_signal_catalog(7);
  disconnect_output_catalog(7);
  CHECK(signal_catalog_token_for_connection(7) == 0);
  const auto second = connection.connect(7);
  REQUIRE(second != first);
  connect_signal_catalog(7, second);
  connect_output_catalog(7, second);
  CHECK(read(output_catalog_characteristic()).bytes[7] == 0);
  CHECK(read(signal_catalog_characteristic()).bytes[7] == 0);
  CHECK(token_of(read(signal_catalog_characteristic())) == second);
  CHECK(token_of(read(output_catalog_characteristic())) == second);
  REQUIRE(connection.disconnect(7));
  disconnect_signal_catalog(7);
  disconnect_output_catalog(7);
}
